// ===========================================================================
// Application: ゲームビルド（裏ジョブ）
// ---------------------------------------------------------------------------
// ApplicationProject.cpp にあった BuildGame / BakeTexturesIntoPak をここへ移し、
//   ・メインスレッド: PrepareBuildInput（設定・DXC・パスの取り込み）/ StartBuildGame / PollBuildGame
//   ・ワーカー: RunBuildPipeline（ファイル IO・pak 書き出し・テクスチャ事前生成の子プロセス待ち）
// に分けた。ワーカーは Application / PathResolver / エディタの状態を読まない（core/BuildJob.h の BuildInput だけ）。
// 同期版 BuildGame()（CLI --build / UI テスト）も同じ RunBuildPipeline を呼ぶ＝経路は 1 本。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/BuildJob.h"
#include "core/BundledFont.h"        // プロジェクトにフォントが無いとき同梱する日本語フォント
#include "core/VirtualGuard.h"       // 仮想入力モード中は ShellExecute を実行しない
#include "core/vfs/PakWriter.h"
#include "resource/AssetPrewarmer.h"

#include <chrono>
#include <fstream>
#include <iterator>

namespace dx12e
{
using namespace appdetail;

namespace
{
namespace fs = std::filesystem;

// 1 つのビルドの実行（ワーカーから呼ぶ。同期版はメインスレッドから）。
struct Pipeline
{
    const BuildInput& in;
    BuildProgress&    prog;
    std::string       err;

    bool Cancelled() const { return prog.cancelRequested.load(); }
    bool Fail(const std::string& m) { err = m; return false; }

    bool Run();
    bool CopyRuntime();
    bool PackAssets();
    void BakeTextures();
};

// ---- 段 1〜2: 出力先の用意とランタイムのコピー ---------------------------------------------
bool Pipeline::CopyRuntime()
{
    prog.SetStage(2);
    const fs::path runtimeSrc = in.exeDir / "GameRuntime.exe";
    std::error_code ec;
    if (!fs::exists(runtimeSrc, ec))
    {
        Logger::Error("GameRuntime.exe が見つかりません（{}）。先にエンジンをビルドしてください", runtimeSrc.string());
        return Fail("GameRuntime.exe が見つかりません。先にエンジンをビルドしてください");
    }
    fs::copy_file(runtimeSrc, in.outputDir / in.exeName, fs::copy_options::overwrite_existing, ec);
    if (ec) return Fail("GameRuntime.exe をコピーできません: " + ec.message());
    Logger::Info("Copied GameRuntime.exe -> {}", in.exeName);

    // 同じフォルダの .dll をすべて配布フォルダへ。
    // dxcompiler.dll(実行時シェーダーコンパイル専用、エディタのみ必要)だけ除外する。
    // ゲームは ShaderCompiler::LoadFromFile が game.pak から .cso を読むだけで実行時コンパイルは
    // 不要なため、同梱すると無駄に容量が増えるだけ(~25MB)。GameRuntime は dxcompiler.dll を
    // delay-load にしてある(ルート CMakeLists.txt)ので、同梱しなくても exe は正常起動する。
    // ※ dxil.dll は除外しない: これは D3D12 ランタイムが CreatePipelineState 時に
    // (Developer Mode OFF の環境で)DXIL署名検証のため内部で LoadLibrary するもので、
    // 我々のコードがリンクしているわけではない delay-load できない実行時依存。
    // 除外するとユーザー環境次第で PSO 生成が失敗するため、常に同梱する。
    // ★tracyclient.dll: 計測ビルド(cmake --preset windows-tracy)のツリーからゲームを
    //   ビルドしたときに、プロファイラのクライアントが配布物へ紛れ込むのを防ぐ。
    static const std::unordered_set<std::string> kDllExcludeList = { "dxcompiler.dll", "tracyclient.dll" };
    for (auto& entry : fs::directory_iterator(in.exeDir, ec))
    {
        if (Cancelled()) return false;
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".dll" && ext != ".DLL") continue;
        std::string lowerName = entry.path().filename().string();
        for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (kDllExcludeList.count(lowerName)) continue;
        std::error_code cec;
        fs::copy_file(entry.path(), in.outputDir / entry.path().filename(), fs::copy_options::overwrite_existing, cec);
        Logger::Info("Copied dll -> {}", entry.path().filename().string());
    }

    // ★エディタで作ったキー割り当てを配布物へ持っていく。
    //   input_bindings.json はプロジェクトルート直下にあり、pak に入るのは
    //   assets / scripts / shaders の 3 本だけなので、これまで **一度も同梱されていなかった**。
    //   ゲーム側の LoadActionBindings は exe の隣を見る（プレイヤーが後から書き換える
    //   ファイルなので pak ではなく生ファイルで正しい）。ここへ既定値として置いてやる。
    {
        std::error_code bec;
        if (!in.bindingsSrc.empty() && fs::exists(in.bindingsSrc, bec))
        {
            fs::copy_file(in.bindingsSrc, in.outputDir / in.bindingsSrc.filename(),
                          fs::copy_options::overwrite_existing, bec);
            if (bec) Logger::Warn("キー割り当ての同梱に失敗しました: {}", bec.message());
            else     Logger::Info("Copied input_bindings.json");
        }
    }

