#pragma once
// GpuEvent.h（W05-27）: GpuTimer に依存しない軽い GPU イベント（PIX / DRED のパンくず）。GpuMarker.h と RenderStageHooks.h が使う。
#include "core/Types.h"

#include <directx/d3d12.h>
#include <cstddef>

namespace dx12e
{
namespace GpuMarker
{
    constexpr u32 kPixEventAnsiVersion = 1;   // PIX_EVENT_ANSI_VERSION

    // 試験用の計数（スレッドごと。GPU もコマンドリストも要らない）。常に数える（整数 1 個の加減算だけ。リリースでも残す: 計数をマクロで切ると Renderer と試験で定義が食い違い ODR になる）。
    struct Counters { u32 begins = 0; u32 ends = 0; i32 depth = 0; i32 maxDepth = 0; };
    inline Counters& Stats() { static thread_local Counters c; return c; }
    inline void ResetStats() { Stats() = Counters{}; }

    inline void BeginEvent(ID3D12GraphicsCommandList* cmd, const char* name, u32 len)
    {
        Counters& c = Stats();
        ++c.begins;
        if (++c.depth > c.maxDepth) c.maxDepth = c.depth;
        if (cmd) cmd->BeginEvent(kPixEventAnsiVersion, name ? name : "", name ? len + 1 : 1u);
    }
    inline void EndEvent(ID3D12GraphicsCommandList* cmd)
    {
        Counters& c = Stats();
        ++c.ends;
        --c.depth;
        if (cmd) cmd->EndEvent();
    }

} // namespace GpuMarker

// イベントだけの RAII（GpuTimer に触れない）。len は name の長さ（NUL を含まない）。呼び出し側がキャッシュして渡す。
class GpuEventScope
{
public:
    GpuEventScope(ID3D12GraphicsCommandList* cmd, const char* name, u32 len) : m_cmd(cmd) { GpuMarker::BeginEvent(cmd, name, len); }
    // 文字列リテラル用。長さは N-1（コンパイル時）。
    template<size_t N>
    GpuEventScope(ID3D12GraphicsCommandList* cmd, const char (&name)[N]) : GpuEventScope(cmd, name, static_cast<u32>(N - 1)) {}
    ~GpuEventScope() { GpuMarker::EndEvent(m_cmd); }
    GpuEventScope(const GpuEventScope&) = delete;
    GpuEventScope& operator=(const GpuEventScope&) = delete;
private:
    ID3D12GraphicsCommandList* m_cmd = nullptr;
};

} // namespace dx12e
