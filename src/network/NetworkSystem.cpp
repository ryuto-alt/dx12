#include "network/NetworkSystem.h"

#include <algorithm>
#include "network/EnetTransport.h"
#include "network/Interest.h"
#include "network/NetBuffer.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "physics/PhysicsSystem.h"   // 予測リコンシリエーションのリプレイ専用(フェーズ⑦b)

#include <DirectXMath.h>
#include <cmath>

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
    m_localTime = 0.0f;
    m_snapshotAccum = 0.0f;
    m_tick = 0;
    m_interpBuffers.clear();
    m_inputHistory.clear();
    m_latestInput.clear();
    m_predictedHistory.clear();
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
    m_localTime = 0.0f;
    m_snapshotAccum = 0.0f;
    m_tick = 0;
    m_interpBuffers.clear();
    m_inputHistory.clear();
    m_latestInput.clear();
    m_predictedHistory.clear();
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
    m_localTime = 0.0f;
    m_snapshotAccum = 0.0f;
    m_tick = 0;
    m_interpBuffers.clear();
    m_rpcHandlers.clear();   // Lua側のsol::functionを握ったままにしない(Play終了でLua stateごと消える)
    m_inputHistory.clear();
    m_latestInput.clear();
    m_predictedHistory.clear();
}

namespace {
// ルート + 子孫をまとめて破棄する。
// ★以前はルート 1 個だけ destroy していたので、子を持つプレハブ（武器やコリジョン用の
//   子ノードを分けた敵など）を despawn すると **子メッシュだけがワールド原点付近に
//   取り残されて描かれ続けた**（ComputeWorldMatrix は無効な親で打ち切るため、
//   親の変換が抜けたローカル変換で描かれる）。エンジンの通常削除経路は
//   ApplicationRender.cpp:2548 で同じ BFS をしている。
void DestroyEntitySubtree(entt::registry& reg, entt::entity root)
{
    if (!reg.valid(root)) return;
    std::vector<entt::entity> all{root};
    for (size_t head = 0; head < all.size(); ++head)
        for (auto [child, tf] : reg.view<const Transform>().each())
            if (tf.parent == all[head]
                && std::find(all.begin(), all.end(), child) == all.end())
                all.push_back(child);
    for (auto it = all.rbegin(); it != all.rend(); ++it)
        if (reg.valid(*it)) reg.destroy(*it);
}
} // namespace

void NetworkSystem::PreSimUpdate(f32 /*dt*/, entt::registry& reg)
{
    if (!m_transport) return;

    // サーバーは毎フレーム未割当(_netId==0)の NetworkIdentity を拾っておく
    // (エディタ/ゲームが実行中にコンポーネントを足すケースも取りこぼさない)。
    if (IsServer()) AssignNetIds(reg);

    for (auto& ev : m_transport->Poll(0)) HandleTransportEvent(ev, reg);
}

void NetworkSystem::PostSimUpdate(f32 dt, entt::registry& reg)
{
    if (!m_transport) return;
    m_localTime += dt;
    ++m_tick;   // 自分側のローカルtick(参考値。厳密なfixed-tickではない)

    if (IsServer())
    {
        m_snapshotAccum += dt;
        const f32 interval = (m_config.snapshotRate > 0)
            ? (1.0f / static_cast<f32>(m_config.snapshotRate)) : 0.0f;
        if (interval <= 0.0f || m_snapshotAccum >= interval)
        {
            m_snapshotAccum = (interval > 0.0f) ? std::fmod(m_snapshotAccum, interval) : 0.0f;
            SendSnapshots(reg);
        }
    }
    else if (IsClient())
    {
        RecordPredictedState(reg);
        ApplyInterpolation(reg);
        SendInput();
    }
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
    const NetChannel ch = reliable ? NetChannel::Reliable : NetChannel::Unreliable;
    m_transport->Send(peer, ch, reliable, packet.data(), packet.size());
}

