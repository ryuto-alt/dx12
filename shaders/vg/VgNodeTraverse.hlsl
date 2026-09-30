// VgNodeTraverse.hlsl ― K1: BVH（4 分木）の 1 段ぶんの走査。cs_6_6。
//
// 1 スレッド = 「入力キューの要素 1 個（{instance, node}）x 子スロット 1 個」。ExecuteIndirect で段ごとに起動し、
// 出力キューへ次の段を積む（ウェーブフロント）。子ごとに:
//   錐台（cullSphere → ワールド）/ LOD 枝刈り（親側・自分側、設計書 §2.3.2）/ HZB（phase 0 = 前フレーム、phase 1 = 今フレーム）
//   子がノード → 次段のキューへ / 子が葉（グループ）→ GroupQ へ / HZB でだけ落ちた（phase 0 のみ）→ 二相目へ回す
//   （ノード → Deferred、葉 → GroupQ1）。
// 式は src/renderer/vg/VgLodMath.h / VgCullReference.h と同一。
//
// ルート定数: gPass0 = {phase, inOffset, inCounter, inCap} / gPass1 = {outOffset, outCounter, outCap, _}
#include "VgCommon.hlsli"

[numthreads(VG_THREADS, 1, 1)]
void CSTraverse(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    RWByteAddressBuffer work = VgWork();
    const uint phase = gPass0.x;
    const uint inCount = min(work.Load(gOffs1.z + gPass0.z * 4u), gPass0.w);

    const uint tid = LinearThread(gid, gi);
    const uint entry = tid / VG_NODE_CHILDREN;
    const uint ci = tid % VG_NODE_CHILDREN;
    const bool valid = entry < inCount;

    uint nVisited = 0, nTested = 0, nGroups = 0;
    uint nCullFrustum = 0, nCullLod = 0, nDeferred = 0;

    if (valid)
    {
        const uint2 e = work.Load2(gPass0.y + entry * 8u);       // {instance, node}
        StructuredBuffer<VgInstance> instances = ResourceDescriptorHeap[gHeap0.y];
        StructuredBuffer<VgAssetGpu> assets     = ResourceDescriptorHeap[gHeap0.x];
        const VgInstance inst = instances[e.x];
        const VgAssetGpu asset = assets[inst.assetIndex];
        StructuredBuffer<HierNode> nodes = ResourceDescriptorHeap[NonUniformResourceIndex(asset.nodesSrv)];

        if (ci == 0) nVisited = 1;

        const HierChild c = nodes[e.y].c[ci];
        if (c.ref != VG_NONE)
        {
            nTested = 1;
            const float s = inst.maxScale;

            // 錐台
            const float3 cc = VgXform(inst.w0, inst.w1, inst.w2, c.cullSphere.xyz);
            const float  cr = c.cullSphere.w * s;
            if (VgSphereOutsideFrustum(cc, cr))
            {
                nCullFrustum = 1;
            }
            else
            {
                // LOD 枝刈り: 部分木に「親が粗すぎ && 自分が十分細かい」クラスタが存在しうるか
                const float3 lc = VgXform(inst.w0, inst.w1, inst.w2, c.lodSphere.xyz);
                const float4 ls = float4(lc, c.lodSphere.w * s);
                const float tau = gLod.x;
                const bool parentCoarse = VgProjErr(c.maxParentError, ls, s) > tau;
                const bool ownFineEnough = !(VgProjErrFar(c.minOwnError, ls, s) > tau);
                if (!(parentCoarse && ownFineEnough))
                {
                    nCullLod = 1;
                }
                else
                {
                    const bool isLeaf = (c.ref & 0x80000000u) != 0;
                    const bool hzbOcc = VgHzbOccluded(phase, inst, c.cullSphere.xyz, c.cullSphere.w);
                    if (hzbOcc)
                    {
                        if (phase == 0)
                        {
                            nDeferred = 1;
                            if (isLeaf) VgPush(work, gOffs1.x, VG_CNT_GROUPQ1, gCaps.y, VG_STAT_OVF_GROUPQ, uint2(e.x, c.groupPacked));
                            else        VgPush(work, gOffs0.z, VG_CNT_DEFERRED, gCaps.w, VG_STAT_OVF_DEFERRED, uint2(e.x, c.ref));
                        }
                        // phase 1: 今フレームの HZB でも隠れている → 捨てる
                    }
                    else if (isLeaf)
                    {
                        nGroups = 1;
                        if (phase == 0) VgPush(work, gOffs0.w, VG_CNT_GROUPQ0, gCaps.y, VG_STAT_OVF_GROUPQ, uint2(e.x, c.groupPacked));
                        else            VgPush(work, gOffs1.x, VG_CNT_GROUPQ1, gCaps.y, VG_STAT_OVF_GROUPQ, uint2(e.x, c.groupPacked));
                    }
                    else
                    {
                        VgPush(work, gPass1.x, gPass1.y, gPass1.z, VG_STAT_OVF_NODEQ, uint2(e.x, c.ref));
                    }
                }
            }
        }
    }

    VgStatAdd(work, VG_STAT_NODES_VISITED, nVisited);
    VgStatAdd(work, VG_STAT_CHILDREN_TESTED, nTested);
    VgStatAdd(work, VG_STAT_GROUPS, nGroups);
    VgStatAdd(work, VG_STAT_NODE_CULL_FRUSTUM, nCullFrustum);
    VgStatAdd(work, VG_STAT_NODE_CULL_LOD, nCullLod);
    VgStatAdd(work, VG_STAT_NODE_DEFERRED, nDeferred);
}
