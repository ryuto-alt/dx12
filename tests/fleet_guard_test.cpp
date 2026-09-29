// フリート運用の自己終了(core/mcp/FleetGuard.h)のテスト。
//   ・owner が消えたら OwnerGone(1 秒間隔に間引かれる)
//   ・idle が閾値(小数分も)を超えたら Idle。ping / describe_mcp_manifest は活動に数えない
//   ・idleExitMin=0 / ownerPid=0 なら何も起きない
//   ・TouchInput は活動に数える
//   ・IsProcessAlive: 自プロセス=生存、存在しない pid=死亡
// GPU もエンジンも要らない。実行: ctest --output-on-failure -R FleetGuard

#include <cstdio>

#include "core/mcp/FleetGuard.h"

using namespace dx12e::fleet;

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Check(bool cond, const char* label)
{
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::printf("  NG  %s\n", label);
}

struct Fixture
{
    double now = 1000.0;
    bool   ownerAlive = true;
    int    aliveCalls = 0;
    Guard  g;
    Fixture()
    {
        g.SetClock([this] { return now; });
        g.SetAliveFn([this](uint32_t) { ++aliveCalls; return ownerAlive; });
    }
};
} // namespace

int main()
{
    // owner 死亡
    {
        Fixture f;
        f.g.Configure(4242, 0.0, "fleet-a");
        Check(f.g.PollExit(f.now) == ExitReason::None, "owner 生存 → None");
        f.ownerAlive = false;
        f.now += 0.5;
        Check(f.g.PollExit(f.now) == ExitReason::None, "1 秒未満は確認を間引く(死亡を見逃す)");
        f.now += 0.6;
        Check(f.g.PollExit(f.now) == ExitReason::OwnerGone, "1 秒経ったら owner 死亡を検出");
        Check(f.g.InstanceId() == "fleet-a" && f.g.OwnerPid() == 4242, "Configure の値が読める");
    }
    // 間引き: 1 秒に 1 回しか生存確認を呼ばない
    {
        Fixture f;
        f.g.Configure(7, 0.0, "");
        for (int i = 0; i < 60; ++i) { f.now += 1.0 / 60.0; f.g.PollExit(f.now); }
        Check(f.aliveCalls <= 2, "60fps で 1 秒回しても生存確認は高々 2 回");
    }
    // ownerPid=0 は監視しない
    {
        Fixture f;
        f.ownerAlive = false;
        f.g.Configure(0, 0.0, "");
        f.now += 100.0;
        Check(f.g.PollExit(f.now) == ExitReason::None && f.aliveCalls == 0, "ownerPid=0 なら OwnerGone にならず確認もしない");
    }
    // idle
    {
        Fixture f;
        f.g.Configure(0, 10.0, "");
        f.now += 599.0;
        Check(f.g.PollExit(f.now) == ExitReason::None, "10 分未満は None");
        Check(f.g.IdleSec() > 598.9 && f.g.IdleSec() < 599.1, "IdleSec が経過秒");
        f.now += 2.0;
        Check(f.g.PollExit(f.now) == ExitReason::Idle, "10 分を超えたら Idle");
    }
    // 小数分
    {
        Fixture f;
        f.g.Configure(0, 0.1, "");   // 6 秒
        f.now += 5.9;
        Check(f.g.PollExit(f.now) == ExitReason::None, "0.1 分: 5.9 秒は None");
        f.now += 0.2;
        Check(f.g.PollExit(f.now) == ExitReason::Idle, "0.1 分: 6.1 秒で Idle");
    }
    // ping / manifest は活動ではない・それ以外は活動
    {
        Fixture f;
        f.g.Configure(0, 0.1, "");
        f.now += 4.0;
        f.g.Touch("ping");
        f.g.Touch("describe_mcp_manifest");
        f.now += 2.5;
        Check(f.g.PollExit(f.now) == ExitReason::Idle, "ping / describe_mcp_manifest を撃っても idle は進む");
        f.g.Touch("list_entities");
        Check(f.g.IdleSec() < 0.001, "ping 以外の method で idle がリセットされる");
        Check(f.g.PollExit(f.now) == ExitReason::None, "リセット後は None");
    }
    // TouchInput
    {
        Fixture f;
        f.g.Configure(0, 0.1, "");
        f.now += 5.0;
        f.g.TouchInput();
        f.now += 5.0;
        Check(f.g.PollExit(f.now) == ExitReason::None, "TouchInput で idle がリセットされる");
    }
    // idleExitMin=0 / 負数は無効
    {
        Fixture f;
        f.g.Configure(0, 0.0, "");
        f.now += 1e6;
        Check(f.g.PollExit(f.now) == ExitReason::None && f.g.IdleExitMin() == 0.0, "idleExitMin=0 は無効");
        f.g.Configure(0, -5.0, "");
        Check(f.g.IdleExitMin() == 0.0, "負数は 0 に丸める");
    }
    // UptimeSec
    {
        Fixture f;
        f.g.Configure(0, 0.0, "");
        f.now += 42.0;
        Check(f.g.UptimeSec() > 41.9 && f.g.UptimeSec() < 42.1, "UptimeSec");
    }
    // プロセス生存判定(実 OS)
    {
#ifdef _WIN32
        Check(IsProcessAlive(static_cast<uint32_t>(GetCurrentProcessId())), "自プロセスは生存");
        Check(!IsProcessAlive(0x7ffffff0u), "存在しない pid は死亡");
        Check(!IsProcessAlive(0), "pid 0 は死亡扱い");
#endif
    }
    std::printf("FleetGuard: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
