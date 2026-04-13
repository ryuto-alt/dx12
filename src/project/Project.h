#pragma once

#include <string>
#include <vector>

namespace dx12e
{

struct ProjectInfo
{
    std::string name;          // "MyGame"
    std::string rootDir;       // "C:/Projects/MyGame"
    std::string assetsDir;     // rootDir + "/assets"
    std::string scriptsDir;    // rootDir + "/scripts"
    std::string shadersDir;    // rootDir + "/shaders" or build shaders
    std::string defaultScene;  // "scenes/main.json"
    std::string engineVersion = "0.1.0";
};

class Project
{
public:
    static bool Save(const ProjectInfo& info, const std::string& path);
    static bool Load(const std::string& path, ProjectInfo& outInfo);
    static void CreateDefaultStructure(const ProjectInfo& info);
};

} // namespace dx12e
