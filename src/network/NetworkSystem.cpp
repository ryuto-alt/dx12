#include "network/NetworkSystem.h"
#include "network/EnetTransport.h"
#include "network/NetBuffer.h"
#include "core/Logger.h"
#include "ecs/Components.h"

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
    m_nextClientId = 1;
    m_nextNetId = 1;
    m_peerToClient.clear();
    m_readyClients.clear();
    m_netToEntity.clear();
    m_pendingSpawns.clear();
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
    // ハンドシェイクの許容時間は固定5秒(ENet内部タイムアウトの目安)。
    if (!transport->ConnectToServer(ip, port, 5000, outError)) return false;

    m_transport = std::move(transport);
    m_role = NetRole::Client;
    m_peerToClient.clear();
    m_readyClients.clear();
    m_netToEntity.clear();
    m_pendingSpawns.clear();
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
    m_serverPeer = kInvalidPeer;
    m_peerToClient.clear();
    m_readyClients.clear();
    m_netToEntity.clear();
    m_pendingSpawns.clear();
}

void NetworkSystem::PreSimUpdate(f32 /*dt*/, entt::registry& reg)
{
    if (!m_transport) return;

    // サーバーは毎フレーム未割当(_netId==0)の NetworkIdentity を拾っておく
    // (エディタ/ゲームが実行中にコンポーネントを足すケースも取りこぼさない)。
    if (IsServer()) AssignNetIds(reg);

    for (auto& ev : m_transport->Poll(0)) HandleTransportEvent(ev, reg);
}

void NetworkSystem::PostSimUpdate(f32 /*dt*/, entt::registry& /*reg*/)
{
    // フェーズ⑤でtick進行・スナップショット送受信・補間適用をここに追加する。
}

void NetworkSystem::AssignNetIds(entt::registry& reg)
{
    for (auto [e, ni] : reg.view<NetworkIdentity>().each())
    {
        if (ni._netId == kInvalidNetId)
        {
            ni._netId = m_nextNetId++;
            m_netToEntity[ni._netId] = e;
        }
    }
}

void NetworkSystem::SendTo(PeerHandle peer, PacketType type, const std::vector<u8>& body, bool reliable)
{
    if (!m_transport) return;
    std::vector<u8> packet;
    packet.reserve(body.size() + 1);
    packet.push_back(static_cast<u8>(type));
    packet.insert(packet.end(), body.begin(), body.end());
    m_transport->Send(peer, NetChannel::Reliable, reliable, packet.data(), packet.size());
}

void NetworkSystem::BroadcastPacket(PacketType type, const std::vector<u8>& body, bool reliable, PeerHandle exclude)
{
    if (!m_transport) return;
    std::vector<u8> packet;
    packet.reserve(body.size() + 1);
    packet.push_back(static_cast<u8>(type));
    packet.insert(packet.end(), body.begin(), body.end());
    m_transport->Broadcast(NetChannel::Reliable, reliable, packet.data(), packet.size(), exclude);
}

void NetworkSystem::SendWelcomeAndBaseline(PeerHandle peer, entt::registry& reg)
{
    auto it = m_peerToClient.find(peer);
    const ClientId id = (it != m_peerToClient.end()) ? it->second : ClientId{ 0 };

    NetWriter welcome;
    welcome.WriteU16(id);
    welcome.WriteString(m_hooks.currentScenePath ? m_hooks.currentScenePath() : std::string{});
    SendTo(peer, PacketType::Welcome, welcome.Data(), true);

    NetWriter baseline;
    auto view = reg.view<NetworkIdentity>();
    const u32 count = static_cast<u32>(view.size());
    baseline.WriteU32(count);
    for (auto [e, ni] : view.each())
        baseline.WriteU32(ni._netId);
    SendTo(peer, PacketType::Baseline, baseline.Data(), true);

    Logger::Info("NetworkSystem: sent Welcome(clientId={}) + Baseline({} entities) to peer {}",
                 id, count, peer);
}

void NetworkSystem::HandleSceneReady(PeerHandle peer)
{
    auto it = m_peerToClient.find(peer);
    if (it == m_peerToClient.end()) return;
    m_readyClients.insert(it->second);
    Logger::Info("NetworkSystem: client {} is scene-ready", it->second);
    if (m_eventBus)
    {
        EngineEvent e; e.name = "net.clientReady";
        e.set("client", static_cast<double>(it->second));
        m_eventBus->Post(std::move(e));
    }
}

