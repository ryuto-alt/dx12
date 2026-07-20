// EnetTransport の疎通確認(127.0.0.1 ループバックでサーバー/クライアントを同一プロセス内に
// 立てて Connected/Data/Disconnected イベントが往復することを検証する)。GPU 非依存。
//
// 注意: ConnectToServer は非同期(即 return)なので、ハンドシェイクを進めるには
// サーバー・クライアント双方の Poll() を交互に呼び続ける必要がある(片方だけ待っても
// もう片方が enet_host_service を呼ばない限りパケットは処理されない)。
#include "network/EnetTransport.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace dx12e;

static int g_failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// tick() は毎回サーバー・クライアント両方を非ブロッキングでポーリングし、条件成立で true を返す。
// 成立するまで stepMs 間隔で最大 totalMs 待つ。
template <typename TickFn>
static bool WaitUntil(TickFn tick, int totalMs = 3000, int stepMs = 5)
{
    for (int elapsed = 0; elapsed < totalMs; elapsed += stepMs)
    {
        if (tick()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
    }
    return tick();
}

int main()
{
    constexpr u16 kPort = 44117; // 他サービスと衝突しにくいテスト専用ポート

    EnetTransport server;
    std::string err;
    CHECK(server.HostServer(kPort, 4, err));

    EnetTransport client;
    bool connectStarted = client.ConnectToServer("127.0.0.1", kPort, 2000, err);
    if (!connectStarted) std::printf("ConnectToServer error: %s\n", err.c_str());
    CHECK(connectStarted);

    // 双方に Connected イベントが来るまで交互にポーリング。
    PeerHandle serverSidePeer = kInvalidPeer;
    bool clientConfirmed = false;
    bool bothConnected = WaitUntil([&]() {
        for (auto& e : server.Poll(0))
        {
            if (e.type == TransportEventType::Connected) serverSidePeer = e.peer;
        }
        for (auto& e : client.Poll(0))
        {
            if (e.type == TransportEventType::Connected) clientConfirmed = true;
        }
        return serverSidePeer != kInvalidPeer && clientConfirmed;
    });
    CHECK(bothConnected);
    CHECK(serverSidePeer != kInvalidPeer);
    CHECK(client.IsConnected());
    CHECK(server.IsConnected());

    // クライアント→サーバーへ信頼チャネルでメッセージ送信、サーバー側で受信できるか確認。
    // EnetTransport はピアIDを1から採番するため、クライアント視点で唯一のピア(=サーバー)は1。
    const std::string payload = "hello-server";
    client.Send(1, NetChannel::Reliable, true, payload.data(), payload.size());

    std::string received;
    bool gotData = WaitUntil([&]() {
        (void)client.Poll(0);   // 送信側もサービスしないとキューがflushされない
        for (auto& e : server.Poll(0))
        {
            if (e.type == TransportEventType::Data) received.assign(e.data.begin(), e.data.end());
        }
        return !received.empty();
    });
    CHECK(gotData);
    CHECK(received == payload);

    // サーバー→クライアントへブロードキャスト、クライアント側で受信できるか確認。
    const std::string reply = "hello-client";
    server.Broadcast(NetChannel::Reliable, true, reply.data(), reply.size());

    std::string clientReceived;
    bool clientGotData = WaitUntil([&]() {
        (void)server.Poll(0);   // flush
        for (auto& e : client.Poll(0))
        {
            if (e.type == TransportEventType::Data) clientReceived.assign(e.data.begin(), e.data.end());
        }
        return !clientReceived.empty();
    });
    CHECK(clientGotData);
    CHECK(clientReceived == reply);

    // 切断がサーバー側にも伝わることを確認。
    client.Disconnect();
    bool gotDisconnect = WaitUntil([&]() {
        for (auto& e : server.Poll(0))
        {
            if (e.type == TransportEventType::Disconnected) return true;
        }
        return false;
    });
    CHECK(gotDisconnect);

    server.Shutdown();

    if (g_failures == 0) { std::printf("EnetLoopbackTests: all passed\n"); return 0; }
    std::printf("EnetLoopbackTests: %d failure(s)\n", g_failures);
    return 1;
}
