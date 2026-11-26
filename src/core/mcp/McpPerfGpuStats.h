#pragma once
// ===== perf_stats のフレーム単位 GPU 時間の分位点（FU-E-22）=====
// SC-1「GPU p95 <= 11ms」を perf_gate が判定するための値。平均では尾の遅いフレームが見えないので、
// 窓内の各フレームの GpuTimer::Total（ms）から p50 / p95 / p99 / max を出す。
// 純ロジック（GPU・nlohmann に触れない）。分位点の式は PerfPercentile（ApplicationInternal.h）と同じ
// 「最近傍（四捨五入）」で、frameMs.p95 と同じ定義のまま並べて読める。
#include <algorithm>
#include <cstddef>
#include <vector>

namespace dx12e::mcpperf
{

struct GpuFrameStats
{
    int    samples = 0;       // 計測できたフレーム数（0 ms = 未計測は除く）
    double p50 = 0.0, p95 = 0.0, p99 = 0.0, max = 0.0;
};

// 昇順に並べ済みの v から p(0..1) 分位点。空は 0。
inline double SortedPercentile(const std::vector<float>& sorted, double p)
{
    if (sorted.empty()) return 0.0;
    const double pc = (std::min)((std::max)(p, 0.0), 1.0);
    const size_t i = (std::min)(sorted.size() - 1,
        static_cast<size_t>(pc * static_cast<double>(sorted.size() - 1) + 0.5));
    return sorted[i];
}

// perFrameTotalMs: 窓内の各フレームの GPU Total（ms）。0 以下（未計測・タイマー無効）は捨てる。
inline GpuFrameStats ComputeGpuFrameStats(std::vector<float> perFrameTotalMs)
{
    perFrameTotalMs.erase(std::remove_if(perFrameTotalMs.begin(), perFrameTotalMs.end(),
                                         [](float v) { return !(v > 0.0f); }),   // NaN も落ちる
                          perFrameTotalMs.end());
    GpuFrameStats s;
    s.samples = static_cast<int>(perFrameTotalMs.size());
    if (perFrameTotalMs.empty()) return s;
    std::sort(perFrameTotalMs.begin(), perFrameTotalMs.end());
    s.p50 = SortedPercentile(perFrameTotalMs, 0.50);
    s.p95 = SortedPercentile(perFrameTotalMs, 0.95);
    s.p99 = SortedPercentile(perFrameTotalMs, 0.99);
    s.max = perFrameTotalMs.back();
    return s;
}

} // namespace dx12e::mcpperf
