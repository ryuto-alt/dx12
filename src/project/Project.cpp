#include "project/Project.h"
#include "core/Logger.h"

#include <fstream>
#include <filesystem>
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
        Logger::Error("Failed to save project: {}", path);
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
        Logger::Error("Failed to load project: {}", path);
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

void Project::CreateDefaultStructure(const ProjectInfo& info)
{
    namespace fs = std::filesystem;

    fs::create_directories(info.assetsDir + "scenes");
    fs::create_directories(info.assetsDir + "models");
    fs::create_directories(info.assetsDir + "textures");
    fs::create_directories(info.assetsDir + "audio/bgm");
    fs::create_directories(info.assetsDir + "audio/sfx");
    fs::create_directories(info.scriptsDir);

    // Default game.lua
    std::string luaPath = info.scriptsDir + "game.lua";
    if (!fs::exists(luaPath))
    {
        std::ofstream ofs(luaPath);
        ofs << "-- " << info.name << "\n\n";
        ofs << "function OnStart()\n";
        ofs << "    -- Called once when play mode starts\n";
        ofs << "end\n\n";
        ofs << "function OnUpdate(dt)\n";
        ofs << "    -- Called every frame\n";
        ofs << "end\n";
    }

    // 開始シーンは敢えて生成しない。
    // 初回ロード時に Application が Grid + 平行光源のスターターシーンを作って保存する。
    fs::create_directories(info.assetsDir + "scenes");

    Logger::Info("Created project structure: {}", info.rootDir);
}

} // namespace dx12e
