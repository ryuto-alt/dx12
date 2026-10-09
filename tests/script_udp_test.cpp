// scripting/ScriptUdp（Lua net.udpOpen の実体）の単体テスト。ループバックで 2 本つないで往復する。
#include "scripting/ScriptUdp.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace dx12e;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// 届くまで少し待って 1 通読む（ループバックでも即時とは限らない）
static bool WaitRecv(ScriptUdpSocket& s, std::string& out, bool latest = false)
{
    for (int i = 0; i < 200; ++i)
    {
        if (latest ? s.RecvLatest(out) : s.Recv(out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

int main()
{
    const int pa = 47821, pb = 47822;
    std::string err;
    auto a = ScriptUdpSocket::Open(pa, err);
    auto b = ScriptUdpSocket::Open(pb, err);
    CHECK(a && b);
    if (!a || !b) return 1;

    // 同じ port の二重 bind は失敗して err が入る
    { std::string e2; auto dup = ScriptUdpSocket::Open(pa, e2); CHECK(!dup && !e2.empty()); }
    // 範囲外 port
    { std::string e3; CHECK(!ScriptUdpSocket::Open(80, e3)); CHECK(!ScriptUdpSocket::Open(70000, e3)); }

    std::string got;
    CHECK(!a->Recv(got));          // 何も来ていない＝ブロックせず false
    CHECK(!a->RecvLatest(got));

    // a -> b 往復（バイナリ含む）
    CHECK(a->Send(pb, "hello"));
    CHECK(WaitRecv(*b, got) && got == "hello");
    CHECK(b->Send(pa, std::string("x\0y", 3)));
    CHECK(WaitRecv(*a, got) && got == std::string("x\0y", 3));

    // recvLatest は最後の 1 通だけ、その後は空
    CHECK(a->Send(pb, "1")); CHECK(a->Send(pb, "2")); CHECK(a->Send(pb, "3"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(WaitRecv(*b, got, true) && got == "3");
    CHECK(!b->RecvLatest(got));

    // recv は到着順に 1 通ずつ
    CHECK(a->Send(pb, "p")); CHECK(a->Send(pb, "q"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(b->Recv(got) && got == "p");
    CHECK(b->Recv(got) && got == "q");
    CHECK(!b->Recv(got));

    // 大きさの上限
    CHECK(!a->Send(pb, std::string(ScriptUdpSocket::kMaxSend + 1, 'z')));
    CHECK(a->Send(pb, std::string(ScriptUdpSocket::kMaxSend, 'z')));
    CHECK(WaitRecv(*b, got) && got.size() == ScriptUdpSocket::kMaxSend);
    CHECK(!a->Send(0, "x"));
    CHECK(!a->Send(70000, "x"));

    // 誰も聞いていない port へ送っても、その後の受信が壊れない（WSAECONNRESET 対策）
    CHECK(a->Send(47899, "nobody"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(b->Send(pa, "after"));
    CHECK(WaitRecv(*a, got) && got == "after");

    // 閉じたら同じ port を取り直せる／閉じた後の操作は安全に false
    a->Close(); a->Close();
    CHECK(!a->IsOpen() && !a->Send(pb, "x") && !a->Recv(got));
    { auto again = ScriptUdpSocket::Open(pa, err); CHECK(again != nullptr); }
    // 破棄でも閉じる
    b.reset();
    { auto again = ScriptUdpSocket::Open(pb, err); CHECK(again != nullptr); }

    std::printf("script_udp: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