    // ★settings.json も同じ理由で同梱する。ゲームは起動時に PersistGet で
    //   render_instancing / render_clustered / 影の解像度 / CSM / テクスチャ圧縮 /
    //   video_* を読むのに、このファイルが配布物に無いので**全部エンジンの既定値**で
    //   動いていた（エディタで詰めた描画品質が一切届かない）。
    //   置き先は exe の隣＝プレイヤーが後から書き換える場所そのものなので、
    //   「作者が選んだ既定値」として置き、以後はプレイヤーの変更が上書きしていく。
    {
        std::error_code sec;
        if (!in.persistSrc.empty() && fs::exists(in.persistSrc, sec))
        {
            fs::copy_file(in.persistSrc, in.outputDir / "settings.json", fs::copy_options::overwrite_existing, sec);
            if (sec) Logger::Warn("設定の同梱に失敗しました: {}", sec.message());
            else     Logger::Info("Copied settings.json");
        }
    }
    return !Cancelled();
}

// ---- 段 3: game.pak へ assets / scripts / shaders を詰める ------------------------------------
bool Pipeline::PackAssets()
{
    // 詰める対象を先に数える（進捗バーの分母）。"." 始まりのフォルダ（.thumbcache / .texcache）は
    // エディタ専用のキャッシュで、出荷 pak に入れても実行時には参照されず容量を食うだけなので丸ごと除外する。
    struct Item { std::string abs, rel; };
    std::vector<Item> items;
    std::vector<std::string> packFailures;
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(in.assetsDir, ec), end; it != end; it.increment(ec))
        {
            // ★ここで break すると assets/ の**残り全部**を諦めたまま成功扱いになる。
            if (ec)
            {
                packFailures.push_back("assets の走査に失敗: " + ec.message());
                break;
            }
            if (it->is_directory(ec))
            {
                const std::string dirName = it->path().filename().string();
                if (!dirName.empty() && dirName[0] == '.')
                    it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            items.push_back({it->path().string(), it->path().lexically_relative(in.assetsDir).generic_string()});
        }
        // scripts/ 配下（"scripts/" プレフィックスを付けて格納）
        if (fs::exists(in.scriptsDir, ec))
            for (auto& entry : fs::recursive_directory_iterator(in.scriptsDir, ec))
            {
                if (!entry.is_regular_file()) continue;
                items.push_back({entry.path().string(),
                                 "scripts/" + entry.path().lexically_relative(in.scriptsDir).generic_string()});
            }
    }
    // shaders/ 配下の .cso（プロジェクトオーバーライドで再コンパイル済みなら baked .cso より優先する）
    std::vector<std::pair<std::string, const std::vector<uint8_t>*>> shaderBlobs;
    {
        std::error_code ec;
        if (fs::exists(in.shadersDir, ec))
            for (auto& entry : fs::recursive_directory_iterator(in.shadersDir, ec))
            {
                if (!entry.is_regular_file()) continue;
                const std::string relPath = "shaders/" + entry.path().lexically_relative(in.shadersDir).generic_string();
                auto ov = in.shaderOverrides.find(relPath);
                if (ov != in.shaderOverrides.end()) shaderBlobs.push_back({relPath, &ov->second});
                else items.push_back({entry.path().string(), relPath});
            }
    }

    prog.SetStage(3, {}, static_cast<int>(items.size() + shaderBlobs.size() + in.blobs.size()));

    vfs::PakWriter pak;
    if (!pak.Open((in.outputDir / "game.pak").string()))
    {
        Logger::Error("game.pak を書き込み用に開けません");
        return Fail("game.pak を書き込み用に開けません");
    }

    // ★以前は AddFile の戻り値を全部捨てていた。読めないファイルが 1 つあるだけで**そのアセットが欠けた pak が
    //   「ビルド完了」として出荷される**ので、拾って最後に失敗させる。
    int n = 0;
    for (const Item& it : items)
    {
        if (Cancelled()) return false;
        if (!pak.AddFile(it.abs, it.rel)) packFailures.push_back(it.rel + "  <- " + it.abs);
        prog.done = ++n;
        if ((n & 63) == 0) prog.SetDetail(std::to_string(n) + " / " + std::to_string(prog.total.load()) + " ファイル");
    }
    for (const auto& sb : shaderBlobs)
    {
        pak.AddBlob(sb.first, sb.second->data(), sb.second->size());
        prog.done = ++n;
    }
    for (const auto& b : in.blobs)
    {
        pak.AddBlob(b.rel, b.bytes.data(), b.bytes.size());
        prog.done = ++n;
    }

    // 1 件でも詰め損ねていたら「完了」と言わない（欠けた配布物を出さない）。
    if (!packFailures.empty())
    {
        std::string msg = "game.pak に入れられなかったファイルがあります。ビルドを中止しました:\n";
        for (std::size_t i = 0; i < packFailures.size() && i < 20; ++i) msg += "  - " + packFailures[i] + "\n";
        if (packFailures.size() > 20) msg += "  ...ほか " + std::to_string(packFailures.size() - 20) + " 件\n";
        Logger::Error("{}", msg);
        return Fail(msg);
    }
    if (Cancelled()) return false;

