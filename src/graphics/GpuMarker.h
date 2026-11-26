#pragma once
// ===========================================================================
// GpuMarker.h（W05-27 / F0-3b）
//   名前付きの GPU イベント（PIX / Nsight / DRED のパンくずが読む）と GpuTimer のタイムスタンプを 1 行で打つ。
//   WinPixEventRuntime には依存しない（ID3D12GraphicsCommandList::BeginEvent を直接呼ぶ）。
//
//   使い方:  UNO_GPU_SCOPE(cmd, timer, GpuTimer::Shadows);      // スコープを抜けると自動で End（例外・早期 return でも対が崩れない）
//            GpuScope s(cmd, nullptr, GpuTimer::Total);          // timer が null ならイベントだけ
//            GpuEventScope e(cmd, name, len);                    // イベントだけ（GpuEvent.h。GpuTimer 非依存）
//   対にできない既存のコード（Begin と End が別の場所）は GpuMarker::Begin / End を使う（同じ効果、手動の対）。
// ===========================================================================
#include "graphics/GpuEvent.h"
#include "graphics/GpuTimer.h"

#include <cstring>

namespace dx12e
{

#ifndef UNO_CONCAT
#define UNO_CONCAT_IMPL(a, b) a##b
#define UNO_CONCAT(a, b) UNO_CONCAT_IMPL(a, b)
#endif

namespace GpuMarker
{
    // GpuTimer のスコープ名の長さ（NUL を含まない）。初回に 1 回だけ全部計算してキャッシュする（毎フレーム strlen しない）。
    inline u32 DefaultNameLen(GpuTimer::Scope s)
    {
        struct Table
        {
            u32 len[GpuTimer::kMaxScopes] = {};
            Table() { for (u32 i = 0; i < GpuTimer::Count; ++i) len[i] = static_cast<u32>(std::strlen(GpuTimer::Info(static_cast<GpuTimer::Scope>(i)).name)); }
        };
        static const Table t;
        return (s < GpuTimer::Count) ? t.len[s] : 0u;
    }

    // 手動の対（timer は null 可）。
    inline void Begin(ID3D12GraphicsCommandList* cmd, GpuTimer* timer, GpuTimer::Scope s)
    {
        BeginEvent(cmd, GpuTimer::Info(s).name, DefaultNameLen(s));
        if (timer && cmd) timer->Begin(cmd, s);
    }
    inline void End(ID3D12GraphicsCommandList* cmd, GpuTimer* timer, GpuTimer::Scope s)
    {
        if (timer && cmd) timer->End(cmd, s);
        EndEvent(cmd);
    }
} // namespace GpuMarker

class GpuScope
{
public:
    // 名前は GpuTimer::Info(scope).name（長さはキャッシュ済み）。timer が null ならイベントだけ。
    GpuScope(ID3D12GraphicsCommandList* cmd, GpuTimer* timer, GpuTimer::Scope scope)
        : m_cmd(cmd), m_timer(timer), m_scope(scope), m_hasTimer(timer != nullptr)
    {
        GpuMarker::BeginEvent(cmd, GpuTimer::Info(scope).name, GpuMarker::DefaultNameLen(scope));
        if (m_timer && m_cmd) m_timer->Begin(m_cmd, m_scope);
    }
    // 別名を付けたいとき。len は name の長さ（呼び出し側がキャッシュして渡す。ここでは strlen しない）。
    GpuScope(ID3D12GraphicsCommandList* cmd, GpuTimer* timer, GpuTimer::Scope scope, const char* name, u32 len)
        : m_cmd(cmd), m_timer(timer), m_scope(scope), m_hasTimer(timer != nullptr)
    {
        GpuMarker::BeginEvent(cmd, name, len);
        if (m_timer && m_cmd) m_timer->Begin(m_cmd, m_scope);
    }
    // 文字列リテラル用。長さは N-1（コンパイル時。strlen なし）。GpuScope(cmd, timer, scope, "TestPass") と書ける。
    template<size_t N>
    GpuScope(ID3D12GraphicsCommandList* cmd, GpuTimer* timer, GpuTimer::Scope scope, const char (&name)[N])
        : GpuScope(cmd, timer, scope, name, static_cast<u32>(N - 1)) {}
    ~GpuScope()
    {
        if (m_hasTimer && m_cmd) m_timer->End(m_cmd, m_scope);
        GpuMarker::EndEvent(m_cmd);
    }
    GpuScope(const GpuScope&) = delete;
    GpuScope& operator=(const GpuScope&) = delete;

private:
    ID3D12GraphicsCommandList* m_cmd = nullptr;
    GpuTimer*                  m_timer = nullptr;
    GpuTimer::Scope            m_scope = GpuTimer::Total;
    bool                       m_hasTimer = false;
};

#define UNO_GPU_SCOPE(cmd, timer, scope) ::dx12e::GpuScope UNO_CONCAT(_gs, __LINE__){cmd, timer, scope}

} // namespace dx12e
