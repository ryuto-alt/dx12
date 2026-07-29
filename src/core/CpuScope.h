#pragma once

// CPU 側の計測ブロック。GPU パス(GpuTimer)と対になる workMs の内訳で、
// 「GPU は暇なのに fps が出ない」ときの犯人をブロック単位で指す。
// 実体（合計値の配列 m_cpuMs）は Application が持ち、MCP の dx12_perf_stats が
// cpuScopeMs として吐く。エディタ側（Editor ライブラリ）からも同じ配列へ加算できるよう、
// enum と RAII タイマーだけを Application.h から切り出したのがこのヘッダ。

#include <chrono>
#include "core/Types.h"

namespace dx12e
{

// ★スコープを足す動機は「other が大きい＝どこで時間を使っているか分からない」を潰すこと。
//   dx12_perf_stats の cpuScopeMs は AI が読む唯一の CPU 内訳なので、
//   ここが盲目だと「重い」以上のことを誰も言えない（実測で other が work の 40% を
//   占めていたのでこの 4 つを足した）。
enum CpuScope { CpuUpdate, CpuBuildList, CpuListSort, CpuShadowRec, CpuMainRec, CpuEditorUi,
                CpuPicking, CpuGizmo,
                CpuLights, CpuPrepass, CpuImGui, CpuMcp,
                CpuScopeCount };
const char* CpuScopeName(u32 i);

// 計測ブロックを囲む RAII。加算式なので同一スコープを複数回通っても合計になる。
// slot == nullptr なら何もしない（EditorContext 経由で配列を借りる側が、ゲーム/ヘッドレスの
// ように Application の計測が無い状況でも同じコードで書けるようにするため）。
struct CpuScopeTimer
{
    f32* slot;
    std::chrono::high_resolution_clock::time_point t0;
    explicit CpuScopeTimer(f32* s) : slot(s), t0(std::chrono::high_resolution_clock::now()) {}
    ~CpuScopeTimer()
    {
        if (!slot) return;
        *slot += std::chrono::duration<f32, std::milli>(
            std::chrono::high_resolution_clock::now() - t0).count();
    }

    CpuScopeTimer(const CpuScopeTimer&) = delete;
    CpuScopeTimer& operator=(const CpuScopeTimer&) = delete;
};

} // namespace dx12e
