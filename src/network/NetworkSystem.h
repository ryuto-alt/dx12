#pragma once
#include "engine/core/EventBus.h"
#include "network/NetworkConfig.h"
#include "network/Transport.h"

#include <entt/entt.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dx12e {

enum class NetRole : u8 { Offline, Host, Client };

// マルチプレイの中枢オーケストレーター。Scene/Graphics に非依存だが、NetworkIdentity の
// netId 割当/ベースライン構築のため entt::registry は直接触る(GPU非依存なので GameRuntime
// でもリンクのみで動く)。シーンロード等アプリ寄りの操作は Hooks 経由で Application が注入する。
//
// Application::Update の Play 分岐から毎フレーム PreSimUpdate → (物理更新) → PostSimUpdate の順に
// 呼んでもらう想定。フェーズ⑤でtick進行・スナップショット送受信を PostSimUpdate に追加する。
class NetworkSystem
{
public:
    NetworkSystem();
    ~NetworkSystem();
    NetworkSystem(const NetworkSystem&) = delete;
    NetworkSystem& operator=(const NetworkSystem&) = delete;

    void SetEventBus(EventBus* bus) { m_eventBus = bus; }
    void SetConfig(const NetworkConfig& cfg) { m_config = cfg; }
    const NetworkConfig& Config() const { return m_config; }

    // Application がシーン操作を注入するためのフック。null な関数は「未対応」として無視される。
    struct Hooks
    {
        std::function<std::string()> currentScenePath;             // 現在の assets 相対シーンパス
        std::function<void(const std::string&)> requestSceneLoad;  // シーンロード要求(pendingGameLoadPath等)
    };
    void SetHooks(Hooks h) { m_hooks = std::move(h); }

    // リッスンサーバーとして待ち受け開始。port/maxPlayers は明示指定
    // (Lua バインド側で「省略時は Config() の既定値を使う」解決を行う)。
    bool Host(u16 port, u32 maxPlayers, std::string& outError);
    // サーバーへ接続開始(非同期。実際の成立は net.connected イベントで分かる)。
    bool Join(const std::string& ip, u16 port, std::string& outError);
    void Disconnect();

    // Application::Update の Play 分岐から毎フレーム呼ぶ。reg はネットワーク複製対象
    // (NetworkIdentity)の走査に使う。
    void PreSimUpdate(f32 dt, entt::registry& reg);
    void PostSimUpdate(f32 dt, entt::registry& reg);

    NetRole Role() const { return m_role; }
    bool IsServer() const { return m_role == NetRole::Host; }
    bool IsClient() const { return m_role == NetRole::Client; }
    bool IsConnected() const { return m_transport && m_transport->IsConnected(); }
    ClientId LocalClientId() const { return m_localClientId; }

    struct PlayerInfo { ClientId id; u32 rttMs; };
    std::vector<PlayerInfo> Players() const;

private:
    void HandleTransportEvent(const TransportEvent& ev, entt::registry& reg);
    void ResetState();

    // ---- サーバー専用 ----
    void AssignNetIds(entt::registry& reg);                       // 未割当(_netId==0)のNetworkIdentityにid採番
    void SendWelcomeAndBaseline(PeerHandle peer, entt::registry& reg);
    void HandleSceneReady(PeerHandle peer);

    // ---- クライアント専用 ----
    void HandleWelcome(const std::vector<u8>& body);
    void HandleBaseline(const std::vector<u8>& body, entt::registry& reg);

    void SendTo(PeerHandle peer, PacketType type, const std::vector<u8>& body, bool reliable);

    std::unique_ptr<ITransport> m_transport;
    NetworkConfig m_config;
    NetRole       m_role = NetRole::Offline;
    ClientId      m_localClientId = kServerClientId;
    EventBus*     m_eventBus = nullptr;
    Hooks         m_hooks;

    ClientId m_nextClientId = 1;   // 0 はサーバー/ホスト予約
    NetId    m_nextNetId    = 1;   // 0 は「未割当」の意味で予約
    PeerHandle m_serverPeer = kInvalidPeer;   // クライアント視点: サーバーを指すピアハンドル

    std::unordered_map<PeerHandle, ClientId>  m_peerToClient;
    std::unordered_set<ClientId>              m_readyClients;   // SceneReady 済み(サーバー視点)
};

} // namespace dx12e
