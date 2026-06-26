#include "scene/SceneFlow.h"
#include "core/Logger.h"
#include "core/vfs/Vfs.h"

#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace dx12e
{
using json = nlohmann::json;

bool SceneFlow::LoadFromString(const std::string& jsonStr)
{
    json j;
    try { j = json::parse(jsonStr); }
    catch (const std::exception& e)
    {
        Logger::Warn("SceneFlow parse error: {}", e.what());
        return false;
    }

    m_start = j.value("start", "");
    if (j.contains("flow") && j["flow"].is_object())
    {
        for (auto it = j["flow"].begin(); it != j["flow"].end(); ++it)
        {
            Node n;
            n.next   = it.value().value("next", "");
            n.onFail = it.value().value("onFail", "");
            m_flow[it.key()] = n;
        }
    }
    m_loaded = true;
    Logger::Info("SceneFlow loaded: start={}, {} nodes", m_start, m_flow.size());
    return true;
}

bool SceneFlow::Load(const std::string& path)
{
    m_loaded = false;
    m_start.clear();
    m_flow.clear();

    // VFS 経由で読む（ゲームモード: pak 復号。エディタ: ディスク）。
    auto bytes = vfs::ReadAsset("sceneflow.json");
    if (!bytes.empty())
        return LoadFromString(std::string(bytes.begin(), bytes.end()));

    // ディスクフォールバック
    std::ifstream ifs(path);
    if (!ifs.is_open())
        return false;
    std::stringstream ss; ss << ifs.rdbuf();
    return LoadFromString(ss.str());
}

bool SceneFlow::Save(const std::string& path) const
{
    json j;
    j["start"] = m_start;
    json flow = json::object();
    for (const auto& [scene, node] : m_flow)
        flow[scene] = { {"next", node.next}, {"onFail", node.onFail} };
    j["flow"] = flow;

    std::ofstream ofs(path);
    if (!ofs.is_open())
    {
        Logger::Error("Failed to save sceneflow: {}", path);
        return false;
    }
    ofs << j.dump(2);
    Logger::Info("SceneFlow saved: {}", path);
    return true;
}

std::string SceneFlow::Next(const std::string& currentRel) const
{
    auto it = m_flow.find(currentRel);
    return (it != m_flow.end()) ? it->second.next : std::string{};
}

std::string SceneFlow::OnFail(const std::string& currentRel) const
{
    auto it = m_flow.find(currentRel);
    return (it != m_flow.end()) ? it->second.onFail : std::string{};
}

void SceneFlow::SetNext(const std::string& scene, const std::string& next)
{
    m_flow[scene].next = next;
}

void SceneFlow::SetOnFail(const std::string& scene, const std::string& onFail)
{
    m_flow[scene].onFail = onFail;
}

} // namespace dx12e