    // ブートマニフェスト（game.json の代替。GameRuntime は pak からこれを読む）。ビルド設定のタイトル/解像度を反映する。
    {
        // JSON 文字列エスケープ（" と \ のみ。タイトルは UTF-8 のまま格納）
        std::string titleEsc;
        for (char c : in.title)
        {
            if (c == '\\' || c == '"') titleEsc += '\\';
            titleEsc += c;
        }

        // ★UI の既定フォントを決めて manifest に書く。
        //   配布ゲームは今まで C:\Windows\Fonts の Yu Gothic / Meiryo を直読みしていて、
        //   その 2 つが入っていない環境（日本語 SKU 以外の素の Windows）では
        //   ImGui が ProggyClean（ASCII のみ）へ落ち、**日本語 UI が全部消える**。
        //   assets/fonts/ のフォント（dx12_install_font が置く Noto Sans JP 等。pak には既に入っている）を指す。
        //   無ければエンジン同梱の日本語フォント（Noto Sans JP のサブセット・SIL OFL 1.1）を pak へ足して使う。
        std::string uiFontRel;
        {
            std::error_code fec;
            const fs::path fontsDir = in.assetsDir / "fonts";
            if (fs::is_directory(fontsDir, fec))
            {
                std::vector<std::string> found;
                for (auto& de : fs::directory_iterator(fontsDir, fec))
                {
                    if (!de.is_regular_file()) continue;
                    std::string ext = de.path().extension().string();
                    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") found.push_back(de.path().filename().string());
                }
                std::sort(found.begin(), found.end());   // 選択を再現可能にする
                if (!found.empty()) uiFontRel = "fonts/" + found.front();
            }
            if (uiFontRel.empty())
            {
                // ライセンス文は pak の中と、出力フォルダの licenses/ の両方へ置く（OFL は再配布物にライセンス文を添えることを求める）。
                auto readAll = [](const std::string& path) {
                    std::ifstream f(fs::path(PathResolver::Utf8ToWide(path)), std::ios::binary);
                    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                };
                const std::vector<uint8_t> fontBytes = in.bundledFont.empty() ? std::vector<uint8_t>{} : readAll(in.bundledFont);
                if (!fontBytes.empty() && pak.AddBlob(bundled_font::kPakFontRel, fontBytes.data(), fontBytes.size()))
                {
                    uiFontRel = bundled_font::kPakFontRel;
                    const std::vector<uint8_t> lic = in.bundledFontLicense.empty() ? std::vector<uint8_t>{} : readAll(in.bundledFontLicense);
                    if (!lic.empty())
                    {
                        pak.AddBlob(bundled_font::kPakLicenseRel, lic.data(), lic.size());
                        std::error_code lec;
                        fs::create_directories(in.outputDir / "licenses", lec);
                        std::ofstream lout(in.outputDir / "licenses" / "NotoSansJP-OFL.txt", std::ios::binary);
                        lout.write(reinterpret_cast<const char*>(lic.data()), static_cast<std::streamsize>(lic.size()));
                        std::ofstream nout(in.outputDir / "licenses" / "README.txt", std::ios::binary);
                        nout << "This game bundles Noto Sans JP (subset), (c) 2014-2021 Adobe, licensed under the SIL Open Font License 1.1.\r\n"
                                "See NotoSansJP-OFL.txt. https://github.com/google/fonts/tree/main/ofl/notosansjp\r\n";
                    }
                    else
                        Logger::Warn("同梱フォントのライセンス文(LICENSE-NotoSansJP-OFL.txt)が見つかりません");
                    Logger::Info("UI 既定フォント: {}（エンジン同梱の Noto Sans JP・プロジェクトにフォントが無いため）", uiFontRel);
                }
                else
                    Logger::Warn("assets/fonts/ にフォントが無く、エンジン同梱の日本語フォントも見つかりません。"
                                 "配布ゲームは OS の Yu Gothic / Meiryo に頼るため、それらが無い環境では日本語 UI が "
                                 "表示されません（dx12_install_font で日本語対応フォントを入れてください）");
            }
            else
                Logger::Info("UI 既定フォント: {}", uiFontRel);
        }

        const std::string manifest =
            std::string("{\n") +
            "  \"title\": \"" + titleEsc + "\",\n" +
            "  \"startScene\": \"" + in.startSceneRel + "\",\n" +
            "  \"uiFont\": \"" + uiFontRel + "\",\n" +
            "  \"windowWidth\": " + std::to_string(in.winW) + ",\n" +
            "  \"windowHeight\": " + std::to_string(in.winH) + "\n" +
            "}\n";
        pak.AddBlob("__manifest__", reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
    }

    prog.SetDetail("pak を書き出し中");
    if (!pak.Finish(/*stripStrings=*/true))
    {
        Logger::Error("game.pak の書き出しに失敗しました");
        return Fail("game.pak の書き出しに失敗しました");
    }
    Logger::Info("Packed game.pak (startScene = {})", in.startSceneRel);
    return !Cancelled();
}

// ---- 段 4: テクスチャを BC 圧縮済みの形で game.pak へ焼く -----------------------------------
//   BC7 の CPU 圧縮は 1 枚数秒で、プレイヤーの PC の初回起動で全テクスチャ分を払うと 60〜90 秒掛かっていた。
//   キャッシュのキーから絶対パスを外したので（TextureLoader::NormalizeCacheKey）、ここで同じキャッシュを作って
//   pak の "texcache/" に入れておけば、初回から当たる。
//   やり方: 出来上がった Game を隠し窓で 1 回だけ走らせ（DX12E_TEXBAKE_*）、実際に読み込まれた
//   テクスチャの圧縮結果（エディタ側の assets/.texcache の .dds）を使われたものだけ pak へ追記する。
//   失敗しても配布物は正しく動く（プレイヤーの PC で従来どおり初回に圧縮されるだけ）。
void Pipeline::BakeTextures()
{
    prog.SetStage(4, "ゲームを非表示で 1 回実行して圧縮済みの画像を集めます", 0);
    const auto t0 = std::chrono::steady_clock::now();

    std::error_code ec;
    const fs::path texDir = in.assetsDir / ".texcache";
    fs::create_directories(texDir, ec);

    // 作業フォルダ（リスト・ゲームのユーザーデータ置き場）。出力フォルダを汚さない。
    const fs::path work = fs::temp_directory_path(ec) / ("dx12e_texbake_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(work, ec);
    fs::create_directories(work / "data", ec);
    const fs::path filesTxt = work / "files.txt";
    const fs::path listTxt  = work / "list.txt";

    // 先読みさせる JSON（シーン / プレハブ / マテリアル）。開始シーンは起動時に読まれるのでそれ以外も含めて全部。
    {
        std::ofstream f(filesTxt, std::ios::binary);
        for (fs::recursive_directory_iterator it(in.assetsDir, ec), end; it != end && !ec; it.increment(ec))
        {
            if (it->is_directory(ec))
            {
                const std::string dn = it->path().filename().string();
                if (!dn.empty() && dn[0] == '.') it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".json" && ext != ".dxmat" && ext != ".dxprefab" && ext != ".prefab") continue;
            f << it->path().lexically_relative(in.assetsDir).generic_string() << "\n";
        }
    }

    // 出力フォルダの「走らせる前」の状態を覚えて、ゲームが書いた物を後で消す（settings.json 等は元へ戻す）。
    std::unordered_set<std::string> before;
    std::unordered_map<std::string, std::string> keep;
    for (auto& e : fs::directory_iterator(in.outputDir, ec))
    {
        const std::string n = e.path().filename().string();
        before.insert(n);
        if (n == "settings.json" || n == "input_bindings.json")
        {
            std::ifstream f(e.path(), std::ios::binary);
            keep[n].assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
    }

    // 子プロセスへ渡す環境ブロック。このプロセスの環境変数を書き換えない（ワーカースレッドから呼ぶので、
    // SetEnvironmentVariable でプロセス全体を一時的に汚すとエディタの他のスレッドの子プロセスに漏れる）。
    const std::wstring extraVars[] = {
        L"DX12E_TEXBAKE_DIR=" + texDir.wstring(),
        L"DX12E_TEXBAKE_LIST=" + listTxt.wstring(),
        L"DX12E_TEXBAKE_FILES=" + filesTxt.wstring(),
        L"DX12E_DATA_DIR=" + (work / "data").wstring(),
    };
    std::wstring envBlock;
    if (LPWCH cur = GetEnvironmentStringsW())
    {
        for (const wchar_t* p = cur; *p; p += wcslen(p) + 1)
        {
            bool ours = false;
            for (const auto& v : extraVars)
            {
                const size_t eq = v.find(L'=');
                if (_wcsnicmp(p, v.c_str(), eq + 1) == 0) { ours = true; break; }
            }
            if (!ours) { envBlock += p; envBlock.push_back(L'\0'); }
        }
        FreeEnvironmentStringsW(cur);
    }
    for (const auto& v : extraVars) { envBlock += v; envBlock.push_back(L'\0'); }
    envBlock.push_back(L'\0');

    const std::wstring exePath = (in.outputDir / in.exeName).wstring();
    // 前面に出さない（--background=offscreen,notool）。ユーザーの操作を奪わない。
    std::wstring cmd = L"\"" + exePath + L"\" --background=offscreen,notool";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    const bool started = CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                        BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                        envBlock.data(), in.outputDir.wstring().c_str(), &si, &pi) != FALSE;

    bool finished = false, cancelled = false;
    if (!started)
    {
        Logger::Warn("テクスチャの事前生成を開始できません（初回起動時に圧縮されます）");
    }
    else
    {
        Logger::Info("テクスチャの事前生成: ゲームを非表示で実行します（初回のみ数十秒〜数分）");
        // 30 分で打ち切る（固まったゲームでビルドを止めない）。250ms ごとにキャンセルと経過時間を見る。
        for (;;)
        {
            const DWORD r = WaitForSingleObject(pi.hProcess, 250);
            if (r == WAIT_OBJECT_0) { finished = true; break; }
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (Cancelled()) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); cancelled = true; break; }
            if (sec > 30 * 60) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); break; }
            prog.SetDetail("経過 " + std::to_string(static_cast<int>(sec)) + " 秒（初回は圧縮に時間がかかります）");
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    // ゲームが出力フォルダに書いた物を片付ける
    for (auto& e : fs::directory_iterator(in.outputDir, ec))
    {
        const std::string n = e.path().filename().string();
        if (before.count(n)) continue;
        std::error_code rec;
        fs::remove_all(e.path(), rec);
    }
    for (auto& [n, bytes] : keep)
    {
        std::ofstream out(in.outputDir / n, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    // 使われたキャッシュを pak へ追記
    size_t added = 0, totalBytes = 0;
    if (finished && !cancelled && fs::exists(listTxt, ec))
    {
        prog.SetDetail("pak へ追記中");
        vfs::PakWriter pak;
        if (pak.OpenAppend((in.outputDir / "game.pak").string()))
        {
            std::ifstream lf(listTxt);
            std::string name;
            while (std::getline(lf, name))
            {
                while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
                if (name.empty()) continue;
                const fs::path src = texDir / name;
                std::error_code fec;
                const auto sz = fs::file_size(src, fec);
                if (fec) continue;
                if (pak.AddFile(src.string(), "texcache/" + name)) { ++added; totalBytes += static_cast<size_t>(sz); }
            }
            if (!pak.Finish(/*stripStrings=*/true))
                Logger::Error("テクスチャキャッシュの pak への追記に失敗しました。ビルドをやり直してください");
        }
    }
    else if (started && !cancelled)
    {
        Logger::Warn("テクスチャの事前生成が完了しませんでした（初回起動時に圧縮されます）");
    }

    fs::remove_all(work, ec);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Logger::Info("テクスチャの事前生成: {} 枚 ({:.0f} MB) を pak へ追記 ({:.1f} 秒)", added,
                 totalBytes / (1024.0 * 1024.0), sec);
}

bool Pipeline::Run()
{
    // 出力フォルダの準備。クリーンアップ（安全策: 既存が「前回ビルド or 空」でなければ消さずに中止＝ユーザーデータ保護）。
    prog.SetStage(1);
    if (fs::exists(in.outputDir))
    {
        std::error_code ec;
        const bool looksLikeBuild = fs::exists(in.outputDir / "Game.exe")
                                 || fs::exists(in.outputDir / in.exeName)
                                 || fs::exists(in.outputDir / "game.pak")
                                 || fs::is_empty(in.outputDir, ec);
        if (!looksLikeBuild)
        {
            Logger::Error("ビルドを中止しました: 出力先に過去のビルド以外のデータが存在します（保護のため中断）: {}",
                          in.outputDir.string());
            return Fail("出力先に過去のビルド以外のデータが存在します（保護のため中断しました）:\n" + in.outputDir.string());
        }
        fs::remove_all(in.outputDir, ec);
    }
    {
        std::error_code ec;
        fs::create_directories(in.outputDir, ec);
        if (ec) return Fail("出力フォルダを作れません: " + ec.message());
    }
    if (Cancelled()) return false;

    if (!CopyRuntime()) return false;
    if (!PackAssets()) return false;
    if (Cancelled()) return false;

    BakeTextures();
    if (Cancelled()) return false;

    // 起動用バッチ（GameRuntime は --game 不要: 常にゲームモード）。
    // （shaders は game.pak に暗号化封入済み＝プレーンな shaders/ は出力しない）
    prog.SetStage(5);
    {
        std::ofstream bat(in.outputDir / (in.productName + ".bat"));
        bat << "@echo off\n";
        bat << "\"" << in.exeName << "\"\n";
        bat << "pause\n";
    }
    Logger::Info("Game build complete: {}", in.outputDir.string());
    return true;
}

// ワーカーの入口 / 同期版の本体。終端の状態（成功・失敗・キャンセル）まで面倒を見る。
void RunBuildJob(const BuildInput& in, BuildProgress& prog)
{
    Pipeline p{in, prog};
    bool ok = false;
    try { ok = p.Run(); }
    catch (const std::exception& e)
    {
        Logger::Error("ビルド中に例外が発生しました: {}", e.what());
        p.err = std::string("ビルド中に例外が発生しました: ") + e.what();
    }
    if (!ok && prog.cancelRequested.load())
    {
        // 中途半端な出力を残さない（この出力フォルダは開始時に作り直した自分のもの）
        std::error_code ec;
        fs::remove_all(in.outputDir, ec);
        Logger::Info("ビルドをキャンセルしました");
        prog.Finish(BuildProgress::Cancelled);
        return;
    }
    prog.Finish(ok ? BuildProgress::Succeeded : BuildProgress::Failed, ok ? std::string() : p.err);
}

} // namespace

// ===========================================================================
//  メインスレッド側
// ===========================================================================

bool Application::PrepareBuildInput(BuildInput& in, std::string& err)
{
    namespace fs = std::filesystem;

    // --- 出力パスの非ASCII（日本語フォルダ名等）検出ガード（最優先）---
    // 出力先に非ASCII文字が含まれると、配布した Game.exe が起動時に std::filesystem の
    // UTF-8↔ANSI 誤変換で即クラッシュする（Windows error 1113 "No mapping for the Unicode
    // character..."）。原因不明の「ビルド成功 → 実行時クラッシュ」を防ぐため、ここで明示的に
    // 失敗させる。chosen は生の std::string（UTF-8でもACPでも日本語は >=0x80 を含む）なので
    // fs::path を経由せず（=ここで例外を出さず）バイト走査で判定する。
    {
        const std::string chosen =
            (m_editorCtx && !m_editorCtx->buildConfig.outputDir.empty())
                ? m_editorCtx->buildConfig.outputDir
                : PathResolver::BaseDir();
        bool nonAscii = false;
        for (unsigned char c : chosen) if (c >= 0x80) { nonAscii = true; break; }
        if (nonAscii)
        {
            Logger::Error("ビルドを中止しました: 出力先パスに非ASCII文字（日本語フォルダ名など）が"
                          "含まれています。このままビルドすると起動時にパスエラーで落ちるため、"
                          "半角英数のみのフォルダを指定してください。パス: {}", chosen);
            err = "出力フォルダのパスに日本語など非ASCII文字が含まれています。\n"
                  "このまま配布すると Game.exe が起動時にクラッシュします。\n"
                  "出力先を半角英数字のみのパスにしてください。\n\n" + chosen;
            return false;
        }
    }

    // ビルド出力先。ユーザーがビルド設定で選んだフォルダの中に「製品名_build」サブフォルダを作る。
    // 選んだフォルダ自体を出力先にして remove_all するとユーザーのデータを消す恐れがあるので必ずサブフォルダ化する。
    // 製品名 = ゲーム名をサニタイズ（英数・空白・_- のみ残す）。空なら "Game"。
    std::string productName;
    if (m_editorCtx)
        for (char c : std::string(m_editorCtx->buildConfig.title))
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ')
                productName += c;
    while (!productName.empty() && productName.back()  == ' ') productName.pop_back();
    while (!productName.empty() && productName.front() == ' ') productName.erase(productName.begin());
    if (productName.empty()) productName = "Game";
    in.productName = productName;
    in.exeName     = productName + ".exe";

    if (m_editorCtx && !m_editorCtx->buildConfig.outputDir.empty())
        in.outputDir = fs::path(m_editorCtx->buildConfig.outputDir) / (productName + "_build");
    else
        in.outputDir = fs::path(PathResolver::BaseDir()) / "build" / "game";

    // 開始シーンの相対パス。ビルド設定で明示指定があればそれを最優先。無ければ現在開いているシーンから求める。
    in.startSceneRel = "scenes/default.json";
    if (m_editorCtx && !m_editorCtx->buildConfig.startScene.empty())
    {
        in.startSceneRel = m_editorCtx->buildConfig.startScene;
    }
    else if (m_editorCtx && !m_editorCtx->currentScenePath.empty())
    {
        auto norm = [](std::string s) { for (auto& c : s) if (c == '\\') c = '/'; return s; };
        std::string full = norm(m_editorCtx->currentScenePath);
        std::string base = norm(PathResolver::AssetsDir());
        if (!base.empty() && full.rfind(base, 0) == 0)
            in.startSceneRel = full.substr(base.size());
        else
            in.startSceneRel = fs::path(full).lexically_relative(fs::path(base)).generic_string();

        if (in.startSceneRel.empty() || in.startSceneRel.rfind("..", 0) == 0)
        {
            Logger::Warn("現在のシーンが assets/ の外にあるため、既定の開始シーンを使用します: {}",
                         m_editorCtx->currentScenePath);
            in.startSceneRel = "scenes/default.json";
        }
    }
    if (m_editorCtx)
    {
        in.title = m_editorCtx->buildConfig.title[0] != '\0' ? std::string(m_editorCtx->buildConfig.title) : std::string("Game");
        in.winW = m_editorCtx->buildConfig.width;
        in.winH = m_editorCtx->buildConfig.height;
        in.openFolderAfter = m_editorCtx->buildConfig.openFolderAfterBuild;
    }
    else in.title = "Game";

    // 取り込むものの絶対パス（ビルド中にプロジェクトを開き直しても変わらない）
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        in.exeDir = fs::path(exePath).parent_path();
    }
    in.assetsDir  = fs::path(PathResolver::Utf8ToWide(PathResolver::AssetsDir()));
    in.scriptsDir = fs::path(PathResolver::Utf8ToWide(PathResolver::ScriptsDir()));
    in.shadersDir = fs::path(PathResolver::ShaderDirW());
    // 末尾の '/' 付きのパスは filename() が空になる。lexically_relative / directory_iterator のために正規化する。
    for (fs::path* p : {&in.assetsDir, &in.scriptsDir, &in.shadersDir})
        if (!p->empty() && !p->has_filename()) *p = p->parent_path();
    in.bindingsSrc = fs::path(ActionBindingsPath());
    in.persistSrc  = fs::path(PersistPath());
    in.bundledFont        = bundled_font::JapaneseFontPath();
    in.bundledFontLicense = bundled_font::JapaneseLicensePath();