void NetworkSystem::BroadcastPacket(PacketType type, const std::vector<u8>& body, bool reliable, PeerHandle exclude)
{
    if (!m_transport) return;
    std::vector<u8> packet;
    packet.reserve(body.size() + 1);
    packet.push_back(static_cast<u8>(type));
    packet.insert(packet.end(), body.begin(), body.end());
    const NetChannel ch = reliable ? NetChannel::Reliable : NetChannel::Unreliable;
    m_transport->Broadcast(ch, reliable, packet.data(), packet.size(), exclude);
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
            DestroyEntitySubtree(reg, it->second);
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

    DestroyEntitySubtree(reg, it->second);
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

void NetworkSystem::SendSnapshots(entt::registry& reg)
{
    if (m_readyClients.empty()) return;   // 誰も受け取れる状態でないなら送るだけ無駄

    struct Candidate { entt::entity e; NetId netId; ClientId owner; f32 interestRadius; };
    std::vector<Candidate> candidates;
    for (auto [e, ni, nt, tf] : reg.view<NetworkIdentity, NetworkTransform, Transform>().each())
    {
        (void)nt; (void)tf;
        if (ni._netId == kInvalidNetId) continue;
        candidates.push_back({ e, ni._netId, ni._owner, ni.interestRadius });
    }
    if (candidates.empty()) return;

    // 興味管理(フェーズ⑧): クライアント毎に「自分が所有するエンティティの位置」を観測点とし、
    // interestRadius(NetworkIdentity)を超えて離れた候補はそのクライアントへは送らない
    // (帯域のO(接続数×エンティティ数)爆発を避ける最初の一手。観測点が見つからない=
    // 所有物がまだ無いクライアントにはフィルタせず全部送る)。
    for (auto& [peer, clientId] : m_peerToClient)
    {
        if (m_readyClients.count(clientId) == 0) continue;   // Baseline適用待ちには送らない

        DirectX::XMFLOAT3 observerPos{};
        bool hasObserver = false;
        for (auto& c : candidates)
        {
            if (c.owner == clientId) { observerPos = reg.get<Transform>(c.e).position; hasObserver = true; break; }
        }

        NetWriter w;
        w.WriteU32(m_tick);

        std::vector<const Candidate*> relevant;
        for (auto& c : candidates)
        {
            if (hasObserver)
            {
                const auto& pos = reg.get<Transform>(c.e).position;
                if (!IsRelevant(observerPos, pos, c.interestRadius)) continue;
            }
            relevant.push_back(&c);
        }

        w.WriteU32(static_cast<u32>(relevant.size()));
        for (auto* cp : relevant)
        {
            const auto& ni = reg.get<NetworkIdentity>(cp->e);
            const auto& nt = reg.get<NetworkTransform>(cp->e);
            const auto& tf = reg.get<Transform>(cp->e);

            u8 flags = 0;
            if (nt.syncPosition) flags |= 0x1;
            if (nt.syncRotation) flags |= 0x2;
            if (nt.syncScale)    flags |= 0x4;

            // オーナー予測(syncMode=1)のリコンシリエーションに使う: サーバーがこのエンティティの
            // 所有者から最後に受理した入力のtick。非予測エンティティやowner未接続時は0(無視される)。
            u32 lastProcessedInputTick = 0;
            {
                auto iit = m_latestInput.find(ni._owner);
                if (iit != m_latestInput.end()) lastProcessedInputTick = iit->second.tick;
            }

            w.WriteU32(ni._netId);
            w.WriteU8(flags);
            w.WriteU32(lastProcessedInputTick);
            if (nt.syncPosition)
            {
                w.WriteF32(tf.position.x); w.WriteF32(tf.position.y); w.WriteF32(tf.position.z);
            }
            if (nt.syncRotation)
            {
                using namespace DirectX;
                XMFLOAT4 q = tf.quaternion;
                if (!tf.useQuaternion)
                {
                    XMStoreFloat4(&q, XMQuaternionRotationRollPitchYaw(
                        XMConvertToRadians(tf.rotation.x),
                        XMConvertToRadians(tf.rotation.y),
                        XMConvertToRadians(tf.rotation.z)));
                }
                w.WriteF32(q.x); w.WriteF32(q.y); w.WriteF32(q.z); w.WriteF32(q.w);
            }
            if (nt.syncScale)
            {
                w.WriteF32(tf.scale.x); w.WriteF32(tf.scale.y); w.WriteF32(tf.scale.z);
            }
        }

        SendTo(peer, PacketType::Snapshot, w.Data(), false);
    }
}

void NetworkSystem::HandleSnapshot(const std::vector<u8>& body, entt::registry& reg)
{
    try
    {
        NetReader r(body);
        const NetTick tick = r.ReadU32();
        (void)tick;   // 現状未使用(将来のデバッグ表示等向けに保持)。
        const u32 count = r.ReadU32();
        for (u32 i = 0; i < count; ++i)
        {
            const NetId netId = r.ReadU32();
            const u8 flags = r.ReadU8();
            const NetTick lastProcessedInputTick = r.ReadU32();

            SnapshotBuffer::Sample s;
            s.time = m_localTime;
            if (flags & 0x1) { s.hasPosition = true; s.position = { r.ReadF32(), r.ReadF32(), r.ReadF32() }; }
            if (flags & 0x2) { s.hasRotation = true; s.rotation = { r.ReadF32(), r.ReadF32(), r.ReadF32(), r.ReadF32() }; }
            if (flags & 0x4) { s.hasScale    = true; s.scale    = { r.ReadF32(), r.ReadF32(), r.ReadF32() }; }

            // 自分が所有する予測(syncMode=1)エンティティは補間バッファに積まず、
            // リコンシリエーション(サーバー位置との突き合わせ+必要ならリプレイ)へ回す。
            bool reconciled = false;
            if (s.hasPosition)
            {
                entt::entity e = FindEntityByNetId(netId);
                if (e != entt::null && reg.valid(e) && reg.all_of<NetworkIdentity, NetworkTransform>(e))
                {
                    const auto& ni = reg.get<NetworkIdentity>(e);
                    const auto& nt = reg.get<NetworkTransform>(e);
                    if (ni._isLocalOwner && nt.syncMode == 1)
                    {
                        ReconcilePredictedEntity(reg, e, netId, lastProcessedInputTick, s.position);
                        reconciled = true;
                    }
                }
            }
            if (!reconciled) m_interpBuffers[netId].Push(s);
        }
    }
    catch (const NetReadError&)
    {
        Logger::Warn("NetworkSystem: Snapshot パケットの解析に失敗しました");
    }
}

void NetworkSystem::RecordPredictedState(entt::registry& reg)
{
    for (auto [e, ni, nt, tf] : reg.view<NetworkIdentity, NetworkTransform, Transform>().each())
    {
        if (!ni._isLocalOwner || nt.syncMode != 1 || ni._netId == kInvalidNetId) continue;
        auto& hist = m_predictedHistory[ni._netId];
        hist.push_back({ m_tick, tf.position });
        while (hist.size() > kInputHistoryCap) hist.pop_front();
    }
}

void NetworkSystem::ReconcilePredictedEntity(entt::registry& reg, entt::entity e, NetId netId,
                                              NetTick lastProcessedInputTick, const DirectX::XMFLOAT3& serverPos)
{
    auto histIt = m_predictedHistory.find(netId);
    if (histIt == m_predictedHistory.end()) return;

    // 同tickで自分が予測していた位置を探す(履歴が短すぎて突き合わせできなければ何もしない)。
    DirectX::XMFLOAT3 predictedPos{};
    bool found = false;
    for (auto& [t, p] : histIt->second) { if (t == lastProcessedInputTick) { predictedPos = p; found = true; break; } }
    if (!found) return;

    const auto& nt = reg.get<NetworkTransform>(e);
    const f32 dx = predictedPos.x - serverPos.x;
    const f32 dy = predictedPos.y - serverPos.y;
    const f32 dz = predictedPos.z - serverPos.z;
    if (dx * dx + dy * dy + dz * dz <= nt.snapDistance * nt.snapDistance)
        return;   // 誤差許容範囲内: ローカル予測を信頼してそのまま(見た目のカクつき防止)

    if (!m_physics)
    {
        // PhysicsSystem未注入ならTransformだけ補正する(リプレイはできない=誤差が残り得る)。
        reg.get<Transform>(e).position = serverPos;
        return;
    }

    // サーバー確定位置へテレポートしてから、サーバーがまだ処理していない(tickが新しい)
    // 入力だけを CharacterController 経由で再適用し、現在フレームまで追いつく。
    m_physics->SetCharacterPosition(e, serverPos);
    auto* cc = reg.try_get<CharacterController>(e);
    for (const auto& cmd : m_inputHistory)
    {
        if (cmd.tick <= lastProcessedInputTick) continue;
        if (cc)
        {
            cc->_desiredVel = { cmd.moveVelX, 0.0f, cmd.moveVelZ };
            if (cmd.jump) cc->_jumpQueued = true;
        }
        m_physics->StepSingleCharacter(e, 1.0f / 60.0f, reg);
    }
    m_physics->SyncCharactersToTransforms(reg);
}

void NetworkSystem::ApplyInterpolation(entt::registry& reg)
{
    if (m_interpBuffers.empty()) return;

    for (auto& [netId, buf] : m_interpBuffers)
    {
        if (buf.Empty()) continue;
        auto it = m_netToEntity.find(netId);
        if (it == m_netToEntity.end() || !reg.valid(it->second)) continue;
        const entt::entity e = it->second;
        if (!reg.all_of<Transform, NetworkTransform>(e)) continue;

        const auto& nt = reg.get<NetworkTransform>(e);
        // ★予測をスキップするのは「自分が所有していて syncMode=1」のときだけ。
        //   以前は syncMode!=0 で一律に弾いていたが、**他人が所有する予測エンティティ**を
        //   書き込む経路は他に 1 つも無い（:519 も :539 も _isLocalOwner 前提）。
        //   結果、NetworkTransform を「オーナー予測」にすると自分のキャラだけ動いて
        //   **他プレイヤーが全員スポーン位置で固まる**。スナップショットは届いて
        //   m_interpBuffers に積まれ、ここで捨てられていた（ログも警告も無し）。
        //   1 クライアントのテストループでは自分しか見ないので気づけない。
        const auto& niE = reg.get<NetworkIdentity>(e);
        if (nt.syncMode != 0 && niE._isLocalOwner) continue;

        const f32 renderTime = m_localTime - (nt.interpDelayMs / 1000.0f);
        SnapshotBuffer::Sample s;
        if (!buf.TrySample(renderTime, s)) continue;

        auto& tf = reg.get<Transform>(e);
        if (s.hasPosition) tf.position = s.position;
        if (s.hasRotation) { tf.quaternion = s.rotation; tf.useQuaternion = true; }
        if (s.hasScale)    tf.scale = s.scale;
    }
}

void NetworkSystem::SetRpcHandler(const std::string& name, RpcHandler handler)
{
    m_rpcHandlers[name] = std::move(handler);
}

bool NetworkSystem::SendRpcToServer(const std::string& name, const RpcArgs& args, std::string& outError)
{
    if (!IsClient()) { outError = "rpc-to-server is client-only"; return false; }
    if (m_serverPeer == kInvalidPeer) { outError = "not connected"; return false; }

    NetWriter w;
    w.WriteString(name);
    WriteRpcArgs(w, args);
    SendTo(m_serverPeer, PacketType::RpcMessage, w.Data(), true);
    return true;
}

bool NetworkSystem::SendRpcToClient(ClientId target, const std::string& name, const RpcArgs& args, std::string& outError)
{
    if (!IsServer()) { outError = "rpc-to-client is server-only"; return false; }

    PeerHandle peer = kInvalidPeer;
    for (auto& [p, id] : m_peerToClient) if (id == target) { peer = p; break; }
    if (peer == kInvalidPeer) { outError = "unknown client"; return false; }

    NetWriter w;
    w.WriteString(name);
    WriteRpcArgs(w, args);
    SendTo(peer, PacketType::RpcMessage, w.Data(), true);
    return true;
}

bool NetworkSystem::SendRpcToAll(const std::string& name, const RpcArgs& args, std::string& outError)
{
    if (!IsServer()) { outError = "rpc-to-all is server-only"; return false; }

    NetWriter w;
    w.WriteString(name);
    WriteRpcArgs(w, args);
    BroadcastPacket(PacketType::RpcMessage, w.Data(), true);
    return true;
}

void NetworkSystem::HandleRpc(PeerHandle fromPeer, const std::vector<u8>& body)
{
    try
    {
        NetReader r(body);
        const std::string name = r.ReadString();
        const RpcArgs args = ReadRpcArgs(r);

        ClientId sender = kServerClientId;
        if (IsServer())
        {
            auto it = m_peerToClient.find(fromPeer);
            sender = (it != m_peerToClient.end()) ? it->second : ClientId{ 0 };
        }

        auto hit = m_rpcHandlers.find(name);
        if (hit != m_rpcHandlers.end() && hit->second)
            hit->second(sender, args);
        else
            Logger::Warn("NetworkSystem: 未登録のRPCを受信しました: {}", name);
    }
    catch (const NetReadError&)
    {
        Logger::Warn("NetworkSystem: RpcMessage パケットの解析に失敗しました");
    }
}

void NetworkSystem::SetLocalInput(const InputCommand& cmdIn)
{
    if (!IsClient()) return;
    InputCommand cmd = cmdIn;
    cmd.tick = m_tick;
    m_inputHistory.push_back(cmd);
    // SendInput が送るのは直近3件(冗長送信)だが、リプレイ(⑦b)には未確認分すべてが要るため
    // 保持自体は kInputHistoryCap 件まで持つ。
    while (m_inputHistory.size() > kInputHistoryCap) m_inputHistory.pop_front();
}

NetworkSystem::InputCommand NetworkSystem::GetLatestInput(ClientId owner) const
{
    auto it = m_latestInput.find(owner);
    return (it != m_latestInput.end()) ? it->second : InputCommand{};
}

void NetworkSystem::SendInput()
{
    if (m_inputHistory.empty() || m_serverPeer == kInvalidPeer) return;

    // 送信は直近3件だけ(冗長送信でパケットロスを補う)。保持自体はもっと長い
    // (kInputHistoryCap、⑦bのリプレイ用)がそれを毎回全部送るのは無駄。
    const size_t sendCount = std::min<size_t>(m_inputHistory.size(), 3);
    NetWriter w;
    w.WriteU8(static_cast<u8>(sendCount));
    for (size_t i = m_inputHistory.size() - sendCount; i < m_inputHistory.size(); ++i)
    {
        const auto& c = m_inputHistory[i];
        w.WriteU32(c.tick);
        w.WriteF32(c.moveVelX); w.WriteF32(c.moveVelZ);
        w.WriteF32(c.aimYaw);   w.WriteF32(c.aimPitch);
        w.WriteU32(c.buttons);
        w.WriteBool(c.jump);
    }
    // 高頻度・欠落許容(冗長送信で補う)なので unreliable。
    SendTo(m_serverPeer, PacketType::Input, w.Data(), false);
}

void NetworkSystem::HandleInput(PeerHandle fromPeer, const std::vector<u8>& body)
{
    auto it = m_peerToClient.find(fromPeer);
    if (it == m_peerToClient.end()) return;
    const ClientId owner = it->second;

    try
    {
        NetReader r(body);
        const u8 count = r.ReadU8();

        InputCommand newest;
        bool any = false;
        for (u8 i = 0; i < count; ++i)
        {
            InputCommand c;
            c.tick      = r.ReadU32();
            c.moveVelX  = r.ReadF32(); c.moveVelZ = r.ReadF32();
            c.aimYaw    = r.ReadF32(); c.aimPitch = r.ReadF32();
            c.buttons   = r.ReadU32();
            c.jump      = r.ReadBool();

            if (!any || c.tick > newest.tick) { newest = c; any = true; }
        }
        if (!any) return;

        auto existing = m_latestInput.find(owner);
        if (existing == m_latestInput.end() || newest.tick > existing->second.tick)
            m_latestInput[owner] = newest;
    }
    catch (const NetReadError&)
    {
        Logger::Warn("NetworkSystem: Input パケットの解析に失敗しました");
    }
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
            // ★接続状態を戻す。以前は m_serverPeer も m_role も触っていなかったので:
            //   - `net:rpc(...)` のガード（IsClient() && m_serverPeer != kInvalidPeer）を
            //     素通りし、送信は無言で捨てられるのに **成功を返し続けた**
            //   - Join() の「already hosting/connected」ガードに引っかかって、
            //     `net:disconnect()` を明示的に呼ぶまで**別サーバーへ再接続できない**
            //   RPC ハンドラ（Lua の関数）は消さない。Lua state は生きていて、
            //   再接続したら同じハンドラをそのまま使いたいため（ResetState は使わない）。
            m_serverPeer = kInvalidPeer;
            m_role       = NetRole::Offline;
            m_netToEntity.clear();
            m_interpBuffers.clear();
            m_inputHistory.clear();
            m_predictedHistory.clear();
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
            if      (type == PacketType::SceneReady)  HandleSceneReady(ev.peer);
            else if (type == PacketType::RpcMessage)  HandleRpc(ev.peer, body);
            else if (type == PacketType::Input)       HandleInput(ev.peer, body);
        }
        else if (IsClient())
        {
            if      (type == PacketType::Welcome)    HandleWelcome(body);
            else if (type == PacketType::Baseline)   HandleBaseline(body, reg);
            else if (type == PacketType::Spawn)      HandleSpawn(body);
            else if (type == PacketType::Despawn)    HandleDespawn(body, reg);
            else if (type == PacketType::Snapshot)   HandleSnapshot(body, reg);
            else if (type == PacketType::RpcMessage) HandleRpc(ev.peer, body);
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
    {
        out.push_back({ id, m_transport->GetRoundTripTimeMs(peer),
                         m_transport->GetBytesSent(peer), m_transport->GetBytesReceived(peer) });
    }
    return out;
}

u32 NetworkSystem::SyncedEntityCount(const entt::registry& reg) const
{
    return static_cast<u32>(reg.view<const NetworkIdentity>().size());
}

} // namespace dx12e
