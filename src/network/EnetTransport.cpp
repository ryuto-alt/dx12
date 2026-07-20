#include "network/EnetTransport.h"
#include "core/Logger.h"

#include <enet/enet.h>

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace dx12e {

namespace {
// enet_initialize/enet_deinitialize はプロセス全体で1回ずつが前提。McpBridge の WSAStartup と
// 同様にインスタンス跨ぎの参照カウントで管理する(通常はエディタで1個・--net-client起動で1個の
// EnetTransport が同時に生きるだけだが、念のため複数生成に耐えるようにしておく)。
std::atomic<int> g_enetRefCount{ 0 };

bool AcquireEnet()
{
    if (g_enetRefCount.fetch_add(1) == 0)
    {
        if (enet_initialize() != 0)
        {
            g_enetRefCount.fetch_sub(1);
            return false;
        }
    }
    return true;
}

void ReleaseEnet()
{
    if (g_enetRefCount.fetch_sub(1) == 1)
    {
        enet_deinitialize();
    }
}
} // namespace

struct EnetTransport::Impl
{
    ENetHost* host = nullptr;
    bool isServer = false;
    bool enetAcquired = false;

    // PeerHandle(u32) <-> ENetPeer*。ENetPeer::data に PeerHandle を埋め込んで逆引きする。
    // peersById は「接続試行中(ハンドシェイク未完了)も含む」ピア全体、confirmedPeers は
    // ENET_EVENT_TYPE_CONNECT を実際に受け取った(=接続確立済みの)ピアだけ。
    // ConnectToServer は非同期で即 return するため、この区別がないと IsConnected() が
    // ハンドシェイク中なのに true を返してしまう。
    std::unordered_map<u32, ENetPeer*> peersById;
    std::unordered_set<u32> confirmedPeers;
    u32 nextPeerId = 1;

    // 統計窓(フェーズ⑧)用の累積バイト数。ENetPeer自体には安定して公開された累積カウンタが
    // ないため、送受信のたびに自前で加算する(概算。パケットヘッダ等は含まない本体サイズのみ)。
    std::unordered_map<u32, u64> bytesSent;
    std::unordered_map<u32, u64> bytesReceived;

    static u32 IdOf(ENetPeer* peer) { return static_cast<u32>(reinterpret_cast<uintptr_t>(peer->data)); }
    static void SetId(ENetPeer* peer, u32 id) { peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id)); }

    void Reset()
    {
        if (host) { enet_host_destroy(host); host = nullptr; }
        peersById.clear();
        confirmedPeers.clear();
        bytesSent.clear();
        bytesReceived.clear();
        nextPeerId = 1;
        isServer = false;
        if (enetAcquired) { ReleaseEnet(); enetAcquired = false; }
    }
};

EnetTransport::EnetTransport() : m_impl(std::make_unique<Impl>()) {}

EnetTransport::~EnetTransport() { Shutdown(); }

bool EnetTransport::HostServer(u16 port, u32 maxClients, std::string& outError)
{
    auto& s = *m_impl;
    if (s.host) { outError = "already hosting/connected"; return false; }
    if (!AcquireEnet()) { outError = "enet_initialize failed"; return false; }
    s.enetAcquired = true;

    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = port;

    s.host = enet_host_create(&addr, maxClients, kNetChannelCount, 0, 0);
    if (!s.host)
    {
        outError = "enet_host_create failed (port in use?)";
        s.Reset();
        return false;
    }
    s.isServer = true;
    Logger::Info("EnetTransport: hosting on port {}", port);
    return true;
}

bool EnetTransport::ConnectToServer(const std::string& ip, u16 port, u32 timeoutMs, std::string& outError)
{
    auto& s = *m_impl;
    if (s.host) { outError = "already hosting/connected"; return false; }
    if (!AcquireEnet()) { outError = "enet_initialize failed"; return false; }
    s.enetAcquired = true;

    s.host = enet_host_create(nullptr, 1, kNetChannelCount, 0, 0);
    if (!s.host)
    {
        outError = "enet_host_create failed (client)";
        s.Reset();
        return false;
    }

    ENetAddress addr{};
    if (enet_address_set_host(&addr, ip.c_str()) != 0)
    {
        outError = "invalid host address: " + ip;
        s.Reset();
        return false;
    }
    addr.port = port;

    ENetPeer* peer = enet_host_connect(s.host, &addr, kNetChannelCount, 0);
    if (!peer)
    {
        outError = "enet_host_connect failed (no free peers)";
        s.Reset();
        return false;
    }

    // 非同期で接続を開始するだけで即 return する。相手(サーバー)も自分の enet_host_service を
    // 定期的に呼んで初めてハンドシェイクが進むため、ここで同期ブロックして待つと
    // 呼び出し元が単一スレッドでサーバーとクライアント両方を持つ場合(リッスンサーバーの
    // ホスト自身のテスト接続、あるいは本テストのようにプロセス内で両方生成するケース)に
    // サーバー側が一切サービスされず永久にタイムアウトする。実際の接続成立/失敗は
    // Poll() が返す TransportEvent::Connected / Disconnected で通知する
    // (ENet内部のデフォルトタイムアウトで失敗時は自動的に Disconnected が来る)。
    // timeoutMs は enet_peer_timeout に渡し、ハンドシェイク中の許容時間として使う。
    enet_peer_timeout(peer, 0, timeoutMs, timeoutMs);

    u32 id = s.nextPeerId++;
    Impl::SetId(peer, id);
    s.peersById[id] = peer;
    s.isServer = false;
    Logger::Info("EnetTransport: connecting to {}:{}...", ip, port);
    return true;
}