    // プロジェクト独自シェーダー(上書き/自作)がある場合は実行時再コンパイルして反映する（DXC は ShaderManager /
    // GraphMaterialSystem の状態を触るのでメインスレッドで済ませ、バイト列だけワーカーへ渡す）。
    // コンパイル失敗があれば古い .cso を出荷せずビルド自体を中止する。
    std::error_code ec;
    {
        std::vector<std::string> shaderErrors;
        if (m_shaderManager && !m_shaderManager->RecompileAllForBuild(&shaderErrors))
        {
            std::string msg = "プロジェクトのシェーダーのコンパイルに失敗しました。ビルドを中止しました:\n";
            for (const auto& e : shaderErrors) msg += "  - " + e + "\n";
            Logger::Error("{}", msg);
            err = msg;
            return false;
        }

        if (m_shaderManager && fs::exists(in.shadersDir, ec))
        {
            for (auto& entry : fs::recursive_directory_iterator(in.shadersDir, ec))
            {
                if (!entry.is_regular_file()) continue;
                const std::string relPath = "shaders/" + entry.path().lexically_relative(in.shadersDir).generic_string();
                // プロジェクトオーバーライドで再コンパイル済みなら baked .cso より優先する。
                if (const std::vector<u8>* ov = m_shaderManager->TryGetOverride(entry.path().filename().wstring()))
                    in.shaderOverrides[relPath] = *ov;
            }
        }

        // カスタムシェーダー(Registry外、MeshRenderer::shaderPath 割当用)。
        // キー規約は Application::EnsureCustomPso のゲームモード分岐と一致させること。
        if (m_shaderManager)
        {
            for (const std::string& relPath : m_shaderManager->AllValidCustomRelPaths())
            {
                const std::vector<u8>* vs = m_shaderManager->GetCustomVsBytecode(relPath);
                const std::vector<u8>* ps = m_shaderManager->GetCustomPsBytecode(relPath);
                if (!vs || !ps) continue;
                in.blobs.push_back({"shaders/custom/" + relPath + "_VS.cso", *vs});
                in.blobs.push_back({"shaders/custom/" + relPath + "_PS.cso", *ps});
            }
        }

        // マテリアルグラフ（G2b）: assets 内の全グラフ材質（.dxmat の graph キー）の HLSL を DXC で DXIL にして pak へ焼く。
        //   ゲームモードは DXC を使わない（GraphMaterialSystem は pak の shaders/graph/*.cso から PSO を作る）。
        //   1 つでもコンパイルできなければビルドを止める（壊れたグラフを出荷しない。従来のカスタムシェーダーと同じ方針）。
        if (m_graphMaterials && m_graphMaterials->IsAvailable())
        {
            std::vector<GraphMaterialSystem::BakedBlob> graphBlobs;
            std::vector<std::string> graphErrors;
            if (!m_graphMaterials->BakeForBuild(graphBlobs, graphErrors))
            {
                std::string msg = "マテリアルグラフのシェーダーのコンパイルに失敗しました。ビルドを中止しました:\n";
                for (const auto& e : graphErrors) msg += "  - " + e.substr(0, 400) + "\n";
                Logger::Error("{}", msg);
                err = msg;
                return false;
            }
            for (auto& b : graphBlobs) in.blobs.push_back({b.relPath, std::move(b.bytes)});
            if (!graphBlobs.empty())
                Logger::Info("マテリアルグラフ: {} 個のシェーダーを pak へ焼きました", in.blobs.size());
        }
    }

