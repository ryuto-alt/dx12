#include "core/Application.h"
#include "core/PathResolver.h"
#include "core/Updater.h"
#include "core/SplashScreen.h"
#include "core/Version.h"
#include "core/vfs/Vfs.h"

#include <Windows.h>
#include <shellapi.h>   // CommandLineToArgvW（--net-client / --project の解析用）
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <filesystem>
#include <nlohmann/json.hpp>

#ifndef DX12_GAME_RUNTIME
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

// assets ルートを推定（パス祖先に "assets" があればそこ、無ければ scene の親の親）。
fs::path GuessAssetsDir(const fs::path& scenePath)
{
    fs::path cur = scenePath.parent_path();
    while (!cur.empty())
    {
        if (cur.filename() == "assets") return cur;
        fs::path up = cur.parent_path();
        if (up == cur) break;
        cur = up;
    }
    // 既定: assets/scenes/x.json → assets
    fs::path p = scenePath.parent_path();
    return p.has_parent_path() ? p.parent_path() : p;
}

// シーン JSON を読まずに参照グラフを検証する（Claude が edit→検証→修正を回せるように）。
// チェック: JSON 妥当性 / スクリプトファイル存在 / entity 参照プロパティの解決 /
//           Trigger の filter・action target 解決 / LoadScene/FadeToScene のシーンパス存在。
// 戻り値: エラーなし=0 / エラーあり=1。結果は validate_report.txt と親コンソールに出す。
int RunValidate(const std::string& scenePathStr)
{
    std::vector<std::string> errors, warnings, infos;
    fs::path scenePath(scenePathStr);
    fs::path assetsDir = GuessAssetsDir(scenePath);

    json root;
    {
        std::ifstream ifs(scenePath, std::ios::binary);
        if (!ifs) errors.push_back("scene file open failed: " + scenePath.string());
        else
        {
            try { ifs >> root; }
            catch (const std::exception& e) { errors.push_back(std::string("JSON parse error: ") + e.what()); }
        }
    }

    if (errors.empty())
    {
        const json* entities = nullptr;
        if (root.contains("entities") && root["entities"].is_array()) entities = &root["entities"];
        else if (root.is_array()) entities = &root;

        if (!entities) errors.push_back("no 'entities' array found");
        else
        {
            std::set<std::string> names, dups;
            {
                size_t i = 0;
                for (const auto& ej : *entities)
                {
                    // 型不一致(entities配列にobject以外が混ざる等)で検証ツール自体が
                    // 落ちないように、1エンティティ単位でエラー化して続行する
                    try
                    {
                        std::string nm = ej.value("name", std::string{});
                        if (!nm.empty() && !names.insert(nm).second) dups.insert(nm);
                    }
                    catch (const std::exception& e)
                    {
                        errors.push_back("entities[" + std::to_string(i) + "]: bad value: " + e.what());
                    }
                    ++i;
                }
            }
            for (const auto& d : dups)
                warnings.push_back("duplicate entity name: " + d + " (references become ambiguous)");

            auto checkRef = [&](const std::string& refName, const std::string& where)
            {
                if (refName.empty()) return;
                if (!names.count(refName))
                    errors.push_back("unresolved entity reference: \"" + refName + "\" (" + where + ")");
            };
            auto checkScene = [&](const std::string& rel, const std::string& where)
            {
                if (rel.empty()) return;
                std::error_code ec;
                if (!fs::exists(assetsDir / rel, ec))
                    warnings.push_back("scene path not found: " + rel + " (" + where + ")");
            };

            int scripts = 0, triggers = 0, emitters = 0;
            size_t entityIdx = 0;
            for (const auto& ej : *entities)
            {
                ++entityIdx;
                try
                {
                std::string nm = ej.value("name", std::string("?"));

                if (ej.contains("luaScript"))
                {
                    const auto& lsj = ej["luaScript"];
                    std::string sp = lsj.value("scriptPath", std::string{});
                    if (!sp.empty())
                    {
                        ++scripts;
                        std::error_code ec;
                        if (!fs::exists(assetsDir / sp, ec))
                            errors.push_back("script not found: " + sp + " (entity: " + nm + ")");
                    }
                    if (lsj.contains("props") && lsj["props"].is_array())
                    {
                        for (const auto& pj : lsj["props"])
                        {
                            if (pj.value("type", std::string{}) == "entity")
                            {
                                std::string v = (pj.contains("value") && pj["value"].is_string())
                                    ? pj["value"].get<std::string>() : std::string{};
                                checkRef(v, "entity prop \"" + pj.value("name", std::string{}) + "\" of " + nm);
                            }
                        }
                    }
                }

                if (ej.contains("trigger"))
                {
                    ++triggers;
                    const auto& tj = ej["trigger"];
                    checkRef(tj.value("filter", std::string{}), "trigger filter of " + nm);
                    if (tj.contains("actions") && tj["actions"].is_array())
                    {
                        for (const auto& aj : tj["actions"])
                        {
                            int type = aj.value("type", 0);
                            checkRef(aj.value("target", std::string{}), "trigger action target of " + nm);
                            if (type == 7 || type == 8)  // LoadScene / FadeToScene
                                checkScene(aj.value("str", std::string{}), "trigger action scene of " + nm);
                        }
                    }
                }

                if (ej.contains("particleEmitter")) ++emitters;
                }
                catch (const std::exception& e)
                {
                    errors.push_back("entities[" + std::to_string(entityIdx - 1) +
                                     "]: bad value: " + e.what());
                }
            }

            infos.push_back("entities=" + std::to_string(entities->size()) +
                            "  scripts=" + std::to_string(scripts) +
                            "  triggers=" + std::to_string(triggers) +
                            "  emitters=" + std::to_string(emitters));
        }
    }

    std::ostringstream out;
    out << "=== dx12 scene validate ===\n";
    out << "scene : " << scenePath.string() << "\n";
    out << "assets: " << assetsDir.string() << "\n";
    for (const auto& s : infos)    out << "[info]  " << s << "\n";
    for (const auto& s : warnings) out << "[warn]  " << s << "\n";
    for (const auto& s : errors)   out << "[ERROR] " << s << "\n";
    out << (errors.empty() ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    std::string text = out.str();

    { std::ofstream f("validate_report.txt", std::ios::trunc); if (f) f << text; }
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())
    {
        DWORD written = 0;
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        WriteConsoleA(h, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
    return errors.empty() ? 0 : 1;
}

} // namespace
#endif // !DX12_GAME_RUNTIME

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    try
    {
        bool gameMode  = false;
        bool buildMode = false;
        std::string buildProjectDir;  // --build <dir> で指定したプロジェクト（空=組み込み）
        std::string netClientJoin;    // --net-client <ip[:port]>（マルチプレイのテストクライアント起動）
        std::string netClientProject; // --project <dir>（--net-client と併用。開くプロジェクトルート）
#ifndef DX12_GAME_RUNTIME
        if (lpCmdLine)
        {
            std::string args(lpCmdLine);

            // --validate <scene.json>: ヘッドレスでシーンの参照グラフを検証して終了（GUI 起動しない）。
            size_t vp = args.find("--validate");
            if (vp != std::string::npos)
            {
                std::string rest = args.substr(vp + 10);  // "--validate" の後ろ
                size_t b = rest.find_first_not_of(" \t\"");
                std::string path;
                if (b != std::string::npos)
                {
                    size_t e = rest.find_last_not_of(" \t\"");
                    path = rest.substr(b, e - b + 1);
                }
                return RunValidate(path);
            }

            if (args.find("--game") != std::string::npos)
                gameMode = true;
            // --build [<projectDir>]: ヘッドレスでゲームをビルド。プロジェクトパスを
            // 渡すとそのプロジェクトを対象にする（GUI なしでビルド可能 = CLI から検証できる）。
            size_t bpos = args.find("--build");
            if (bpos != std::string::npos)
            {
                buildMode = true;
                std::string rest = args.substr(bpos + 7);  // "--build" の後ろ
                size_t b = rest.find_first_not_of(" \t\"");
                if (b != std::string::npos && rest[b] != '-')
                {
                    size_t e = rest.find_last_not_of(" \t\"");
                    buildProjectDir = rest.substr(b, e - b + 1);
                }
            }
            if (args.find("--editor") != std::string::npos)
                gameMode = false;  // 明示的にエディタ起動
        }

        // --net-client <ip[:port]> / --project <dir>: マルチプレイのテストクライアント起動(フェーズ⑨)。
        // プロジェクトパスに日本語/空白が入り得るため、lpCmdLine(ANSI)ではなく
        // Wide argv で取って UTF-8 化する（エンジン内部のパスは全て UTF-8）。
        {
            int argc = 0;
            if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
            {
                auto toUtf8 = [](const wchar_t* w) {
                    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                    std::string s(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
                    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
                    return s;
                };
                for (int i = 1; i < argc; ++i)
                {
                    if (wcscmp(argv[i], L"--net-client") == 0 && i + 1 < argc)
                        netClientJoin = toUtf8(argv[++i]);
                    else if (wcscmp(argv[i], L"--project") == 0 && i + 1 < argc)
                        netClientProject = toUtf8(argv[++i]);
                }
                LocalFree(argv);
            }
        }

        // 配布レイアウト判定: exe の隣に game.json があれば（引数無しの直起動でも）ゲームモード。
        // これでビルドした Game.exe をダブルクリックするとゲームが立ち上がる。
        if (!buildMode)
        {
            wchar_t buf[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            std::filesystem::path exeDir = std::filesystem::path(buf).parent_path();
            std::error_code ec;
            if (std::filesystem::exists(exeDir / "game.json", ec))
                gameMode = true;
        }
#endif // !DX12_GAME_RUNTIME

#ifdef DX12_GAME_RUNTIME
        // コンパイル時確約: GameRuntime は常にゲームモード。引数・設定ファイルで変化しない。
        (void)lpCmdLine;  // ゲームランタイムはコマンド引数を処理しない（C4100 回避）
        gameMode  = true;
        buildMode = false;
#endif

        // exe の場所を基準に assets/shaders/scripts のパスを確定（配布 exe を別フォルダで動かすため）
        dx12e::PathResolver::Initialize(gameMode);

#ifdef DX12_GAME_RUNTIME
        // game.pak をマウント（GameRuntime ブート時の必須手順）。失敗したら致命的エラー。
        {
            wchar_t _exeBuf[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, _exeBuf, MAX_PATH);
            std::filesystem::path _exeDir = std::filesystem::path(_exeBuf).parent_path();
            // MountPak expects UTF-8。path::string() は ACP(日本語環境=Shift-JIS)を返すため、
            // 非 ASCII フォルダ("新しいフォルダー"等)だと Utf8ToWide で化けて CreateFileW が失敗する。
            // u8string() で UTF-8 バイト列を保ったまま渡す。
            const std::u8string _pakU8 = (_exeDir / "game.pak").u8string();
            if (!dx12e::vfs::MountPak(std::string(_pakU8.begin(), _pakU8.end())))
                throw std::runtime_error("game.pak not found or corrupt");
        }
#endif

        // 配布版（エディタ）のみ: 起動時に GitHub の最新リリースを確認し、新しければ DL→更新して再起動する。
        // 更新を開始したら本体は即終了（更新バッチが上書き→再起動を担う）。開発ビルド・
        // オフライン・リリース無しの場合は何もせず通常起動する。
        // GameRuntime（配布ゲーム）はエンジンの自動更新を行わない（エンジンのリリースで
        // ゲームの exe が置き換わってしまうのを防ぐ）。
#ifndef DX12_GAME_RUNTIME
        // エディタ起動: 初期化が終わるまで枠なしスプラッシュ（Unity 風）を出す。
        // 専用スレッドで動くので、以降の同期処理（更新チェックの WinHTTP 数秒・
        // D3D12/シェーダ/アセット初期化）中も固まらずアニメし続ける。
        if (!gameMode && !buildMode)
        {
            dx12e::SplashScreen::Show(
                "DX12 Engine",
                std::string("v") + dx12e::kEngineVersion,
                dx12e::PathResolver::AssetsDir() + "editor/icons/logo.png");
            dx12e::SplashScreen::SetStatus("アップデートを確認中...");
        }

        // justUpdated（--updated）による1回スキップは廃止した。「毎回絶対に確認してほしい」という
        // 要求のため、更新直後の再起動を含め全ての起動で必ずチェックする（スプラッシュ画面が
        // チェック中もアニメし続けるので、数秒の同期待ちでも固まって見えない）。
        if (!buildMode && dx12e::Updater::RunStartupCheck())
        {
            dx12e::SplashScreen::Close();   // 更新適用へ（更新バッチが上書き→再起動する）
            return EXIT_SUCCESS;
        }
#endif

        dx12e::Application app;
#ifndef DX12_GAME_RUNTIME
        if (!netClientJoin.empty())
        {
            app.SetNetTestClientJoin(netClientJoin);
            app.SetNetTestProject(netClientProject);
        }
#endif
        app.Initialize(hInstance, nCmdShow, gameMode, nullptr, buildMode);

        if (buildMode)
        {
            // ヘッドレスでゲームをビルドして終了
#ifndef DX12_GAME_RUNTIME
            if (!buildProjectDir.empty())
                dx12e::PathResolver::SetProjectRoot(buildProjectDir);
#endif
            const bool ok = app.BuildGameStandalone();
            app.Shutdown();
            return ok ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        app.Run();
        app.Shutdown();
    }
    catch (const std::exception& e)
    {
        dx12e::SplashScreen::Close();   // 初期化中の例外でスプラッシュが残らないように
        // GUI アプリはコンソールが無いので、致命的エラーをファイルにも残す
        {
            std::ofstream f("dx12_crash.log", std::ios::trunc);
            if (f) f << "Fatal Error: " << e.what() << "\n";
        }
        // エラーメッセージは UTF-8（日本語含む）なので W 版で出す（A 版だと文字化けする）
        {
            const char* msg = e.what();
            int n = MultiByteToWideChar(CP_UTF8, 0, msg, -1, nullptr, 0);
            std::wstring wmsg(n > 0 ? static_cast<size_t>(n - 1) : 0, L'\0');
            if (n > 0) MultiByteToWideChar(CP_UTF8, 0, msg, -1, wmsg.data(), n);
            MessageBoxW(nullptr, wmsg.c_str(), L"致命的なエラー", MB_OK | MB_ICONERROR);
        }
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