void NetworkSystem::HandleWelcome(const std::vector<u8>& body)
{
    try
    {
        NetReader r(body);
        m_localClientId = r.ReadU16();
        std::string scenePath = r.ReadString();
        Logger::Info("NetworkSystem: received Welcome (clientId={}, scene={})", m_localClientId, scenePath);

        if (m_hooks.requestSceneLoad && !scenePath.empty())
        {
            const std::string current = m_hooks.currentScenePath ? m_hooks.currentScenePath() : std::string{};
            if (current != scenePath) m_hooks.requestSceneLoad(scenePath);
        }

        if (m_eventBus)
        {
            EngineEvent e; e.name = "net.connected";
            e.set("client", static_cast<double>(m_localClientId));
            m_eventBus->Post(std::move(e));
        }
    }
    catch (const NetReadError&)
    {
        Logger::Warn("NetworkSystem: Welcome パケットの解析に失敗しました");
    }
}

void NetworkSystem::HandleBaseline(const std::vector<u8>& body, entt::registry& reg)
{
    std::vector<NetId> ids;
    try
    {
        NetReader r(body);
        const u32 count = r.ReadU32();
        ids.reserve(count);
        for (u32 i = 0; i < count; ++i) ids.push_back(r.ReadU32());
    }
    catch (const NetReadError&)
    {
        Logger::Warn("NetworkSystem: Baseline パケットの解析に失敗しました");
        return;
    }

    // サーバーと同じシーンを同じロード順で構築済みである前提で、view の反復順を
    // そのまま対応させる(既にロード済みでないと数が合わない=シーン未ロード/不一致)。
    auto view = reg.view<NetworkIdentity>();
    if (view.size() != ids.size())
    {
        Logger::Warn("NetworkSystem: Baseline の件数不一致(受信{} / 手元{})。"
                     "シーンロードが完了していないか、サーバーと異なるシーンの可能性があります",
                     ids.size(), view.size());
    }

    size_t i = 0;
    for (auto [e, ni] : view.each())
    {
        if (i >= ids.size()) break;
        ni._netId = ids[i++];
        m_netToEntity[ni._netId] = e;
    }

    Logger::Info("NetworkSystem: Baseline 適用完了({}件)", i);

    // Baseline 適用が完了した(できる限り)ので、サーバーへ準備完了を通知する。
    if (m_serverPeer != kInvalidPeer)
        SendTo(m_serverPeer, PacketType::SceneReady, {}, true);
}

void NetworkSystem::HandleSpawn(const std::vector<u8>& body)
{
    try
    {
        NetReader r(body);
        PendingSpawn sp;
        sp.netId      = r.ReadU32();
        sp.owner      = r.ReadU16();
        sp.prefabPath = r.ReadString();
        sp.x = r.ReadF32(); sp.y = r.ReadF32(); sp.z = r.ReadF32();
        m_pendingSpawns.push_back(std::move(sp));
    }
    catch (const NetReadError&)
    {
        Logger::Warn("NetworkSystem: Spawn パケットの解析に失敗しました");
    }
}

void NetworkSystem::HandleDespawn(const std::vector<u8>& body, entt::registry& reg)
{
    try
    {
        NetReader r(body);
        const NetId netId = r.ReadU32();
        auto it = m_netToEntity.find(netId);
        if (it != m_netToEntity.end())
        {
            if (reg.valid(it->second)) reg.destroy(it->second);
            m_netToEntity.erase(it);
        }
        if (m_eventBus)
        {
            EngineEvent e; e.name = "net.despawned";
            e.set("netId", static_cast<double>(netId));
            m_eventBus->Post(std::move(e));
        }
    }
    catch (const NetReadError&)
    {
        Logger::Warn("NetworkSystem: Despawn パケットの解析に失敗しました");
    }
}

NetId NetworkSystem::RequestSpawn(const std::string& prefabPath, f32 x, f32 y, f32 z,
                                  ClientId owner, std::string& outError)
{
    if (!IsServer()) { outError = "spawn is server-only"; return kInvalidNetId; }

    const NetId id = m_nextNetId++;
    m_pendingSpawns.push_back({ id, owner, prefabPath, x, y, z });

    NetWriter w;
    w.WriteU32(id);
    w.WriteU16(owner);
    w.WriteString(prefabPath);
    w.WriteF32(x); w.WriteF32(y); w.WriteF32(z);
    BroadcastPacket(PacketType::Spawn, w.Data(), true);

    Logger::Info("NetworkSystem: spawn requested (netId={}, prefab={})", id, prefabPath);
    return id;
}