    // 先読みスレッドが同じ .texcache を書いている最中に読ませない（テクスチャの事前生成が読む）。
    if (m_assetPrewarmer) m_assetPrewarmer->Stop();

    // 完了後に Explorer で開くため、最終的な出力先を控える
    if (m_editorCtx) m_editorCtx->lastBuildDir = in.outputDir.string();
    return true;
}

bool Application::IsBuildRunning() const
{
    return m_buildJob && m_buildJob->progress.state.load() == BuildProgress::Running;
}

bool Application::StartBuildGame(bool fromUi, std::string& errOut)
{
    if (IsBuildRunning())
    {
        errOut = "ビルドは既に実行中です";
        return false;
    }
    JoinBuildJob();   // 前回の終わったジョブを回収

    auto job = std::make_shared<BuildJob>();
    if (!PrepareBuildInput(job->input, errOut))
    {
        if (m_editorCtx) m_editorCtx->buildErrorMsg = errOut;
        return false;
    }
    job->input.fromUi = fromUi;
    job->progress.Begin(job->input.outputDir.string());
    job->progress.SetStage(1);
    if (m_editorCtx) { m_editorCtx->buildRunning = true; m_editorCtx->buildErrorMsg.clear(); }

    job->worker = std::thread([job]() {
        // エディタの操作感を落とさない（暗号化・圧縮は重い）
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        RunBuildJob(job->input, job->progress);
    });
    m_buildJob = std::move(job);
    Logger::Info("ゲームのビルドを裏で開始しました: {}", m_buildJob->input.outputDir.string());
    return true;
}

