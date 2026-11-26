#pragma once
// MCP の応答に載せるシーンのパス。基準が違う 2 つの形を別のキーで返し、混ざらないようにする。
//   path        : assets からの相対（"scenes/x.json"）。open_scene / save_scene にそのまま渡せる。assets の外なら null。
//   projectPath : プロジェクトルートからの相対（"assets/scenes/x.json" / ".dx12/untitled/x.json"）。プロジェクトの外なら null。
// どちらも、プロジェクトの外の絶対パス（ホームのパスなど）は漏らさない。
#include <optional>
#include <string>

namespace mcpscene {

struct ScenePaths
{
    std::optional<std::string> path;
    std::optional<std::string> projectPath;
};

inline std::string NormSlash(std::string s)
{
    for (auto& c : s) if (c == '\\') c = '/';
    return s;
}

inline std::optional<std::string> RelativeTo(const std::string& fullNorm, const std::string& dirRaw)
{
    std::string d = NormSlash(dirRaw);
    if (d.empty()) return std::nullopt;
    if (d.back() != '/') d += '/';
    if (fullNorm.size() > d.size() && fullNorm.compare(0, d.size(), d) == 0) return fullNorm.substr(d.size());
    return std::nullopt;
}

// full: シーンの絶対パス（空 = 未保存の新規シーン）。既に相対なら assets 相対として扱う。
inline ScenePaths Resolve(const std::string& full, const std::string& assetsDir, const std::string& baseDir)
{
    ScenePaths r;
    if (full.empty()) return r;
    const std::string f = NormSlash(full);
    const bool absolute = (f.size() > 1 && f[1] == ':') || f[0] == '/';
    if (!absolute) { r.path = f; return r; }
    r.path        = RelativeTo(f, assetsDir);
    r.projectPath = RelativeTo(f, baseDir);
    return r;
}

// JSON のオブジェクト j（nlohmann::json）に pathKey / projectKey を足す。無いときは null。
template <class Json>
void AddTo(Json& j, const ScenePaths& p, const char* pathKey = "path", const char* projectKey = "projectPath")
{
    j[pathKey]    = p.path ? Json(*p.path) : Json(nullptr);
    j[projectKey] = p.projectPath ? Json(*p.projectPath) : Json(nullptr);
}

} // namespace mcpscene
