#include "core/Application.h"
#include "core/CrashHandler.h"
#include "core/PathResolver.h"
#include "core/Updater.h"
#include "core/UpdateWindow.h"   // --preview-update-ui（更新案内窓を窓なしで PNG に描く検証入口）
#include "core/ReleaseNotes.h"   // --write-release-notes（GitHub リリース本文を書き出す）
#include "core/SplashScreen.h"
#include "core/SplashPreview.h"
#include "core/Version.h"
#include "core/vfs/Vfs.h"
#include "core/DpiScale.h"
#include "gui/UiTestHarness.h"   // --ui-tests-skip（UI テストの除外指定）
#include "editor/EditorTheme.h"   // --theme-variant（エディタのアイデンティティ案。開発用）
#include "core/mcp/FleetGuard.h"   // --owner-pid / --idle-exit / --instance-id（フリート運用の自己終了）
#include "core/OffscreenShot.h"     // --size WxH（screenshot_final の既定の撮影解像度。純ロジック）
#include "project/Project.h"
#include "scene/ScenePartition.h"   // --validate: 分割保存のセルもつなげて検証する
#include "scene/SceneFormatV2.h"   // --validate: version 2 のシーンは既定値の省略を補ってから検証する

#include <Windows.h>
#include <shellapi.h>   // CommandLineToArgvW（--net-client / --project の解析用）
#include <string>
#include <stdexcept>   // ビルド健全性チェックの std::runtime_error
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
        // 分割保存のシーン（"parts"）はセルファイルもつなげて検証する
        if (errors.empty())
        {
            std::string perr;
            if (!dx12e::scenepart::MergePartsFromDisk(root, scenePath.string(), perr)) errors.push_back(perr);
        }
    }

    if (errors.empty())
    {
        dx12e::scenefmt::InflateScene(root);   // v1 は素通し。v2 は省略された既定値を足す（読み込み経路と同じ形で見る）
        const json* entities = nullptr;
        if (root.contains("entities") && root["entities"].is_array()) entities = &root["entities"];
        else if (root.is_array()) entities = &root;

        if (!entities) errors.push_back("no 'entities' array found");
        else
        {
            std::set<std::string> names, dups;
            // シーンに存在する guid。参照の正は guid なので、guid で解決できるものは
            // 名前がどうであれ生きている（名前だけ見ると嘘の「参照切れ」を出す）。
            std::set<std::string> guids;
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
                        if (ej.contains("guid") && ej["guid"].is_string())
                            guids.insert(ej["guid"].get<std::string>());
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

            // refGuid が空でなく、その guid がシーンに居るなら参照は生きている。
            // guid が 0/未設定/死んでいるときだけ名前で見る（実行時の解決順と同じ）。
            auto checkRef = [&](const std::string& refName, const std::string& refGuid,
                                const std::string& where)
            {
                if (!refGuid.empty() && guids.count(refGuid)) return;
                if (refName.empty()) return;
                if (!names.count(refName))
                    errors.push_back("unresolved entity reference: \"" + refName + "\" (" + where + ")");
            };
            auto jstr = [](const json& j, const char* key) -> std::string {
                return (j.contains(key) && j[key].is_string()) ? j[key].get<std::string>() : std::string{};
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
                                checkRef(v, jstr(pj, "valueGuid"),
                                         "entity prop \"" + pj.value("name", std::string{}) + "\" of " + nm);
                            }
                        }
                    }
                }

                if (ej.contains("materialAssets") && ej["materialAssets"].is_array())
                {
                    for (const auto& mj : ej["materialAssets"])
                    {
                        if (!mj.is_string()) continue;
                        std::string mp = mj.get<std::string>();
                        if (mp.empty()) continue;
                        std::error_code ec;
                        if (!fs::exists(assetsDir / mp, ec))
                            errors.push_back("material asset not found: " + mp + " (entity: " + nm + ")");
                    }
                }

                if (ej.contains("trigger"))
                {
                    ++triggers;
                    const auto& tj = ej["trigger"];
                    checkRef(tj.value("filter", std::string{}), jstr(tj, "filterGuid"),
                                 "trigger filter of " + nm);
                    if (tj.contains("actions") && tj["actions"].is_array())
                    {
                        for (const auto& aj : tj["actions"])
                        {
                            int type = aj.value("type", 0);
                            checkRef(aj.value("target", std::string{}), jstr(aj, "targetGuid"),
                                     "trigger action target of " + nm);
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

    // ★以前は無条件に AttachConsole/AllocConsole してから WriteConsoleA していた。
    //   WriteConsoleA は**コンソールハンドル専用**で、リダイレクト先（ファイル/パイプ）には
    //   書けずに黙って失敗する。つまり CI や `--validate x.json > out.txt` では
    //   **出力ゼロで exit 1 だけが返り、何が壊れているのか分からなかった**
    //   （内容は CWD の validate_report.txt にだけ残っていた）。
    //   標準出力が既に繋がっているならそこへ直接書き、繋がっていないときだけ
    //   親コンソールへ寄生する（ダブルクリック起動でも結果が読めるように）。
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == nullptr || hOut == INVALID_HANDLE_VALUE)
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())
            hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (hOut != nullptr && hOut != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(hOut, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
    return errors.empty() ? 0 : 1;
}

} // namespace
#endif // !DX12_GAME_RUNTIME

namespace {
// プロセスを Per-Monitor V2 の DPI aware にする（表示倍率 125% / 150% ... で Windows に引き伸ばされない）。
// 通常は resources/dx12.manifest が既に指定していて、ここは「マニフェストが効かない経路」の保険
// （二重指定は失敗するが無害。既に PMv2 ならフォールバックへ進まない）。
// どの窓（スプラッシュ・スワップチェイン・ImGui の別窓）よりも前に呼ぶこと。
void EnableProcessDpiAwareness()
{
    using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    using GetThreadCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)();
    using EqualFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto setCtx = reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setCtx)
        {
            if (setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
            // 失敗: マニフェストで設定済み（ERROR_ACCESS_DENIED）なら何もしなくてよい
            auto getThread = reinterpret_cast<GetThreadCtxFn>(GetProcAddress(user32, "GetThreadDpiAwarenessContext"));
            auto equal = reinterpret_cast<EqualFn>(GetProcAddress(user32, "AreDpiAwarenessContextsEqual"));
            if (getThread && equal && equal(getThread(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
        }
    }
    // Windows 8.1〜10 1511: Shcore の Per-Monitor（V1）。それも無ければシステム DPI aware。
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
    {
        using SetAwFn = HRESULT(WINAPI*)(int);
        if (auto setAw = reinterpret_cast<SetAwFn>(GetProcAddress(shcore, "SetProcessDpiAwareness")))
            if (SUCCEEDED(setAw(2 /*PROCESS_PER_MONITOR_DPI_AWARE*/))) { FreeLibrary(shcore); return; }
        FreeLibrary(shcore);
    }
    SetProcessDPIAware();
}
} // namespace

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    // 最初期: DPI aware（窓を 1 枚も作る前）。
    EnableProcessDpiAwareness();

    // ネイティブクラッシュ(アクセス違反等)でも原因が追えるよう、最初にクラッシュハンドラを仕込む。
    // クラッシュ時は CWD に dx12_crash.log(スタックトレース) + dx12_crash.dmp(ミニダンプ)が残る。
    dx12e::CrashHandler::Install();

    try
    {
        // ★ビルド健全性の自己検査。引数解析より前＝全経路（--validate / --write-version /
        //   GameRuntime 含む）で必ず通す。
        //
        // main.cpp の .obj が古い Application.h のまま再コンパイルされずに残ると、WinMain が
        // 確保するスタック領域より Application::Application() が書き込む量のほうが大きくなり、
        // ローカル変数 `Application app` の枠を溢れて /GS のスタッククッキーを踏み潰す。
        // 結果は 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN / __fastfail サブコード2) での即死。
        // __fastfail は SEH を迂回するので CrashHandler も catch も走らず、ログも dmp も
        // 一切残らない＝「何も言わずに落ちる」ため原因特定が極めて困難になる。
        // 2026-07-25 に実際にこれで起動不能になった（ninja の依存記録が #deps 0 に壊れており
        // Application.h を書き換えても main.cpp が再コンパイルされなかった）。
        // 2026-07-30 には ApplicationPipeline.cpp が同じ理由で古くなり、そちらは
        // 「m_rootSignature が壊れる」形（PSO 作成でアドレス 0x7 を読む）で出た。
        //
        // ★かつてここは main.cpp と Application.cpp の 2 本だけを比べていた。
        //   古くなったのがそのどちらでもない TU だったので、2026-07-30 は素通りした。
        //   今は Application を見る全 TU が Application.h の probe で自動登録される
        //   （＝TU を新設しても、ここに足しに来る必要はない）。
        //
        // 復旧手順: build ディレクトリを作り直す。あるいは
        //   cmake --build <builddir> -- -t deps
        // で "#deps 0" になっている .obj を探し、その .cpp を touch して再ビルドする。
        if (const std::size_t bad = dx12e::appdetail::ApplicationLayoutMismatch(); bad != 0)
        {
            throw std::runtime_error(
                "ビルドが壊れています: sizeof(Application) が TU によって "
                + std::to_string(dx12e::appdetail::ApplicationLayoutFirstSeen())
                + " バイトと " + std::to_string(bad)
                + " バイトに割れています（古い .obj の再コンパイル漏れ）。\n"
                  "build ディレクトリを作り直すか、"
                  "cmake --build <builddir> -- -t deps で \"#deps 0\" の .obj を探してください。");
        }

#ifndef DX12_GAME_RUNTIME
        // --splash-preview <dir> [--dpi-scale N] ...: 起動画面を窓なし・オフスクリーンで描いて PNG 連番を書き、即終了する
        // （検証用。エンジン本体は初期化しない = D3D12 デバイス不要。実窓を人の画面に出さずに見た目を確かめる入口）。
        {
            int pargc = 0;
            if (LPWSTR* pargv = CommandLineToArgvW(GetCommandLineW(), &pargc))
            {
                int previewExit = 0;
                bool handled = dx12e::RunSplashPreviewIfRequested(pargc, pargv, previewExit);
                if (!handled) handled = dx12e::RunUpdateUiPreviewIfRequested(pargc, pargv, previewExit);
                LocalFree(pargv);
                if (handled) return previewExit;
            }
        }
#endif

        bool gameMode  = false;
        bool buildMode = false;
        std::string buildProjectDir;  // --build <dir> で指定したプロジェクト（空=組み込み）
        std::string netClientJoin;    // --net-client <ip[:port]>（マルチプレイのテストクライアント起動）
        std::string netClientProject; // --project <dir>（開くプロジェクトルート。--net-client 併用または単独指定）
#ifndef DX12_GAME_RUNTIME
        // UI 自動テスト（エディタ専用。GameRuntime では引数を解釈しないので存在しない）
        bool uiTests       = false;   // --ui-tests（ImGuiTestEngine による UI 自動テストを有効化）
        bool uiTestsRunAll = false;   // --ui-tests-run-all（全テストを自動実行して終了コードを返す）
        bool uiTestsDeep   = false;   // --ui-tests-deep（超詳細診断だけを自動実行。UI 操作をほぼ伴わない）
        int  uiTestsSpeed  = 0;       // --ui-tests-speed=N（0=Fast 1=Normal 2=Cinematic）
        // --headless: 窓を出さずに MCP だけ開ける（CI / 並列実行 / 人の作業を邪魔しない検証）。
        // --mcp-port N: 待受ポートを固定（複数インスタンスを立てるとき必須）。
        // --scene <rel>: プロジェクトロード後に開くシーン（assets 相対）。
        bool headless = false;
        // --allow-autosave: ヘッドレスでもディスクへ書く（既定は読み取り専用）。
        bool headlessAllowSave = false;
        // --virtual-input: 実マウス/実キーボードを ImGui に渡さず OS のカーソルにも触れない（AI 操作用）。
        // --background[=offscreen|minimized|noactivate|hidden][,tool|notool]: 手前に出てこない静かな起動
        //   （仮想入力モードを含意。core/BackgroundMode.h）。
        bool virtualInput = false;
        // --show-whats-new[=<前回の版>]: 「更新内容」画面を（--background / --headless でも）出す検証用。
        //   前回の版を省くと直前のリリースから。表示済みの記録は書かない（fleet のエンジンは毎回「初回」扱いなので既定では出さない）。
        bool whatsNewForce = false;
        std::string whatsNewFrom;
        // --demo-update[=error]: 更新の流れの見本（本物の案内窓で 案内→ダウンロード→展開→適用 を模擬。何も書き換えない）。
        //   最後まで進むと、続けて「更新内容」画面を直前の版からの更新として出す。
        bool demoUpdate = false, demoUpdateFail = false;
        dx12e::BackgroundOptions bgOpt;
        std::string bgError;
        int  mcpPort  = 0;
        // フリート運用（複数エンジンを MCP サーバが起動・管理する）: 親が消えたら / 無操作が続いたら自分で終了する。
        unsigned long fleetOwnerPid = 0;
        double       fleetIdleExitMin = 0.0;
        std::string  fleetInstanceId;
        std::string startupScene;
#endif
#ifndef DX12_GAME_RUNTIME
        if (lpCmdLine)
        {
            std::string args(lpCmdLine);

            // --write-version <file>: 実行バイナリが自認するエンジン版をファイルに書いて終了。
            // installer/build.ps1 がパッケージ前に「exe の版 == Version.cpp の版」を検証するための
            // ヘッドレス出口（過去に一部 obj の再コンパイル漏れで版がズレた exe を配布し、
            // 自動更新が無限ループした事故の再発防止）。
            size_t wv = args.find("--write-version");
            if (wv != std::string::npos)
            {
                std::string rest = args.substr(wv + 15);  // "--write-version" の後ろ
                size_t b = rest.find_first_not_of(" \t\"");
                std::string path;
                if (b != std::string::npos)
                {
                    size_t e = rest.find_last_not_of(" \t\"");
                    path = rest.substr(b, e - b + 1);
                }
                if (path.empty()) return 1;
                std::ofstream f(path, std::ios::trunc);
                if (!f) return 1;
                f << dx12e::kEngineVersion;
                return 0;
            }

            // --write-release-notes <file>: 今の版（kEngineVersion）の GitHub リリース本文（Markdown）を UTF-8 で書いて終了。
            // 更新内容は core/ReleaseNotesData.inc が唯一の正（画面・ランチャー・GitHub 本文が同じデータから出る）。
            // 終了コード: 0=成功 / 1=書き込み失敗 / 2=データ不整合（先頭の版が kEngineVersion と違う・必須項目の欠け）。
            size_t wrn = args.find("--write-release-notes");
            if (wrn != std::string::npos)
            {
                std::string rest = args.substr(wrn + 21);
                size_t b = rest.find_first_not_of(" \t\"");
                std::string path;
                if (b != std::string::npos)
                {
                    size_t e = rest.find_last_not_of(" \t\"");
                    path = rest.substr(b, e - b + 1);
                }
                if (path.empty()) return 1;
                const auto& all = dx12e::relnotes::All();
                if (!dx12e::relnotes::Validate(all).empty()) return 2;
                const dx12e::relnotes::Release* cur = dx12e::relnotes::Find(all, dx12e::kEngineVersion);
                if (!cur || dx12e::relnotes::CompareVersions(all.front().version, dx12e::kEngineVersion) != 0) return 2;
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                if (!f) return 1;
                const std::string md = dx12e::relnotes::ToMarkdown(*cur, dx12e::kEngineName);
                f.write(md.data(), static_cast<std::streamsize>(md.size()));
                f.flush();
                return f ? 0 : 1;
            }

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

            // --new-project <dir> [--template empty|fps|tps|2d]:
            // ヘッドレスでテンプレートからプロジェクトを生成して終了（GUI 起動しない）。
            // Claude Code / CI がランチャーなしでプロジェクトを作れる入口。
            if (args.find("--new-project") != std::string::npos)
            {
                std::string dir, tmpl = "empty";
                int argc = 0;
                if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
                {
                    auto toUtf8 = [](const wchar_t* w) {
                        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                        std::string s(n > 0 ? static_cast<size_t>(n) : 0, '\0');
                        if (n > 0)
                        {
                            WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
                            s.pop_back();
                        }
                        return s;
                    };
                    for (int i = 1; i < argc; ++i)
                    {
                        if (wcscmp(argv[i], L"--new-project") == 0 && i + 1 < argc)
                            dir = toUtf8(argv[++i]);
                        else if (wcscmp(argv[i], L"--template") == 0 && i + 1 < argc)
                            tmpl = toUtf8(argv[++i]);
                    }
                    LocalFree(argv);
                }
                if (dir.empty()) return 1;

                std::filesystem::path root(dir);
                std::filesystem::create_directories(root);
                dx12e::ProjectInfo info;
                info.name         = root.filename().string();
                if (info.name.empty()) info.name = "MyGame";
                info.rootDir      = root.string();
                info.assetsDir    = (root / "assets").string() + "/";
                info.scriptsDir   = (root / "scripts").string() + "/";
                info.defaultScene = "scenes/main.json";
                info.templateId   = tmpl;
                dx12e::Project::CreateDefaultStructure(info);
                const bool ok = dx12e::Project::Save(
                    info, (root / (info.name + ".dx12proj")).string());
                return ok ? 0 : 1;
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
                    std::string s(n > 0 ? static_cast<size_t>(n) : 0, '\0');
                    if (n > 0)
                    {
                        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
                        s.pop_back(); // API が書き込んだ終端 NUL は std::string の長さに含めない
                    }
                    return s;
                };
                for (int i = 1; i < argc; ++i)
                {
                    if (wcscmp(argv[i], L"--net-client") == 0 && i + 1 < argc)
                        netClientJoin = toUtf8(argv[++i]);
                    else if (wcscmp(argv[i], L"--project") == 0 && i + 1 < argc)
                        netClientProject = toUtf8(argv[++i]);
                    else if (wcscmp(argv[i], L"--ui-tests") == 0)
                        uiTests = true;
                    else if (wcscmp(argv[i], L"--ui-tests-run-all") == 0)
                        { uiTests = true; uiTestsRunAll = true; }
                    else if (wcscmp(argv[i], L"--ui-tests-deep") == 0)
                        { uiTests = true; uiTestsRunAll = true; uiTestsDeep = true; }
                    else if (wcsncmp(argv[i], L"--ui-tests-speed=", 17) == 0)
                        { uiTests = true; uiTestsSpeed = _wtoi(argv[i] + 17); }
                    else if (wcscmp(argv[i], L"--ui-tests-skip") == 0 && i + 1 < argc)
                        dx12e::UiTestHarness::SetSkipList(toUtf8(argv[++i]));   // 例: build_game,launcher_screen（名前はカンマ区切り）
                    else if (wcsncmp(argv[i], L"--ui-tests-skip=", 16) == 0)
                        dx12e::UiTestHarness::SetSkipList(toUtf8(argv[i] + 16));
                    else if (wcscmp(argv[i], L"--ui-tests-only") == 0 && i + 1 < argc)
                        dx12e::UiTestHarness::SetOnlyList(toUtf8(argv[++i]));   // 例: hierarchy_reparent,asset_browser（指定したテストだけ走らせる）
                    else if (wcsncmp(argv[i], L"--ui-tests-only=", 16) == 0)
                        dx12e::UiTestHarness::SetOnlyList(toUtf8(argv[i] + 16));
                    else if (wcscmp(argv[i], L"--headless") == 0)
                        headless = true;
                    else if (wcscmp(argv[i], L"--no-splash-sound") == 0)
                        dx12e::SplashScreen::SetSoundEnabled(false);   // 起動音を鳴らさない（既定は鳴らす）
                    else if (wcscmp(argv[i], L"--allow-autosave") == 0)
                        headlessAllowSave = true;
                    else if (wcscmp(argv[i], L"--virtual-input") == 0)
                        virtualInput = true;
                    else if (wcscmp(argv[i], L"--demo-update") == 0 || wcscmp(argv[i], L"--demo-update=error") == 0)
                    {
                        demoUpdate = true;
                        demoUpdateFail = (argv[i][13] == L'=');
                    }
                    else if (wcscmp(argv[i], L"--show-whats-new") == 0 || wcsncmp(argv[i], L"--show-whats-new=", 17) == 0)
                    {
                        whatsNewForce = true;
                        if (argv[i][16] == L'=') whatsNewFrom = toUtf8(argv[i] + 17);
                    }
                    else if (wcscmp(argv[i], L"--background") == 0
                             || wcsncmp(argv[i], L"--background=", 13) == 0)
                    {
                        const std::string value = (argv[i][12] == L'=') ? toUtf8(argv[i] + 13) : std::string();
                        if (!dx12e::ParseBackgroundOption(value, bgOpt, bgError))
                        {
                            OutputDebugStringA((bgError + "\n").c_str());
                            bgOpt = dx12e::BackgroundOptions{};
                            bgOpt.mode = dx12e::BackgroundMode::Offscreen;   // 不明な値でも「静かに」は守る
                            bgOpt.toolWindow = true;
                        }
                    }
                    else if (wcscmp(argv[i], L"--dpi-scale") == 0 || wcsncmp(argv[i], L"--dpi-scale=", 12) == 0)
                    {
                        // 検証用: OS の表示倍率を無視してこの倍率で全体を描く（0.75〜3.0 / "150%" も可）。
                        std::string value;
                        if (argv[i][11] == L'=') value = toUtf8(argv[i] + 12);
                        else if (i + 1 < argc)   value = toUtf8(argv[++i]);
                        float v = 1.0f;
                        if (dx12e::dpi::ParseScale(value, v)) dx12e::dpi::SetOverride(v);
                        else OutputDebugStringA(("--dpi-scale の値が不正です（0.75〜3.0）: " + value + "\n").c_str());
                    }
                    else if (wcscmp(argv[i], L"--theme-variant") == 0 || wcsncmp(argv[i], L"--theme-variant=", 16) == 0)
                    {
                        // 開発用: エディタの見た目のアイデンティティ案。a | b | c | default（既定 = 現行。指定なしなら 1px も変わらない）。
                        std::string value;
                        if (argv[i][15] == L'=') value = toUtf8(argv[i] + 16);
                        else if (i + 1 < argc)   value = toUtf8(argv[++i]);
                        dx12e::theme::Variant tv = dx12e::theme::Variant::Default;
                        dx12e::theme::g_variantSwitchEnabled = true;   // 切替コマンド（コマンドパレット）を有効にする
                        if (dx12e::theme::ParseVariant(value, tv)) dx12e::theme::SetVariant(tv);
                        else OutputDebugStringA(("--theme-variant の値が不正です（a | b | c | default）: " + value + "\n").c_str());
                    }
                    else if (wcscmp(argv[i], L"--mcp-port") == 0 && i + 1 < argc)
                        mcpPort = _wtoi(argv[++i]);
                    else if (wcscmp(argv[i], L"--owner-pid") == 0 && i + 1 < argc)
                        fleetOwnerPid = wcstoul(argv[++i], nullptr, 10);
                    else if (wcscmp(argv[i], L"--idle-exit") == 0 && i + 1 < argc)
                        fleetIdleExitMin = _wtof(argv[++i]);
                    else if (wcscmp(argv[i], L"--instance-id") == 0 && i + 1 < argc)
                        fleetInstanceId = toUtf8(argv[++i]);
                    else if (wcscmp(argv[i], L"--scene") == 0 && i + 1 < argc)
                        startupScene = toUtf8(argv[++i]);
                    else if (wcscmp(argv[i], L"--size") == 0 || wcsncmp(argv[i], L"--size=", 7) == 0)
                    {
                        // Q2: screenshot_final の既定の撮影解像度（ビューポート / ウィンドウに依存しないオフスクリーン出力）。例: --size 1920x1080
                        std::string value;
                        if (argv[i][6] == L'=') value = toUtf8(argv[i] + 7);
                        else if (i + 1 < argc)  value = toUtf8(argv[++i]);
                        uint32_t sw = 0, sh = 0;
                        if (dx12e::offshot::ParseSize(value, sw, sh)
                            && dx12e::offshot::Validate(sw, sh, false) == dx12e::offshot::Verdict::Ok)
                        {
                            dx12e::offshot::Cli().w = sw;
                            dx12e::offshot::Cli().h = sh;
                        }
                        else OutputDebugStringA(("--size の値が不正です（例 1920x1080。1 辺 16〜8192・総画素 8192x4096 まで）: " + value + "\n").c_str());
                    }
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
        // ★--headless ではスプラッシュも出さない。窓を出さないための機能なのに
        //   起動のたびにロゴが前面へ出てきては意味が無い。
        // ★--background も出さない（人の画面に窓を出さないための機能）。読込時のスプラッシュも
        //   SetSuppressed で止める。
        // ★--headless / --build も止める。以前は --background しか SetSuppressed していなかったので、
        //   --headless --project で起動するツール（tools/bench/golden.mjs・engine.mjs など）が
        //   プロジェクト読込のスプラッシュ（ApplicationProject の ShowProjectLoad）を**人の画面の前面へ**出していた。
        //   環境変数 DX12E_NO_SPLASH=1 でも止められる（自動化から起動する道具の保険）。
        wchar_t noSplashEnv[8] = {};
        const bool noSplashByEnv = GetEnvironmentVariableW(L"DX12E_NO_SPLASH", noSplashEnv, 8) > 0
                                   && noSplashEnv[0] != L'0';
        if (bgOpt.Active() || headless || buildMode || noSplashByEnv)
            dx12e::SplashScreen::SetSuppressed(true);
        if (!gameMode && !buildMode && !headless && !bgOpt.Active() && !noSplashByEnv)
        {
            // --project 直開きは起動の直後にプロジェクトを開くので、進捗の計画にその段階を含める。
            dx12e::SplashScreen::ExpectProjectLoad(!netClientProject.empty());
            dx12e::SplashScreen::Show(
                dx12e::kEngineName,
                std::string("v") + dx12e::kEngineVersion,
                dx12e::PathResolver::AssetsDir() + "editor/icons/logo.png");
            dx12e::SplashScreen::SetStage(dx12e::splash::Stage::UpdateCheck);   // 更新確認（WinHTTP）は最大数秒: 微進みで止まって見せない
        }

        // justUpdated（--updated）による1回スキップは廃止した。「毎回絶対に確認してほしい」という
        // 要求のため、更新直後の再起動を含め全ての起動で必ずチェックする（スプラッシュ画面が
        // チェック中もアニメし続けるので、数秒の同期待ちでも固まって見えない）。
        // ★--background / --virtual-input では自動更新を確認しない（更新ダイアログや MessageBox が前面に出るため）。
        if (demoUpdate && !buildMode && !bgOpt.Active() && !headless)
        {
            if (dx12e::Updater::RunDemo(demoUpdateFail) && !whatsNewForce)
            {
                // 見本の更新が終わった体で、直前のリリースからの「更新内容」画面を出す
                const auto& rels = dx12e::relnotes::All();
                whatsNewForce = true;
                whatsNewFrom  = rels.size() >= 2 ? rels[1].version : std::string();
            }
        }
        else if (!buildMode && !bgOpt.Active() && !virtualInput && !headless && dx12e::Updater::RunStartupCheck())
        {
            dx12e::SplashScreen::Close();   // 更新適用へ（更新バッチが上書き→再起動する）
            return EXIT_SUCCESS;
        }
#endif

        dx12e::Application app;
#ifndef DX12_GAME_RUNTIME
        if (!netClientJoin.empty())
            app.SetNetTestClientJoin(netClientJoin);
        if (!netClientProject.empty())
            app.SetNetTestProject(netClientProject);   // --project 単独でもプロジェクトを開ける
        if (uiTests)
            app.EnableUiTests(uiTestsRunAll, uiTestsSpeed, uiTestsDeep);
        if (headless)
        {
            // ★--headless は --project が要る。ランチャーは人が押す前提の UI なので、
            //   窓を出さないまま出すと永久に何も起きない（＝MCP で繋いでも空のまま）。
            if (netClientProject.empty())
            {
                // Logger はまだこの TU に入っていないので MessageBox は使わず終了コードで返す。
                // （--headless は CLI/CI 用なので、出せるのは終了コードだけで十分）
                OutputDebugStringW(L"--headless には --project <dir> が要ります\n");
                return EXIT_FAILURE;
            }
            app.SetHeadless(true);
            app.SetHeadlessAllowSave(headlessAllowSave);
        }
        if (virtualInput) app.SetVirtualInput(true);
        if (whatsNewForce) app.SetWhatsNewForce(whatsNewFrom);
        if (bgOpt.Active()) app.SetBackground(bgOpt);   // 仮想入力モードも含意する
        if (mcpPort > 0)  app.SetMcpPort(mcpPort);
        dx12e::fleet::Instance().Configure(static_cast<uint32_t>(fleetOwnerPid), fleetIdleExitMin, fleetInstanceId);
        if (!startupScene.empty()) app.SetStartupScene(startupScene);
#endif
#ifdef DX12_GAME_RUNTIME
        // 配布ゲームが受け付ける唯一の引数: --background[=offscreen|minimized|noactivate|hidden][,tool|notool]
        // （UI 自動テストが配布ゲームの起動確認をする時に、人の画面へ窓を出さず前面も取らないため。
        //   引数なしの起動は従来どおり）。
        {
            int gargc = 0;
            if (LPWSTR* gargv = CommandLineToArgvW(GetCommandLineW(), &gargc))
            {
                for (int i = 1; i < gargc; ++i)
                {
                    if (wcscmp(gargv[i], L"--background") != 0 && wcsncmp(gargv[i], L"--background=", 13) != 0)
                        continue;
                    std::string value;
                    if (gargv[i][12] == L'=')
                        for (const wchar_t* p = gargv[i] + 13; *p; ++p) value += static_cast<char>(*p < 128 ? *p : '?');
                    dx12e::BackgroundOptions gbg;
                    std::string gerr;
                    if (!dx12e::ParseBackgroundOption(value, gbg, gerr))
                    {
                        gbg = dx12e::BackgroundOptions{};
                        gbg.mode = dx12e::BackgroundMode::Offscreen;
                        gbg.toolWindow = true;
                    }
                    app.SetBackground(gbg);
                }
                LocalFree(gargv);
            }
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
            const bool ok = app.BuildGameStandalone(buildProjectDir);
            app.Shutdown();
            return ok ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        app.Run();
#ifndef DX12_GAME_RUNTIME
        const int uiTestExit = app.UiTestExitCode();   // --ui-tests-run-all のときだけ非 0 になりうる
        app.Shutdown();
        if (uiTests && uiTestsRunAll)
            return uiTestExit;
#else
        app.Shutdown();
#endif
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
            std::wstring wmsg(n > 0 ? static_cast<size_t>(n) : 0, L'\0');
            if (n > 0)
            {
                MultiByteToWideChar(CP_UTF8, 0, msg, -1, wmsg.data(), n);
                wmsg.pop_back();
            }
            MessageBoxW(nullptr, wmsg.c_str(), L"致命的なエラー", MB_OK | MB_ICONERROR);
        }
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
