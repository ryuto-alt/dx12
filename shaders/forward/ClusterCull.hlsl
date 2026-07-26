// ClusterCull.hlsl - ライト → クラスタの割り当て compute（cs_6_0 / CSMain）。
//
// 1 スレッド = 1 クラスタ。固定ストライドのインデックスリストへ書くので
// アトミックカウンタもリセット dispatch も不要（＝グローバル溢れという最悪の失敗モードが無い）。
// 溢れるのは 1 クラスタ内だけで、CLUSTER_MAX_LIGHTS_PER_CLUSTER で打ち切るだけ
// （ヒートマップ可視化で気づけるようにしてある）。

#define CLUSTER_CS 1
#include "ClusterCommon.hlsli"

StructuredBuffer<ClusterLight>  g_lights    : register(t0);
RWStructuredBuffer<ClusterAABB> g_clusters  : register(u0);
RWStructuredBuffer<uint>        g_indexList : register(u1);
RWStructuredBuffer<uint>        g_countList : register(u2);

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint ci = dtid.x;
    if (ci >= gGridDims.w) return;

    ClusterAABB box = g_clusters[ci];
    float3 bmin = box.minPt.xyz;
    float3 bmax = box.maxPt.xyz;
    float3 bc   = (bmin + bmax) * 0.5;
    float  br   = length(bmax - bmin) * 0.5;   // 外接球（スポットの円錐テスト用）

    uint numLights = (uint)gMisc.z;
    uint maxPer    = (uint)gMisc.w;
    uint base      = ci * maxPer;
    uint n         = 0;

    [loop]
    for (uint i = 0; i < numLights && n < maxPer; ++i)
    {
        ClusterLight L = g_lights[i];

        // カリングは view 空間で行う（クラスタ AABB が view 空間なので）
        float3 pv = mul(float4(L.position, 1.0), gViewMat).xyz;

        if (!ClusterSphereVsAabb(pv, L.range, bmin, bmax)) continue;

        if (L.type > 0.5)   // spot: 球だけでは false positive が多いので円錐で追い込む
        {
            float3 dv = mul(float4(L.direction, 0.0), gViewMat).xyz;
            if (!ClusterConeVsSphere(pv, normalize(dv), L.range, L.cosOuter, L.sinOuter, bc, br))
                continue;
        }

        g_indexList[base + n] = i;
        ++n;
    }

    g_countList[ci] = n;
}
