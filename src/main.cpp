#include "core/Application.h"
#include "core/PathResolver.h"

#include <Windows.h>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <filesystem>
#include <nlohmann/json.hpp>

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
            for (const auto& ej : *entities)
            {
                std::string nm = ej.value("name", std::string{});
                if (nm.empty()) continue;
                if (!names.insert(nm).second) dups.insert(nm);
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
            for (const auto& ej : *entities)
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

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    try
    {
        bool gameMode  = false;
        bool buildMode = false;
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
            if (args.find("--build") != std::string::npos)
                buildMode = true;
            if (args.find("--editor") != std::string::npos)
                gameMode = false;  // 明示的にエディタ起動
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

        // exe の場所を基準に assets/shaders/scripts のパスを確定（配布 exe を別フォルダで動かすため）
        dx12e::PathResolver::Initialize(gameMode);

        dx12e::Application app;
        app.Initialize(hInstance, nCmdShow, gameMode);

        if (buildMode)
        {
            // ヘッドレスでゲームをビルドして終了
            app.BuildGameStandalone();
            app.Shutdown();
            return EXIT_SUCCESS;
        }

        app.Run();
        app.Shutdown();
    }
    catch (const std::exception& e)
    {
        // GUI アプリはコンソールが無いので、致命的エラーをファイルにも残す
        {
            std::ofstream f("dx12_crash.log", std::ios::trunc);
            if (f) f << "Fatal Error: " << e.what() << "\n";
        }
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
