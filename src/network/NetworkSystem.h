#pragma once
#include "engine/core/EventBus.h"
#include "network/NetworkConfig.h"
#include "network/Transport.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dx12e {

enum class NetRole : u8 { Offline, Host, Client };

// マルチプレイの中枢オーケストレーター。Scene/Graphics/entt に非依存
// (GPU不要・GameRuntimeでもリンクのみで動く。スポーン等シーンに触る処理は
// Application 側が Hooks 経由で注入する設計 — フェーズ④で追加予定)。
//
// Application::Update の Play 分岐から毎フレーム PreSimUpdate → (物理更新) → PostSimUpdate の順に
// 呼んでもらう想定。フェーズ②時点では両者ともトランスポートのポーリング止まりで、
// tick 進行・スナップショット送受信は後続フェーズ(⑤)で PostSimUpdate に追加する。
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

    // リッスンサーバーとして待ち受け開始。port/maxPlayers は明示指定
    // (Lua バインド側で「省略時は Config() の既定値を使う」解決を行う)。
    bool Host(u16 port, u32 maxPlayers, std::string& outError);
    // サーバーへ接続開始(非同期。実際の成立は net.clientConnected イベントで分かる)。
    bool Join(const std::string& ip, u16 port, std::string& outError);
    void Disconnect();

    // Application::Update の Play 分岐から毎フレーム呼ぶ。
    void PreSimUpdate(f32 dt);
    void PostSimUpdate(f32 dt);

    NetRole Role() const { return m_role; }
    bool IsServer() const { return m_role == NetRole::Host; }
    bool IsClient() const { return m_role == NetRole::Client; }
    bool IsConnected() const { return m_transport && m_transport->IsConnected(); }
    ClientId LocalClientId() const { return m_localClientId; }

    struct PlayerInfo { ClientId id; u32 rttMs; };
    std::vector<PlayerInfo> Players() const;

private:
    void HandleTransportEvent(const TransportEvent& ev);
    void ResetState();

    std::unique_ptr<ITransport> m_transport;
    NetworkConfig m_config;
    NetRole       m_role = NetRole::Offline;
    ClientId      m_localClientId = kServerClientId;
    EventBus*     m_eventBus = nullptr;

    // フェーズ②の簡易割当: PeerHandle の下位ビットをそのまま ClientId として使う。
    // フェーズ③で HELLO/WELCOME ハンドシェイクによる正式な clientId 割当に置き換える。
    std::unordered_map<PeerHandle, ClientId> m_peerToClient;
};

} // namespace dx12e