void EnetTransport::Disconnect()
{
    auto& s = *m_impl;
    if (!s.host) return;
    for (auto& [id, peer] : s.peersById)
    {
        (void)id;
        enet_peer_disconnect(peer, 0);
    }
    // 相手への DISCONNECT 送出のため少しだけサービスして flush する。
    ENetEvent ev{};
    for (int i = 0; i < 10 && enet_host_service(s.host, &ev, 10) > 0; ++i) { /* drain */ }
    s.Reset();
}

void EnetTransport::Shutdown()
{
    m_impl->Reset();
}

void EnetTransport::Send(PeerHandle peer, NetChannel channel, bool reliable, const void* data, size_t size)
{
    auto& s = *m_impl;
    if (!s.host) return;
    auto it = s.peersById.find(peer);
    if (it == s.peersById.end()) return;

    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    if (enet_peer_send(it->second, static_cast<enet_uint8>(channel), packet) < 0 && packet->referenceCount == 0)
    {
        enet_packet_destroy(packet);
    }
    else
    {
        s.bytesSent[peer] += size;
    }
}

void EnetTransport::Broadcast(NetChannel channel, bool reliable, const void* data, size_t size, PeerHandle exclude)
{
    auto& s = *m_impl;
    if (!s.host || s.peersById.empty()) return;

    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    for (auto& [id, peer] : s.peersById)
    {
        if (id == exclude) continue;
        if (enet_peer_send(peer, static_cast<enet_uint8>(channel), packet) == 0)
            s.bytesSent[id] += size;
    }
    // 誰にもキューされなかった(全員除外等)場合、ENet は参照カウント0のパケットを自動解放しない。
    if (packet->referenceCount == 0) enet_packet_destroy(packet);
}

std::vector<TransportEvent> EnetTransport::Poll(u32 timeoutMs)
{
    auto& s = *m_impl;
    std::vector<TransportEvent> out;
    if (!s.host) return out;

    ENetEvent ev{};
    u32 wait = timeoutMs;
    while (enet_host_service(s.host, &ev, wait) > 0)
    {
        wait = 0;   // 2回目以降はキューを吸い出すだけ(待たない)
        switch (ev.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
        {
            // サーバー側で accept した新規ピアは id 未割当(0)。クライアント側は
            // ConnectToServer で既に id を振っているのでそのまま使う。
            u32 id = Impl::IdOf(ev.peer);
            if (id == 0)
            {
                id = s.nextPeerId++;
                Impl::SetId(ev.peer, id);
                s.peersById[id] = ev.peer;
            }
            s.confirmedPeers.insert(id);
            TransportEvent te;
            te.type = TransportEventType::Connected;
            te.peer = id;
            out.push_back(std::move(te));
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
        {
            u32 id = Impl::IdOf(ev.peer);
            s.peersById.erase(id);
            s.confirmedPeers.erase(id);
            s.bytesSent.erase(id);
            s.bytesReceived.erase(id);
            TransportEvent te;
            te.type = TransportEventType::Disconnected;
            te.peer = id;
            out.push_back(std::move(te));
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE:
        {
            TransportEvent te;
            te.type = TransportEventType::Data;
            te.peer = Impl::IdOf(ev.peer);
            te.channel = static_cast<NetChannel>(ev.channelID);
            te.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
            s.bytesReceived[te.peer] += ev.packet->dataLength;
            enet_packet_destroy(ev.packet);
            out.push_back(std::move(te));
            break;
        }
        default:
            break;
        }
    }
    return out;
}

u32 EnetTransport::GetRoundTripTimeMs(PeerHandle peer) const
{
    auto& s = *m_impl;
    auto it = s.peersById.find(peer);
    if (it == s.peersById.end()) return 0;
    return it->second->roundTripTime;
}

bool EnetTransport::IsHost() const { return m_impl->isServer; }

bool EnetTransport::IsConnected() const { return m_impl->host != nullptr && !m_impl->confirmedPeers.empty(); }

u64 EnetTransport::GetBytesSent(PeerHandle peer) const
{
    auto it = m_impl->bytesSent.find(peer);
    return it != m_impl->bytesSent.end() ? it->second : 0;
}

u64 EnetTransport::GetBytesReceived(PeerHandle peer) const
{
    auto it = m_impl->bytesReceived.find(peer);
    return it != m_impl->bytesReceived.end() ? it->second : 0;
}

} // namespace dx12e
