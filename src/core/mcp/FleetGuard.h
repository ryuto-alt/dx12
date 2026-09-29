#pragma once

// ===========================================================================
// フリート(複数エンジンの管理)用の「自分で終わる」仕組み。ヘッダオンリー・依存ゼロ(Win32 部分のみ _WIN32)。
// ---------------------------------------------------------------------------
// MCP サーバ(Node)が専用エンジンを起動する運用では、エージェントが終了時に閉じ忘れてもエンジンが残らないように、
// エンジン側にも 2 つの自己終了条件を持たせる:
//   --owner-pid <pid>     その pid のプロセスが消えたら自分で終了する(MCP サーバが落ちた/殺された場合の保険)
//   --idle-exit <分>      MCP の操作(と表示モードの実入力)が無い時間がこの長さを超えたら自分で終了する。0 で無効。小数可
//   --instance-id <id>    ping の instanceId に返す識別子(フリートのレジストリのキー)
// ★「活動」に数えないもの: ping / describe_mcp_manifest(状態監視の定期ポーリングで永久に終わらなくなるため)。
// ★時計とプロセス生存判定は差し替え可能(tests/fleet_guard_test.cpp から使う)。
// ===========================================================================

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#  include <d3d12.h>
#  include <dxgi1_4.h>
#  include <wrl/client.h>
#endif

namespace dx12e::fleet
{

enum class ExitReason { None, OwnerGone, Idle };

inline const char* ExitReasonName(ExitReason r)
{
    switch (r)
    {
    case ExitReason::OwnerGone: return "owner-gone";
    case ExitReason::Idle:      return "idle";
    default:                    return "none";
    }
}

// pid のプロセスが生きているか。OpenProcess が ERROR_INVALID_PARAMETER = 存在しない(死亡)。
// ERROR_ACCESS_DENIED は「あるが開けない」なので生存扱い。それ以外の失敗も生存扱い(誤って自殺しない側に倒す)。
inline bool IsProcessAlive(uint32_t pid)
{
#ifdef _WIN32
    if (pid == 0) return false;
    HANDLE h = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
    {
        return ::GetLastError() != ERROR_INVALID_PARAMETER;
    }
    const DWORD w = ::WaitForSingleObject(h, 0);
    ::CloseHandle(h);
    return w == WAIT_TIMEOUT;   // WAIT_OBJECT_0 = 終了済み
#else
    (void)pid;
    return true;
#endif
}

class Guard
{
public:
    using NowFn   = std::function<double()>;          // 単調増加の秒
    using AliveFn = std::function<bool(uint32_t)>;

    Guard() { Reset(); }

    // 時計 / 生存判定の差し替え(テスト用)。
    void SetClock(NowFn f)    { m_now = std::move(f); }
    void SetAliveFn(AliveFn f){ m_alive = std::move(f); }

    void Reset()
    {
        m_now = DefaultNow;
        m_alive = [](uint32_t p) { return IsProcessAlive(p); };
        m_ownerPid = 0; m_idleExitMin = 0.0; m_instanceId.clear();
        m_start = m_lastActivity = m_now();
        m_lastOwnerCheck = -1e18;
    }

    void Configure(uint32_t ownerPid, double idleExitMin, std::string instanceId)
    {
        m_ownerPid    = ownerPid;
        m_idleExitMin = idleExitMin > 0.0 ? idleExitMin : 0.0;
        m_instanceId  = std::move(instanceId);
        m_start = m_lastActivity = m_now();
        m_lastOwnerCheck = -1e18;
    }

    uint32_t           OwnerPid()    const { return m_ownerPid; }
    double             IdleExitMin() const { return m_idleExitMin; }
    const std::string& InstanceId()  const { return m_instanceId; }

    // MCP method が届いた。ping / describe_mcp_manifest は状態監視なので活動に数えない。
    void Touch(std::string_view method)
    {
        if (method == "ping" || method == "describe_mcp_manifest") return;
        m_lastActivity = m_now();
    }
    // 実マウス/実キーボードの入力(表示モードで人が触っている)。
    void TouchInput() { m_lastActivity = m_now(); }

    double IdleSec()   const { const double v = m_now() - m_lastActivity; return v > 0.0 ? v : 0.0; }
    double UptimeSec() const { const double v = m_now() - m_start;        return v > 0.0 ? v : 0.0; }

    // 毎フレーム呼んでよい(owner 確認は 1 秒間隔に間引く)。
    ExitReason PollExit() { return PollExit(m_now()); }
    ExitReason PollExit(double nowSec)
    {
        if (m_ownerPid != 0 && nowSec - m_lastOwnerCheck >= 1.0)
        {
            m_lastOwnerCheck = nowSec;
            if (!m_alive(m_ownerPid)) return ExitReason::OwnerGone;
        }
        if (m_idleExitMin > 0.0 && nowSec - m_lastActivity >= m_idleExitMin * 60.0)
            return ExitReason::Idle;
        return ExitReason::None;
    }

private:
    static double DefaultNow()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    NowFn       m_now;
    AliveFn     m_alive;
    uint32_t    m_ownerPid    = 0;
    double      m_idleExitMin = 0.0;
    std::string m_instanceId;
    double      m_start = 0.0;
    double      m_lastActivity = 0.0;
    double      m_lastOwnerCheck = -1e18;
};

inline Guard& Instance()
{
    static Guard g;
    return g;
}

#ifdef _WIN32
// このプロセスの VRAM 使用量と OS が割り当てた予算(LOCAL = 専用 VRAM)を MB で返す。失敗は false。
// ★CurrentUsage は「このプロセス」の使用量で、GPU 全体の使用量ではない(全体は MCP サーバ側が nvidia-smi 等で見る)。
inline bool QueryVideoMemoryMB(ID3D12Device* device, double& usedMB, double& budgetMB)
{
    if (!device) return false;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return false;
    usedMB   = static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0);
    budgetMB = static_cast<double>(info.Budget) / (1024.0 * 1024.0);
    return true;
}
#endif

} // namespace dx12e::fleet
