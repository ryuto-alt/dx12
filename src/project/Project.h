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
    std::string defaultScene;      // "scenes/main.json"
    std::string lastOpenedScene;   // 最後に開いたシーンのフルパス
    std::string engineVersion = "0.1.0";
    // 新規作成時のテンプレート: "empty" / "fps" / "tps"
    // CreateDefaultStructure が この値で開始シーン + game.lua を生成する。
    std::string templateId = "empty";

    // ---- 新規作成フォーム（ランチャー）で選んだ初期設定。作成時（CreateDefaultStructure）だけ使う ----
    // -1 = 触らない（settings.json を作らない = エンジンの既定のまま）。
    int  newShadowQuality = -1;   // 影の解像度 0..3 = 1024 / 2048 / 4096 / 8192（settings.json の shadow_quality）
    int  newVsync         = -1;   // 垂直同期 0 / 1（settings.json の video_vsync）
    bool newGitInit       = false;   // git init + .gitignore（git が無ければ黙って飛ばす）
};

class Project
{
public:
    static bool Save(const ProjectInfo& info, const std::string& path);
    static bool Load(const std::string& path, ProjectInfo& outInfo);
    static void CreateDefaultStructure(const ProjectInfo& info);
};

} // namespace dx12e
