#include "network/NetworkConfig.h"
#include "core/Logger.h"
#include "core/vfs/Vfs.h"

#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace dx12e {
using json = nlohmann::json;

bool NetworkConfig::LoadFromString(const std::string& jsonStr)
{
    json j;
    try { j = json::parse(jsonStr); }
    catch (const std::exception& e)
    {
        Logger::Warn("NetworkConfig の解析に失敗しました: {}", e.what());
        return false;
    }
    tickRate     = j.value("tickRate", tickRate);
    snapshotRate = j.value("snapshotRate", snapshotRate);
    maxPlayers   = j.value("maxPlayers", maxPlayers);
    defaultPort  = static_cast<u16>(j.value("defaultPort", static_cast<u32>(defaultPort)));
    return true;
}

bool NetworkConfig::Load(const std::string& path)
{
    // VFS 経由で読む(ゲームモード: pak 復号。エディタ: ディスク)。SceneFlow::Load と同じ流儀。
    auto bytes = vfs::ReadAsset("network.json");
    if (!bytes.empty())
        return LoadFromString(std::string(bytes.begin(), bytes.end()));

    std::ifstream ifs(path);
    if (!ifs.is_open())
        return false;   // 未作成(初回起動等)。呼び出し側は既定値のまま続行する。
    std::stringstream ss; ss << ifs.rdbuf();
    return LoadFromString(ss.str());
}

bool NetworkConfig::Save(const std::string& path) const
{
    json j;
    j["tickRate"]     = tickRate;
    j["snapshotRate"] = snapshotRate;
    j["maxPlayers"]   = maxPlayers;
    j["defaultPort"]  = defaultPort;

    std::ofstream ofs(path);
    if (!ofs.is_open())
    {
        Logger::Error("network.json の保存に失敗しました: {}", path);
        return false;
    }
    ofs << j.dump(2);
    Logger::Info("NetworkConfig saved: {}", path);
    return true;
}

} // namespace dx12e
