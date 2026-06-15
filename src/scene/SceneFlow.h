#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace dx12e
{
// シーンの流れ（どのシーンの次がどれか）を保持する。assets/sceneflow.json と対応。
// 形式:
// {
//   "start": "scenes/title.json",
//   "flow": { "scenes/title.json": { "next": "scenes/level1.json", "onFail": "" }, ... }
// }
class SceneFlow
{
public:
    struct Node { std::string next; std::string onFail; };

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
    bool IsLoaded() const { return m_loaded; }

    const std::string& Start() const { return m_start; }
    void SetStart(const std::string& s) { m_start = s; }

    std::string Next(const std::string& currentRel) const;
    std::string OnFail(const std::string& currentRel) const;

    void SetNext(const std::string& scene, const std::string& next);
    void SetOnFail(const std::string& scene, const std::string& onFail);

    const std::unordered_map<std::string, Node>& Flow() const { return m_flow; }

private:
    bool        m_loaded = false;
    std::string m_start;
    std::unordered_map<std::string, Node> m_flow;
};

} // namespace dx12e