bool NetworkSystem::RequestDespawn(NetId netId, entt::registry& reg, std::string& outError)
{
    if (!IsServer()) { outError = "despawn is server-only"; return false; }

    auto it = m_netToEntity.find(netId);
    if (it == m_netToEntity.end()) { outError = "unknown netId"; return false; }

    if (reg.valid(it->second)) reg.destroy(it->second);
    m_netToEntity.erase(it);

    NetWriter w;
    w.WriteU32(netId);
    BroadcastPacket(PacketType::Despawn, w.Data(), true);

    Logger::Info("NetworkSystem: despawn requested (netId={})", netId);
    return true;
}

std::vector<NetworkSystem::PendingSpawn> NetworkSystem::ConsumePendingSpawns()
{
    std::vector<PendingSpawn> out;
    out.swap(m_pendingSpawns);
    return out;
}

void NetworkSystem::OnEntityInstantiated(NetId netId, ClientId owner, entt::entity e, entt::registry& reg)
{
    auto& ni = reg.get_or_emplace<NetworkIdentity>(e);
    ni._netId = netId;
    ni._owner = owner;
    ni._isLocalOwner = (owner == m_localClientId);
    ni._netSpawned = true;

    m_netToEntity[netId] = e;
    if (netId >= m_nextNetId) m_nextNetId = netId + 1;   // クライアントは採番しないが将来の衝突予防

    if (m_eventBus)
    {
        EngineEvent ev; ev.name = "net.spawned";
        ev.set("netId", static_cast<double>(netId));
        ev.set("owner", static_cast<double>(owner));
        m_eventBus->Post(std::move(ev));
    }
}

entt::entity NetworkSystem::FindEntityByNetId(NetId netId) const
{
    auto it = m_netToEntity.find(netId);
    return it != m_netToEntity.end() ? it->second : entt::entity{ entt::null };
}

void NetworkSystem::HandleTransportEvent(const TransportEvent& ev, entt::registry& reg)
{
    switch (ev.type)
    {
    case TransportEventType::Connected:
    {
        if (IsServer())
        {
            const ClientId id = m_nextClientId++;
            m_peerToClient[ev.peer] = id;
            SendWelcomeAndBaseline(ev.peer, reg);
            Logger::Info("NetworkSystem: client connected (clientId={})", id);
            if (m_eventBus)
            {
                EngineEvent e; e.name = "net.clientConnected";
                e.set("client", static_cast<double>(id));
                m_eventBus->Post(std::move(e));
            }
        }
        else if (IsClient())
        {
            m_serverPeer = ev.peer;
            // 「つながった」の正式な通知は Welcome 受信時(net.connected)。
        }
        break;
    }
    case TransportEventType::Disconnected:
    {
        if (IsServer())
        {
            auto it = m_peerToClient.find(ev.peer);
            const ClientId id = (it != m_peerToClient.end()) ? it->second : ClientId{ 0 };
            if (it != m_peerToClient.end())
            {
                m_readyClients.erase(id);
                m_peerToClient.erase(it);
            }
            Logger::Info("NetworkSystem: client disconnected (clientId={})", id);
            if (m_eventBus)
            {
                EngineEvent e; e.name = "net.clientDisconnected";
                e.set("client", static_cast<double>(id));
                m_eventBus->Post(std::move(e));
            }
        }
        else
        {
            Logger::Info("NetworkSystem: disconnected from server");
            if (m_eventBus)
            {
                EngineEvent e; e.name = "net.disconnected";
                m_eventBus->Post(std::move(e));
            }
        }
        break;
    }
    case TransportEventType::Data:
    {
        if (ev.data.empty()) break;
        const PacketType type = static_cast<PacketType>(ev.data[0]);
        const std::vector<u8> body(ev.data.begin() + 1, ev.data.end());

        if (IsServer())
        {
            if (type == PacketType::SceneReady) HandleSceneReady(ev.peer);
            // Input/RpcMessage はフェーズ⑥/⑦で処理する。
        }
        else if (IsClient())
        {
            if      (type == PacketType::Welcome)  HandleWelcome(body);
            else if (type == PacketType::Baseline) HandleBaseline(body, reg);
            else if (type == PacketType::Spawn)    HandleSpawn(body);
            else if (type == PacketType::Despawn)  HandleDespawn(body, reg);
            // Snapshot/RpcMessage は後続フェーズで処理する。
        }
        break;
    }
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
