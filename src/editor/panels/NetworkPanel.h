#pragma once
#include "network/NetworkConfig.h"

#include <entt/entt.hpp>
#include <string>

namespace dx12e
{

class NetworkSystem;
class EditorContext;

// マルチプレイのエディタパネル。
// - RenderStatus: 状態モニタ(読み取り専用)。ロール/tick・複製数・接続一覧(RTT/送受信バイト概算)。
// - RenderSettings: assets/network.json の編集窓(tickRate/snapshotRate/maxPlayers/defaultPort)。
//   保存を押すまで NetworkSystem::Config() へは反映しない(誤操作で接続中に値が変わらないように)。
class NetworkPanel
{
public:
    void RenderStatus(NetworkSystem& net, entt::registry& reg, EditorContext& ctx);
    void RenderSettings(NetworkSystem& net, EditorContext& ctx, const std::string& assetsDir);

private:
    NetworkConfig m_staging;
    bool m_loaded = false;   // 初回表示時に net.Config() から m_staging を初期化したか
};

} // namespace dx12e
