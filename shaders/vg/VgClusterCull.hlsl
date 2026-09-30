// VgClusterCull.hlsl ― K2: クラスタカリング + LOD 選択。cs_6_6。
//
// 1 スレッド = 「グループキューの要素 1 個 x メンバークラスタ 1 個」（1 要素 = 16 スレッド。グループのクラスタ数は最大 15）。
// ページプール（ByteAddressBuffer のチャンク）からクラスタヘッダを直接読む（設計書 §2.4.2。SoA の派生表は作らない）。
// 判定の順序: LOD 選択 → 錐台 → 法線コーン → HZB。
//   描く条件（設計書 §2.3.2。全常駐なので childrenResident は常に真）:
//     draw = ProjectedErrorPx(parentLodError, parentLodSphere) > tau  &&  ProjectedErrorPx(lodError, lodSphere) <= tau
//   HZB で落ちたクラスタ: phase 0（前フレーム HZB）→ 二相目へ回す（GroupQ1 に 1 クラスタのグループとして積む）、phase 1 → 捨てる。
// 出力: 可視クラスタ {instance, clusterRef = page | clusterInPage << 16}（VG_CNT_VISIBLE）+ 統計（三角形数・レベル別ヒストグラム・P3: 辺長ヒストグラム）。
//
// ルート定数: gPass0 = {phase, groupQOffset, groupQCounter, groupQCap}
#include "VgCommon.hlsli"

