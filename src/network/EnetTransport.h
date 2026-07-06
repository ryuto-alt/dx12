#pragma once
#include "network/Transport.h"

#include <memory>

namespace dx12e {

// ENet を使った ITransport 実装。enet.h は .cpp 内に pimpl で隠蔽する
// (McpBridge.cpp が winsock2.h を .cpp に閉じ込めているのと同じ流儀)。
class EnetTransport : public ITransport
{
public:
    EnetTransport();
    ~EnetTransport() override;
    EnetTransport(const EnetTransport&) = delete;
    EnetTransport& operator=(const EnetTransport&) = delete;

    bool HostServer(u16 port, u32 maxClients, std::string& outError) override;
    bool ConnectToServer(const std::string& ip, u16 port, u32 timeoutMs, std::string& outError) override;
    void Disconnect() override;
    void Shutdown() override;

    void Send(PeerHandle peer, NetChannel channel, bool reliable, const void* data, size_t size) override;
    void Broadcast(NetChannel channel, bool reliable, const void* data, size_t size,
                    PeerHandle exclude = kInvalidPeer) override;

    std::vector<TransportEvent> Poll(u32 timeoutMs = 0) override;

    u32  GetRoundTripTimeMs(PeerHandle peer) const override;
    bool IsHost() const override;
    bool IsConnected() const override;

    u64 GetBytesSent(PeerHandle peer) const override;
    u64 GetBytesReceived(PeerHandle peer) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dx12e