void Application::PollBuildGame()
{
    if (!m_buildJob) return;
    BuildProgress& p = m_buildJob->progress;
    const int st = p.state.load();
    if (st == BuildProgress::Running)
    {
        // UI 用に進みを写す（ステータスバー / ビルド設定窓）
        if (m_editorCtx)
        {
            m_editorCtx->buildFraction = p.Fraction();
            const int stage = p.stage.load();
            std::string txt = std::to_string(stage) + "/" + std::to_string(BuildProgress::kStageCount) + " " + BuildProgress::StageName(stage);
            { std::lock_guard<std::mutex> lk(p.mu); if (!p.detail.empty()) txt += "（" + p.detail + "）"; }
            m_editorCtx->buildStatusText = std::move(txt);
        }
        return;
    }
    if (st == BuildProgress::Idle) return;
    if (p.notified.exchange(true)) return;   // 通知は 1 回だけ

    if (m_buildJob->worker.joinable()) m_buildJob->worker.join();
    if (!m_editorCtx) return;
    m_editorCtx->buildRunning = false;

    if (st == BuildProgress::Succeeded)
    {
        m_editorCtx->buildCompleteFlash = 3.0f;
        m_editorCtx->Notify(ui::ToastKind::Success, "ゲームをビルドしました: " + m_editorCtx->lastBuildDir);
        if (m_buildJob->input.fromUi && m_buildJob->input.openFolderAfter && !m_editorCtx->lastBuildDir.empty())
            dx12e::guard::ShellExecuteGuarded(nullptr, "open", m_editorCtx->lastBuildDir.c_str(),
                                              nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (st == BuildProgress::Cancelled)
    {
        m_editorCtx->Notify(ui::ToastKind::Info, "ゲームのビルドをキャンセルしました");
    }
    else
    {
        std::string msg;
        { std::lock_guard<std::mutex> lk(p.mu); msg = p.error; }
        m_editorCtx->buildErrorFlash = 6.0f;
        m_editorCtx->errorMessage = msg.empty()
            ? "ビルドに失敗しました。\n詳細は dx12_engine.log を確認してください。"
            : msg;
        m_editorCtx->buildErrorMsg.clear();   // 次回ビルドへ持ち越さない
        m_editorCtx->errorFlash = 1.0f;       // トーストで通知（ビルド失敗の理由は長いので 8 秒出す）
    }
}

bool Application::CancelBuildGame()
{
    if (!IsBuildRunning()) return false;
    m_buildJob->progress.cancelRequested = true;
    Logger::Info("ゲームのビルドのキャンセルを要求しました");
    return true;
}

void Application::JoinBuildJob()
{
    if (!m_buildJob) return;
    if (m_buildJob->worker.joinable())
    {
        m_buildJob->progress.cancelRequested = true;   // 終了時など。走っていれば止めてから回収
        m_buildJob->worker.join();
    }
    if (m_editorCtx) m_editorCtx->buildRunning = false;
}

nlohmann::json Application::BuildStatusJson() const
{
    nlohmann::json j;
    if (!m_buildJob)
    {
        j["state"] = "idle";
        return j;
    }
    const BuildProgress& p = m_buildJob->progress;
    const int st = p.state.load();
    static const char* kNames[] = {"idle", "running", "succeeded", "failed", "cancelled"};
    j["state"] = kNames[std::clamp(st, 0, 4)];
    const int stage = p.stage.load();
    j["stage"] = stage;
    j["stageCount"] = BuildProgress::kStageCount;
    j["stageName"] = BuildProgress::StageName(stage);
    const bool indeterminate = (st == BuildProgress::Running) && p.total.load() == 0 && stage >= 4;
    j["indeterminate"] = indeterminate;
    j["pct"] = static_cast<int>(p.Fraction() * 100.0f + 0.5f);   // 不定の段は段の始まりの値
    j["done"] = p.done.load();
    j["total"] = p.total.load();
    j["elapsedSec"] = p.ElapsedSec();
    {
        std::lock_guard<std::mutex> lk(p.mu);
        j["detail"] = p.detail;
        j["outputDir"] = p.outputDir;
        if (!p.error.empty()) j["error"] = p.error;
    }
    return j;
}

// 同期版: CLI(--build)と UI テスト。呼び出し側のスレッドで最後まで走らせる。
bool Application::BuildGame()
{
    if (IsBuildRunning())
    {
        if (m_editorCtx) m_editorCtx->buildErrorMsg = "ビルドは既に実行中です";
        return false;
    }
    BuildInput in;
    std::string err;
    if (!PrepareBuildInput(in, err))
    {
        if (m_editorCtx) m_editorCtx->buildErrorMsg = err;
        return false;
    }
    BuildProgress prog;
    prog.Begin(in.outputDir.string());
    RunBuildJob(in, prog);
    const bool ok = prog.state.load() == BuildProgress::Succeeded;
    if (!ok && m_editorCtx)
    {
        std::lock_guard<std::mutex> lk(prog.mu);
        m_editorCtx->buildErrorMsg = prog.error;
    }
    return ok;
}

} // namespace dx12e