[numthreads(VG_THREADS, 1, 1)]
void CSClusterCull(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    RWByteAddressBuffer work = VgWork();
    const uint phase = gPass0.x;
    const uint inCount = min(work.Load(gOffs1.z + gPass0.z * 4u), gPass0.w);

    const uint tid = LinearThread(gid, gi);
    const uint entry = tid / VG_CLUSTER_SLOTS;
    const uint slot = tid % VG_CLUSTER_SLOTS;

    uint nTested = 0, nLod = 0, nCullFrustum = 0, nCullCone = 0, nDeferred = 0, nHzbDrop = 0;
    uint nVisible = 0, nTris = 0;
    uint histBin = 0xFFFFFFFFu;
    uint edgeBin = 0xFFFFFFFFu;

    if (entry < inCount)
    {
        const uint2 e = work.Load2(gPass0.y + entry * 8u);           // {instance, groupPacked}
        const uint gp = e.y;
        const uint count = (gp >> 24) & 0xFu;
        if (slot < count)
        {
            StructuredBuffer<VgInstance> instances = ResourceDescriptorHeap[gHeap0.y];
            StructuredBuffer<VgAssetGpu> assets     = ResourceDescriptorHeap[gHeap0.x];
            RWStructuredBuffer<float4>   extra      = ResourceDescriptorHeap[gHeap0.w];   // K0 が書いた UAV をそのまま読む（同じディスクリプタは SRV と UAV を兼ねられない）
            const VgInstance inst = instances[e.x];
            const VgAssetGpu asset = assets[inst.assetIndex];

            const uint page = gp & 0xFFFFu;
            const uint clusterIdx = ((gp >> 16) & 0xFFu) + slot;

            ByteAddressBuffer pool = ResourceDescriptorHeap[NonUniformResourceIndex(asset.poolSrvBase + (page >> VG_CHUNK_PAGES_LOG2))];
            const uint pageBase = (page & ((1u << VG_CHUNK_PAGES_LOG2) - 1u)) * VG_PAGE_BYTES;
            const uint hb = pageBase + 64u + clusterIdx * 128u;       // ClusterHeader（ページ先頭 + 64 + 128 * i）

            const float4 lodSphere       = asfloat(pool.Load4(hb + 0));
            const float4 parentLodSphere = asfloat(pool.Load4(hb + 16));
            const float4 cullSphere      = asfloat(pool.Load4(hb + 32));
            const uint4  misc            = pool.Load4(hb + 48);       // lodError, parentLodError, coneS8, maxEdgeLength
            const float lodError = asfloat(misc.x);
            const float parentLodError = asfloat(misc.y);
            const uint coneS8 = misc.z;

            nTested = 1;
            const float s = inst.maxScale;
            const float tau = gLod.x;

            const float3 lc = VgXform(inst.w0, inst.w1, inst.w2, lodSphere.xyz);
            const float3 pc = VgXform(inst.w0, inst.w1, inst.w2, parentLodSphere.xyz);
            const bool parentCoarse = VgProjErr(parentLodError, float4(pc, parentLodSphere.w * s), s) > tau;
            const bool fine = !(VgProjErr(lodError, float4(lc, lodSphere.w * s), s) > tau);

            if (parentCoarse && fine)
            {
                nLod = 1;
                const float3 cc = VgXform(inst.w0, inst.w1, inst.w2, cullSphere.xyz);
                const float  cr = cullSphere.w * s;
                if (VgSphereOutsideFrustum(cc, cr))
                {
                    nCullFrustum = 1;
                }
                else
                {
                    // 法線コーン（オブジェクト空間。cutoff >= 1 は無効）
                    bool coneCull = false;
                    const float4 co = extra[e.x];
                    // P4: 両面材質（VG_MAT_DOUBLE_SIDED）のクラスタは裏から見えるので法線コーンで落とさない
                    bool doubleSided = false;
                    if (asset.materialBase != VG_NONE)
                    {
                        const uint mi = pool.Load(hb + 72) >> 16;
                        if (mi < asset.materialCount)
                        {
                            StructuredBuffer<VgMaterialGpu> mats = ResourceDescriptorHeap[gHeap1.w];
                            doubleSided = (mats[asset.materialBase + mi].flags & VG_MAT_DOUBLE_SIDED) != 0u;
                        }
                    }
                    if (gCounts.w != 0 && co.w > 0.5 && !doubleSided)
                    {
                        const float cutoff = float(int(coneS8) >> 24) / 127.0;
                        if (cutoff < 1.0)
                        {
                            const float3 axis = float3(float(int(coneS8 << 24) >> 24), float(int(coneS8 << 16) >> 24), float(int(coneS8 << 8) >> 24)) / 127.0;
                            const float3 dv = cullSphere.xyz - co.xyz;
                            const float len = sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
                            const float dp = dv.x * axis.x + dv.y * axis.y + dv.z * axis.z;
                            coneCull = dp >= cutoff * len + cullSphere.w;
                        }
                    }
                    if (coneCull)
                    {
                        nCullCone = 1;
                    }
                    else if (VgHzbOccluded(phase, inst, cullSphere.xyz, cullSphere.w))
                    {
                        if (phase == 0)
                        {
                            nDeferred = 1;
                            // 1 クラスタだけのグループとして二相目へ
                            VgPush(work, gOffs1.x, VG_CNT_GROUPQ1, gCaps.y, VG_STAT_OVF_GROUPQ,
                                   uint2(e.x, (page & 0xFFFFu) | (clusterIdx << 16) | (1u << 24)));
                        }
                        else
                        {
                            nHzbDrop = 1;
                        }
                    }
                    else
                    {
                        nVisible = 1;
                        VgPush(work, gOffs1.y, VG_CNT_VISIBLE, gCaps.z, VG_STAT_OVF_VISIBLE,
                               uint2(e.x, (page & 0xFFFFu) | (clusterIdx << 16)));
                        const uint packedCounts = pool.Load(hb + 72);   // vertexCount | triangleCount << 8 | material << 16
                        const uint flags = pool.Load(hb + 92);
                        nTris = (packedCounts >> 8) & 0xFFu;
                        histBin = min((flags >> 8) & 0xFFu, VG_HIST_LEVELS - 1u);
                        // 最長辺の画面長（px。最近点で見積もる）→ 8 段。P3 の SW ラスタ判断用の計測（フラグ ON のときだけ数える）
                        {
                            const float3 dv = cc - gCamPos.xyz;
                            float d = sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z) - cr;
                            if (d < gCamPos.w) d = gCamPos.w;
                            const float edgePx = asfloat(misc.w) * s * gLod.y / d;
                            edgeBin = edgePx < 1.0 ? 0u : min((uint)floor(log2(edgePx)) + 1u, VG_EDGE_BINS - 1u);
                        }
                    }
                }
            }
        }
    }

    VgStatAdd(work, VG_STAT_CLUSTERS_TESTED, nTested);
    VgStatAdd(work, VG_STAT_CLUSTERS_LOD, nLod);
    VgStatAdd(work, VG_STAT_CL_CULL_FRUSTUM, nCullFrustum);
    VgStatAdd(work, VG_STAT_CL_CULL_CONE, nCullCone);
    VgStatAdd(work, VG_STAT_CL_DEFERRED, nDeferred);
    VgStatAdd(work, VG_STAT_CL_HZB_DROP, nHzbDrop);
    VgStatAdd(work, phase == 0 ? VG_STAT_VISIBLE_P1 : VG_STAT_VISIBLE_P2, nVisible);
    VgStatAdd(work, VG_STAT_TRIS, nTris);
    if (histBin != 0xFFFFFFFFu) VgStatAdd1(work, VG_STAT_HIST_BASE + histBin, 1u);
    if ((gRast.x & VG_RF_EDGE_HIST) != 0u)
    {
        [unroll]
        for (uint k = 0; k < VG_EDGE_BINS; ++k)
        {
            VgStatAdd(work, VG_STAT_EDGE_CL_BASE + k, edgeBin == k ? 1u : 0u);
            VgStatAdd(work, VG_STAT_EDGE_TRI_BASE + k, edgeBin == k ? nTris : 0u);
        }
    }
}
