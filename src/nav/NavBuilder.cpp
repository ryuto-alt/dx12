// ============================================================================
// ナビメッシュ ビルドの司令塔
// ============================================================================
// 設定はメートル/度で受け取り、ここで一括してボクセル単位へ落とす。
// 各段階の所要時間と件数を report に積むので、UI と MCP がそのまま表示できる。
// ============================================================================

#include "nav/NavTypes.h"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace dx12e
{
namespace nav
{

namespace
{
using Clock = std::chrono::steady_clock;

f32 MsSince(const Clock::time_point& t0)
{
    return std::chrono::duration<f32, std::milli>(Clock::now() - t0).count();
}

void Log(NavBuildReport& r, const std::string& line)
{
#ifdef DX12_NAV_TRACE
    std::printf("  [nav] %s\n", line.c_str());
    std::fflush(stdout);
#endif
    r.stageLog += line;
    r.stageLog += "\n";
}
} // namespace

bool BuildNavMesh(const NavInputGeometry& geom, const NavBuildConfig& cfg,
                  NavMesh& outMesh, NavBuildReport& report)
{
    report = NavBuildReport{};
    const auto tAll = Clock::now();

    if (geom.Empty())
    {
        report.error = "入力ジオメトリが空。ナビメッシュに含めるメッシュがシーンにあるか確認すること";
        return false;
    }
    if (cfg.cellSize <= 1e-4f || cfg.cellHeight <= 1e-4f)
    {
        report.error = "cellSize / cellHeight が小さすぎる";
        return false;
    }
    if (cfg.agentRadius < 0.0f || cfg.agentHeight <= 0.0f)
    {
        report.error = "agentRadius / agentHeight が不正";
        return false;
    }

    report.triCount = geom.TriCount();
    std::string err;

    // ---- 1) ラスタライズ ----
    auto t0 = Clock::now();
    NavHeightfield hf;
    if (!RasterizeInput(geom, cfg, hf, err)) { report.error = err; return false; }
    report.spanCount = static_cast<i32>(hf.spans.size());
    Log(report, "1. ラスタライズ: " + std::to_string(hf.w) + "x" + std::to_string(hf.h) +
                " セル / span " + std::to_string(report.spanCount) +
                " (" + std::to_string(static_cast<i32>(MsSince(t0))) + " ms)");

    // ---- 2) フィルタ ----
    t0 = Clock::now();
    FilterHeightfield(cfg, hf);
    Log(report, "2. フィルタ(またぎ/崖/頭上): " + std::to_string(static_cast<i32>(MsSince(t0))) + " ms");

    // ---- 3) コンパクト化 ----
    t0 = Clock::now();
    NavCompactHeightfield chf;
    err.clear();
    if (!BuildCompactHeightfield(cfg, hf, chf, err)) { report.error = err; return false; }
    if (!err.empty()) Log(report, "   " + err);
    report.walkableSpans = chf.spanCount;
    Log(report, "3. コンパクト化: 歩行 span " + std::to_string(chf.spanCount) +
                " (" + std::to_string(static_cast<i32>(MsSince(t0))) + " ms)");
    hf = NavHeightfield{};   // もう要らないので早めに解放

    // ---- 4) 侵食（エージェント半径）----
    t0 = Clock::now();
    const i32 walkableRadius = static_cast<i32>(std::ceil(cfg.agentRadius / cfg.cellSize));
    ErodeWalkableArea(walkableRadius, chf);
    i32 afterErode = 0;
    for (i32 i = 0; i < chf.spanCount; ++i) if (chf.areas[i] != kNullArea) ++afterErode;
    Log(report, "4. 侵食(半径 " + std::to_string(walkableRadius) + " vox): 残り " +
                std::to_string(afterErode) + " span (" +
                std::to_string(static_cast<i32>(MsSince(t0))) + " ms)");
    if (afterErode == 0)
    {
        report.error = "エージェント半径で歩ける面が全部消えた。agentRadius を小さくするか cellSize を下げること";
        return false;
    }

    // ---- 5) 領域分割 ----
    t0 = Clock::now();
    const f32 cellArea = cfg.cellSize * cfg.cellSize;
    const i32 minRegionVox   = (std::max)(1, static_cast<i32>(cfg.minRegionArea / cellArea));
    const i32 mergeRegionVox = (std::max)(1, static_cast<i32>(cfg.mergeRegionArea / cellArea));

    err.clear();
    if (cfg.monotonePartition)
    {
        if (!BuildRegionsMonotone(chf, 0, minRegionVox, mergeRegionVox, err))
        { report.error = err; return false; }
    }
    else
    {
        BuildDistanceField(chf);
        if (!BuildRegions(chf, 0, minRegionVox, mergeRegionVox, err))
        { report.error = err; return false; }
    }
    report.regionCount = chf.maxRegions;
    Log(report, std::string("5. 領域分割(") + (cfg.monotonePartition ? "monotone" : "分水嶺") +
                "): " + std::to_string(chf.maxRegions) + " 領域 (" +
                std::to_string(static_cast<i32>(MsSince(t0))) + " ms)");
    if (chf.maxRegions == 0)
    {
        report.error = "領域が 1 つも作れなかった。minRegionArea を小さくすること";
        return false;
    }

    // ---- 6) 輪郭 ----
    t0 = Clock::now();
    NavContourSet cset;
    const i32 maxEdgeLenVox = static_cast<i32>(cfg.maxEdgeLen / cfg.cellSize);
    err.clear();
    if (!BuildContours(chf, cfg.maxSimplificationErr, maxEdgeLenVox, cset, err))
    { report.error = err.empty() ? "輪郭抽出に失敗" : err; return false; }
    report.contourCount = static_cast<i32>(cset.conts.size());
    Log(report, "6. 輪郭: " + std::to_string(report.contourCount) + " 本 (" +
                std::to_string(static_cast<i32>(MsSince(t0))) + " ms)");

    // ---- 7) ポリゴン化 ----
    t0 = Clock::now();
    NavPolyMeshRaw pm;
    err.clear();
    if (!BuildPolyMesh(cset, cfg.maxVertsPerPoly, pm, err))
    { report.error = err.empty() ? "ポリゴン化に失敗" : err; return false; }
    if (!err.empty()) Log(report, "   " + err);
    report.polyCount = pm.npolys;
    Log(report, "7. ポリゴン化: " + std::to_string(pm.npolys) + " ポリ / " +
                std::to_string(pm.nverts) + " 頂点 (" +
                std::to_string(static_cast<i32>(MsSince(t0))) + " ms)");

    // ---- 8) ランタイム表現 ----
    t0 = Clock::now();
    outMesh.BuildFromPolyMesh(pm, chf, cfg);
    Log(report, "8. ランタイム構築: 高さサンプル " +
                std::to_string(outMesh.GetStats().sampleCount) + " (" +
                std::to_string(static_cast<i32>(MsSince(t0))) + " ms)");

    report.totalMs = MsSince(tAll);
    outMesh.MutableStats().buildMs = report.totalMs;
    report.ok = true;
    Log(report, "合計 " + std::to_string(static_cast<i32>(report.totalMs)) + " ms");
    return true;
}

} // namespace nav
} // namespace dx12e
