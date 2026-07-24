#include "project/Project.h"
#include "project/ProjectTemplates.h"
#include "core/Logger.h"

#include <fstream>
#include <filesystem>
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

    std::ofstream ofs(path);
    if (!ofs.is_open())
    {
        Logger::Error("プロジェクトの保存に失敗しました: {}", path);
        return false;
    }
    ofs << j.dump(2);
    Logger::Info("Project saved: {}", path);
    return true;
}

bool Project::Load(const std::string& path, ProjectInfo& outInfo)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
    {
        Logger::Error("プロジェクトの読み込みに失敗しました: {}", path);
        return false;
    }

    nlohmann::json j;
    ifs >> j;

    namespace fs = std::filesystem;
    fs::path projDir = fs::path(path).parent_path();

    outInfo.name             = j.value("name", j.value("title", "Untitled"));
    outInfo.engineVersion    = j.value("version", "0.1.0");
    // game.json は "startScene"、プロジェクトファイルは "defaultScene" を使う（両対応）
    outInfo.defaultScene     = j.value("startScene", j.value("defaultScene", "scenes/default.json"));
    outInfo.lastOpenedScene  = j.value("lastOpenedScene", "");
    outInfo.rootDir          = projDir.string();
    outInfo.assetsDir     = (projDir / j.value("assetsDir", "assets")).string() + "/";
    outInfo.scriptsDir    = (projDir / j.value("scriptsDir", "scripts")).string() + "/";

    Logger::Info("Project loaded: {} ({})", outInfo.name, path);
    return true;
}

// テンプレートの実体（シーン JSON / Lua コンポーネント / sceneflow）は
// ProjectTemplates.cpp が持つ。ここではフォルダ規約とファイル書き出しだけを行う。
void Project::CreateDefaultStructure(const ProjectInfo& info)
{
    namespace fs = std::filesystem;

    fs::create_directories(info.assetsDir + "scenes");
    fs::create_directories(info.assetsDir + "components");
    fs::create_directories(info.assetsDir + "prefabs");
    fs::create_directories(info.assetsDir + "shaders");
    fs::create_directories(info.assetsDir + "models");
    fs::create_directories(info.assetsDir + "textures");
    fs::create_directories(info.assetsDir + "audio/bgm");
    fs::create_directories(info.assetsDir + "audio/sfx");
    fs::create_directories(info.scriptsDir);

    const std::string tmpl = info.templateId.empty() ? "empty" : info.templateId;
    const auto& files = templates::GetFiles(tmpl);

    const char* mainSceneContent = nullptr;
    for (const auto& f : files)
    {
        fs::path outPath = fs::path(info.rootDir) / f.relPath;
        fs::create_directories(outPath.parent_path());
        std::ofstream ofs(outPath);
        if (!ofs.is_open())
        {
            Logger::Error("テンプレートファイルの書き出しに失敗しました: {}", outPath.string());
            continue;
        }
        if (std::string_view(f.relPath) == "scripts/game.lua")
            ofs << "-- " << info.name << "  (template: " << tmpl << ")\n\n";
        ofs << f.content;
        if (std::string_view(f.relPath) == "assets/scenes/main.json")
            mainSceneContent = f.content;
    }

    // .dx12proj の defaultScene が scenes/main.json 以外を指す場合も開始シーンが
    // ちゃんと存在するように、メインシーンを defaultScene のパスへも書いておく。
    if (mainSceneContent && !info.defaultScene.empty() && info.defaultScene != "scenes/main.json")
    {
        fs::path scenePath = fs::path(info.assetsDir) / info.defaultScene;
        fs::create_directories(scenePath.parent_path());
        std::ofstream ofs(scenePath);
        if (ofs.is_open())
            ofs << mainSceneContent;
    }

    Logger::Info("Created project structure ({} template, {} files): {}",
                 tmpl, files.size(), info.rootDir);
}

} // namespace dx12e
