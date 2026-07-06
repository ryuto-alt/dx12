#include "network/NetworkSystem.h"
#include "network/EnetTransport.h"
#include "core/Logger.h"

namespace dx12e {

NetworkSystem::NetworkSystem() = default;
NetworkSystem::~NetworkSystem() { Disconnect(); }

bool NetworkSystem::Host(u16 port, u32 maxPlayers, std::string& outError)
{
    if (m_role != NetRole::Offline) { outError = "already hosting/connected"; return false; }

    auto transport = std::make_unique<EnetTransport>();
    if (!transport->HostServer(port, maxPlayers, outError)) return false;

    m_transport = std::move(transport);
    m_role = NetRole::Host;
    m_localClientId = kServerClientId;
    m_peerToClient.clear();
    Logger::Info("NetworkSystem: hosting on port {}", port);

    if (m_eventBus)
    {
        EngineEvent e; e.name = "net.hostStarted";
        e.set("port", static_cast<double>(port));
        m_eventBus->Post(std::move(e));
    }
    return true;
}

bool NetworkSystem::Join(const std::string& ip, u16 port, std::string& outError)
{
    if (m_role != NetRole::Offline) { outError = "already hosting/connected"; return false; }

    auto transport = std::make_unique<EnetTransport>();
    // ハンドシェイクの許容時間は tickRate に関係ないので固定 5 秒(ENet内部タイムアウトの目安)。
    if (!transport->ConnectToServer(ip, port, 5000, outError)) return false;

    m_transport = std::move(transport);
    m_role = NetRole::Client;
    m_peerToClient.clear();
    Logger::Info("NetworkSystem: connecting to {}:{}...", ip, port);
    return true;
}

void NetworkSystem::Disconnect()
{
    if (m_role == NetRole::Offline) return;
    if (m_transport) m_transport->Disconnect();
    ResetState();

    if (m_eventBus)
    {
        EngineEvent e; e.name = "net.disconnected";
        m_eventBus->Post(std::move(e));
    }
}

void NetworkSystem::ResetState()
{
    m_transport.reset();
    m_role = NetRole::Offline;
    m_localClientId = kServerClientId;
    m_peerToClient.clear();
}

void NetworkSystem::PreSimUpdate(f32 /*dt*/)
{
    if (!m_transport) return;
    for (auto& ev : m_transport->Poll(0)) HandleTransportEvent(ev);
}

void NetworkSystem::PostSimUpdate(f32 /*dt*/)
{
    // フェーズ⑤でtick進行・スナップショット送受信・補間適用をここに追加する。
}

void NetworkSystem::HandleTransportEvent(const TransportEvent& ev)
{
    switch (ev.type)
    {
    case TransportEventType::Connected:
    {
        // フェーズ②の簡易割当(コメント参照)。
        ClientId id = static_cast<ClientId>(ev.peer & 0xFFFFu);
        m_peerToClient[ev.peer] = id;
        if (IsClient()) m_localClientId = id;
        Logger::Info("NetworkSystem: peer connected (clientId={})", id);
        if (m_eventBus)
        {
            EngineEvent e; e.name = "net.clientConnected";
            e.set("client", static_cast<double>(id));
            m_eventBus->Post(std::move(e));
        }
        break;
    }
    case TransportEventType::Disconnected:
    {
        auto it = m_peerToClient.find(ev.peer);
        ClientId id = (it != m_peerToClient.end()) ? it->second : ClientId{ 0 };
        if (it != m_peerToClient.end()) m_peerToClient.erase(it);
        Logger::Info("NetworkSystem: peer disconnected (clientId={})", id);
        if (m_eventBus)
        {
            EngineEvent e; e.name = "net.clientDisconnected";
            e.set("client", static_cast<double>(id));
            m_eventBus->Post(std::move(e));
        }
        break;
    }
    case TransportEventType::Data:
        // フェーズ③以降でパケット種別(先頭バイト=PacketType)ごとに分岐する。
        break;
    }
}

std::vector<NetworkSystem::PlayerInfo> NetworkSystem::Players() const
{
    std::vector<PlayerInfo> out;
    if (!m_transport) return out;
    out.reserve(m_peerToClient.size());
    for (auto& [peer, id] : m_peerToClient)
        out.push_back({ id, m_transport->GetRoundTripTimeMs(peer) });
    return out;
}

} // namespace dx12e
