// VgUtil.hlsl ― 作業バッファの初期化と ExecuteIndirect 引数の生成。cs_6_6。
//
//   CSReset : カウンタ領域 + 統計領域（連続 128 個の u32）を 0 にする（フレーム先頭に 1 回）。
//   CSArgs  : キューの要素数から次の Dispatch 引数を作り、必要なら次に書き込むキューのカウンタを 0 にする。
//             gPass0 = {mode（0 = 走査: 要素 x 4 スレッド / 1 = クラスタ: 要素 x 16 スレッド）, 読むカウンタ, 消すカウンタ（VG_NONE で無し）, キュー容量}
//             gPass1.x = 引数スロット（VG_ARGS_*）
//   ★スレッド列は VG_GRID_X 幅の 2 次元へ畳む（1 次元の Dispatch は 65535 グループまで。要素数 100 万 x 4 スレッドで超える）。
#include "VgCommon.hlsli"

[numthreads(VG_THREADS, 1, 1)]
void CSReset(uint3 dtid : SV_DispatchThreadID)
{
    RWByteAddressBuffer work = VgWork();
    const uint i = dtid.x;
    if (i < VG_CNT_COUNT + VG_STAT_COUNT)
        work.Store(i * 4u, 0u);
}

[numthreads(1, 1, 1)]
void CSArgs()
{
    RWByteAddressBuffer work = VgWork();
    RWByteAddressBuffer args = ResourceDescriptorHeap[gHeap1.z];

    const uint count = min(work.Load(gOffs1.z + gPass0.y * 4u), gPass0.w);
    const uint per = (gPass0.x == 0u) ? VG_NODE_CHILDREN : VG_CLUSTER_SLOTS;
    const uint groups = (count * per + VG_THREADS - 1u) / VG_THREADS;
    const uint gx = min(groups, VG_GRID_X);
    const uint gy = max((groups + VG_GRID_X - 1u) / VG_GRID_X, 1u);
    args.Store3(gPass1.x * VG_ARGS_STRIDE, uint3(groups == 0u ? 0u : gx, groups == 0u ? 0u : gy, 1u));

    if (gPass0.z != VG_NONE)
        work.Store(gOffs1.z + gPass0.z * 4u, 0u);
}

// ── P3: ラスタの呼び出し範囲と DispatchMesh 引数を作る ───────────────────────────
//   gPass0 = {phase(0 = 一相目 / 1 = 二相目), 引数スロット, useAs(増幅シェーダ経由か), 0}
//   一相目 = [0, 可視数)、二相目 = [一相目が処理済みの数, 可視数)。処理済みの数は VG_CNT_RASTER_DONE に残す。
//   MS のみ: グループ数 = クラスタ数（2 次元へ畳む）/ AS 経由: グループ数 = ceil(クラスタ数 / 32)。
[numthreads(1, 1, 1)]
void CSRasterArgs()
{
    RWByteAddressBuffer work = VgWork();
    RWByteAddressBuffer args = ResourceDescriptorHeap[gHeap1.z];

    const uint total = min(work.Load(gOffs1.z + VG_CNT_VISIBLE * 4u), gCaps.z);
    const uint start = (gPass0.x == 0u) ? 0u : min(work.Load(gOffs1.z + VG_CNT_RASTER_DONE * 4u), total);
    const uint count = total - start;
    work.Store(gOffs1.z + VG_CNT_RASTER_START * 4u, start);
    work.Store(gOffs1.z + VG_CNT_RASTER_COUNT * 4u, count);
    work.Store(gOffs1.z + VG_CNT_RASTER_DONE * 4u, total);
    uint add;
    work.InterlockedAdd(gOffs1.w + VG_STAT_RASTER_CLUSTERS * 4u, count, add);

    const uint groups = (gPass0.z != 0u) ? (count + VG_AS_THREADS - 1u) / VG_AS_THREADS : count;
    const uint gx = min(groups, VG_GRID_X);
    const uint gy = max((groups + VG_GRID_X - 1u) / VG_GRID_X, 1u);
    args.Store3(gPass0.y * VG_ARGS_STRIDE, uint3(groups == 0u ? 0u : gx, groups == 0u ? 0u : gy, 1u));
}

// ── P4: 安定コンパクション（決定論）= このフェーズの可視範囲 [start, start + count) を {instance, clusterRef} の昇順に並べる ──
//   ビトニックソートの「反転 + 半クリーナー」版（全比較が昇順 = 小さい方を若い番号へ）。長さは 2^gPass1.x へ仮想的に
//   パディングした +∞ とみなす。比較の相手（hi）が count 以上なら何もしない＝範囲外へは一度も書かない（+∞ は動かない）。
//   gPass0 = {0 = 反転（ブロック k の中で i と k-1-i）/ 1 = 半クリーナー（距離 j）, k または j, 1 = このフェーズの最初の段（統計を数える）, 0}
//   gPass1.x = log2(パディング後の長さ)。count がそれを超えるフェーズは並べ替えない（VG_STAT_SORT_SKIPPED。決定論は保証しない）。
//   ★増幅シェーダ無しの DispatchMesh はグループ順にラスタ結果が確定する（D3D12 の仕様）ので、並びが決まれば可視性バッファも決まる。
[numthreads(VG_SORT_THREADS, 1, 1)]
void CSSortStep(uint3 dtid : SV_DispatchThreadID)
{
    RWByteAddressBuffer work = VgWork();
    const uint start = work.Load(gOffs1.z + VG_CNT_RASTER_START * 4u);
    const uint count = work.Load(gOffs1.z + VG_CNT_RASTER_COUNT * 4u);
    const uint cap = 1u << gPass1.x;
    if (gPass0.z != 0u && dtid.x == 0u)
    {
        uint o;
        if (count > cap) work.InterlockedAdd(gOffs1.w + VG_STAT_SORT_SKIPPED * 4u, 1u, o);
        else             work.InterlockedAdd(gOffs1.w + VG_STAT_SORTED * 4u, count, o);
    }
    if (count > cap || count < 2u) return;

    const uint t = dtid.x;
    uint lo, hi;
    if (gPass0.x == 0u)
    {
        const uint k = gPass0.y, half = k >> 1;
        const uint blk = t / half, r = t - blk * half;
        lo = blk * k + r;
        hi = blk * k + (k - 1u - r);
    }
    else
    {
        const uint j = gPass0.y;
        lo = (t / j) * (2u * j) + (t % j);
        hi = lo + j;
    }
    if (hi >= count) return;
    const uint base = gOffs1.y + start * 8u;
    const uint2 a = work.Load2(base + lo * 8u);
    const uint2 b = work.Load2(base + hi * 8u);
    if (a.x > b.x || (a.x == b.x && a.y > b.y))
    {
        work.Store2(base + lo * 8u, b);
        work.Store2(base + hi * 8u, a);
    }
}

// ── P3: 可視性バッファの被覆画素数（計測用。gRast.yz = 幅 / 高さ）────────────────
[numthreads(8, 8, 1)]
void CSCoverage(uint3 dtid : SV_DispatchThreadID)
{
    RWByteAddressBuffer work = VgWork();
    Texture2D<uint> vis = ResourceDescriptorHeap[gHeap2.y];
    uint covered = 0;
    if (dtid.x < gRast.y && dtid.y < gRast.z)
        covered = (vis.Load(int3(dtid.xy, 0)) != VG_VIS_EMPTY) ? 1u : 0u;
    VgStatAdd(work, VG_STAT_COVERED_PIXELS, covered);
}
