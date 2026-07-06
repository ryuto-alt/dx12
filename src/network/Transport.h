#pragma once
#include "core/Types.h"
#include "network/NetworkTypes.h"

#include <string>
#include <vector>

namespace dx12e {

// トランスポート層内でピアを指す不透明ハンドル。ENet実装では内部マップ経由でENetPeer*に解決する
// (ENetPeer* を上位に漏らさないための境界)。
using PeerHandle = u32;
constexpr PeerHandle kInvalidPeer = 0xFFFFFFFFu;

enum class TransportEventType : u8 { Connected, Disconnected, Data };

struct TransportEvent
{
    TransportEventType type = TransportEventType::Data;
    PeerHandle peer = kInvalidPeer;
    NetChannel channel = NetChannel::Reliable;
    std::vector<u8> data;   // type==Data の時のみ有効
};

// ENet 等の下層トランスポートを隠す抽象。NetworkSystem はこれ経由でのみ通信し、
// enet.h への依存は EnetTransport.cpp の中だけに閉じ込める。
class ITransport
{
public:
    virtual ~ITransport() = default;

    virtual bool HostServer(u16 port, u32 maxClients, std::string& outError) = 0;

    // 接続を非同期に開始するだけで即 return する(戻り値は「試行開始できたか」であり
    // 「接続が確立したか」ではない)。実際の成立/失敗は Poll() が返す
    // TransportEvent::Connected / Disconnected で分かる。timeoutMs はハンドシェイクの
    // 許容時間。同期ブロックにしないのは、呼び出し元がサーバーとクライアントを同一スレッドで
    // 両方サービスする構成(リッスンサーバー)でデッドロック/タイムアウトしないため。
    virtual bool ConnectToServer(const std::string& ip, u16 port, u32 timeoutMs, std::string& outError) = 0;
    virtual void Disconnect() = 0;
    virtual void Shutdown() = 0;

    virtual void Send(PeerHandle peer, NetChannel channel, bool reliable, const void* data, size_t size) = 0;
    virtual void Broadcast(NetChannel channel, bool reliable, const void* data, size_t size,
                            PeerHandle exclude = kInvalidPeer) = 0;

    // 毎フレーム呼ぶ。timeoutMs=0 はノンブロッキング(1回分のキューだけ吸い出す)。
    virtual std::vector<TransportEvent> Poll(u32 timeoutMs = 0) = 0;

    virtual u32  GetRoundTripTimeMs(PeerHandle peer) const = 0;
    virtual bool IsHost() const = 0;
    virtual bool IsConnected() const = 0;
};

} // namespace dx12e
