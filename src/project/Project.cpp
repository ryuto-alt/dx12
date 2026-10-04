#include "project/Project.h"
#include "project/ProjectTemplates.h"
#include "project/LauncherLogic.h"   // PathFromUtf8（日本語パスを ANSI 解釈させない）
#include "project/GitIntegration.h"
#include "core/Logger.h"
#include "core/AtomicFileJson.h"

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <string_view>
#include <nlohmann/json.hpp>

namespace dx12e
{

bool Project::Save(const ProjectInfo& info, const std::string& path)
{
    nlohmann::json j;
    j["name"]             = info.name;
    j["version"]          = info.engineVersion;
    j["defaultScene"]     = info.defaultScene;
    j["lastOpenedScene"]  = info.lastOpenedScene;
    j["assetsDir"]        = "assets";
    j["scriptsDir"]       = "scripts";

    const auto wr = atomicfile::WriteFile(launcher::PathFromUtf8(path), j.dump(2), atomicfile::JsonVerifier());
    if (!wr)
    {
        Logger::Error("プロジェクトの保存に失敗しました: {} ({})", path, wr.error);
        return false;
    }
    Logger::Info("Project saved: {}", path);
    return true;
}

bool Project::Load(const std::string& path, ProjectInfo& outInfo)
{
    std::ifstream ifs(launcher::PathFromUtf8(path));
    if (!ifs.is_open())
    {
        Logger::Error("プロジェクトの読み込みに失敗しました: {}", path);
        return false;
    }

    nlohmann::json j;
    ifs >> j;

    namespace fs = std::filesystem;
    fs::path projDir = launcher::PathFromUtf8(path).parent_path();

    outInfo.name             = j.value("name", j.value("title", "Untitled"));
    outInfo.engineVersion    = j.value("version", "0.1.0");
    // game.json は "startScene"、プロジェクトファイルは "defaultScene" を使う（両対応）
    outInfo.defaultScene     = j.value("startScene", j.value("defaultScene", "scenes/default.json"));
    outInfo.lastOpenedScene  = j.value("lastOpenedScene", "");
    outInfo.rootDir          = launcher::PathToUtf8(projDir);
    outInfo.assetsDir     = launcher::PathToUtf8(projDir / j.value("assetsDir", "assets")) + "/";
    outInfo.scriptsDir    = launcher::PathToUtf8(projDir / j.value("scriptsDir", "scripts")) + "/";

    Logger::Info("Project loaded: {} ({})", outInfo.name, path);
    return true;
}

// テンプレートの実体（シーン JSON / Lua コンポーネント / sceneflow）は
// ProjectTemplates.cpp が持つ。ここではフォルダ規約とファイル書き出しだけを行う。
// ★パスは UTF-8 の std::string で来る。std::filesystem へ直接渡すと ANSI 扱いになって日本語フォルダが壊れるので、
//   必ず launcher::PathFromUtf8 を通す。
void Project::CreateDefaultStructure(const ProjectInfo& info)
{
    namespace fs = std::filesystem;
    using launcher::PathFromUtf8;

    std::error_code ec;
    const fs::path assets  = PathFromUtf8(info.assetsDir);
    const fs::path scripts = PathFromUtf8(info.scriptsDir);
    const fs::path root    = PathFromUtf8(info.rootDir);
    fs::create_directories(assets / "scenes", ec);
    fs::create_directories(assets / "components", ec);
    fs::create_directories(assets / "prefabs", ec);
    fs::create_directories(assets / "shaders", ec);
    fs::create_directories(assets / "models", ec);
    fs::create_directories(assets / "textures", ec);
    fs::create_directories(assets / "audio/bgm", ec);
    fs::create_directories(assets / "audio/sfx", ec);
    fs::create_directories(scripts, ec);

    const std::string tmpl = info.templateId.empty() ? "empty" : info.templateId;
    const auto& files = templates::GetFiles(tmpl);

    const char* mainSceneContent = nullptr;
    for (const auto& f : files)
    {
        fs::path outPath = root / f.relPath;
        fs::create_directories(outPath.parent_path(), ec);
        std::string body;
        if (std::string_view(f.relPath) == "scripts/game.lua")
            body += "-- " + info.name + "  (template: " + tmpl + ")\n\n";
        body += f.content;
        const auto wr = atomicfile::WriteFile(outPath, body);
        if (!wr)
        {
            Logger::Error("テンプレートファイルの書き出しに失敗しました: {} ({})", launcher::PathToUtf8(outPath), wr.error);
            continue;
        }
        if (std::string_view(f.relPath) == "assets/scenes/main.json")
            mainSceneContent = f.content;
    }

    // .dx12proj の defaultScene が scenes/main.json 以外を指す場合も開始シーンが
    // ちゃんと存在するように、メインシーンを defaultScene のパスへも書いておく。
    if (mainSceneContent && !info.defaultScene.empty() && info.defaultScene != "scenes/main.json")
    {
        fs::path scenePath = assets / PathFromUtf8(info.defaultScene);
        fs::create_directories(scenePath.parent_path(), ec);
        const auto wr = atomicfile::WriteFile(scenePath, mainSceneContent);
        if (!wr) Logger::Warn("開始シーンの書き出しに失敗しました: {}", wr.error);
    }

    // 新規作成フォームで選んだ描画設定。エンジンが読む settings.json（プロジェクトルート直下。数値だけの JSON）へ書く。
    // 選ばなかった項目は書かない＝エンジンの既定のまま。
    {
        nlohmann::json st = nlohmann::json::object();
        if (info.newShadowQuality >= 0) st["shadow_quality"] = std::clamp(info.newShadowQuality, 0, 3);
        if (info.newVsync >= 0)         st["video_vsync"]    = info.newVsync != 0 ? 1 : 0;
        if (!st.empty())
        {
            const auto wr = atomicfile::WriteFile(root / "settings.json", st.dump(2), atomicfile::JsonVerifier());
            if (!wr) Logger::Warn("settings.json の書き出しに失敗しました: {}", wr.error);
        }
    }

    // Git リポジトリの初期化（任意）。git が無い / 失敗しても作成自体は成功扱い。
    if (info.newGitInit && GitIntegration::IsGitAvailable())
    {
        const auto r = GitIntegration::Init(info.rootDir);
        if (!r.ok()) Logger::Warn("git init に失敗しました（プロジェクトの作成は続けます）: {}", r.output);
    }

    Logger::Info("Created project structure ({} template, {} files): {}",
                 tmpl, files.size(), info.rootDir);
}

} // namespace dx12e
