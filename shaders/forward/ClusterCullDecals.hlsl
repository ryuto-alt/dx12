// ClusterCullDecals.hlsl - デカールをクラスタへビニングする compute（cs_6_0 / CSMain）。
//
// 1 スレッド = 1 クラスタ。16x9x24 = 3456 クラスタ ÷ 64 = 54 グループ。
//
// 方式（Wicked Engine）: クラスタ AABB（view 空間）の 8 頂点を「view → world → デカールローカル」で
// 変換し、その AABB を単位 AABB [-0.5,0.5] とテストする。保守的（偽陽性はあるが偽陰性は無い）。
//   https://turanszkij.wordpress.com/2017/10/12/forward-decal-rendering/
//
// ★クラスタ AABB は ClusteredLightCulling の出力を読まずにここで作り直している。
//   理由: あちらの AABB バッファは UNORDERED_ACCESS のまま置かれていて、読むには
//   遷移とライフタイム管理の結合が要る。式は 15 行なので作り直す方が安い（数マイクロ秒）。
//   ★ClusterBuild.hlsl と同じ式であること。片方を変えたら両方直す。

#include "ClusterCommon.hlsli"
#include "DecalCommon.hlsli"

cbuffer DecalCullCB : register(b0)
{
    float4x4 gInvView;    //  0..15  view → world（転置済み）
    uint4    gGrid;       // 16..19  .xyz = gridX,gridY,gridZ / .w = clusterCount
    float4   gZParams;    // 20..23  .x=zNear .y=zFarCluster .z=zFarCamera .w=未使用
    float4   gMisc;       // 24..27  .x=1/proj_11 .y=1/proj_22 .z=デカール数 .w=1クラスタ最大数
};

StructuredBuffer<Decal>  g_decals    : register(t0);
RWStructuredBuffer<uint> g_indexList : register(u0);
RWStructuredBuffer<uint> g_countList : register(u1);

// NDC(-1..1) → view 空間の「z=1 平面上の点」。ClusterBuild.hlsl の ViewRay と同一。
float3 DecalViewRay(float2 ndc)
{
    return float3(ndc.x * gMisc.x, ndc.y * gMisc.y, 1.0);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint ci = dtid.x;
    if (ci >= gGrid.w) return;

    uint x =  ci % gGrid.x;
    uint y = (ci / gGrid.x) % gGrid.y;
    uint z =  ci / (gGrid.x * gGrid.y);

    // ---- クラスタ AABB（view 空間）を ClusterBuild.hlsl と同じ式で作る ----
    float2 t0 = float2(x,     y    ) / float2(gGrid.xy);
    float2 t1 = float2(x + 1, y + 1) / float2(gGrid.xy);
    float2 nMin = float2(t0.x * 2.0 - 1.0, 1.0 - t1.y * 2.0);
    float2 nMax = float2(t1.x * 2.0 - 1.0, 1.0 - t0.y * 2.0);

    float ratio = gZParams.y / gZParams.x;
    float zN = gZParams.x * pow(ratio, (float)z       / (float)gGrid.z);
    float zF = gZParams.x * pow(ratio, (float)(z + 1) / (float)gGrid.z);
    if (z == gGrid.z - 1) zF = max(zF, gZParams.z);

    float3 r00 = DecalViewRay(float2(nMin.x, nMin.y));
    float3 r10 = DecalViewRay(float2(nMax.x, nMin.y));
    float3 r01 = DecalViewRay(float2(nMin.x, nMax.y));
    float3 r11 = DecalViewRay(float2(nMax.x, nMax.y));

    float3 lo = min(min(r00, r10), min(r01, r11));
    float3 hi = max(max(r00, r10), max(r01, r11));

    float3 pN0 = lo * zN, pN1 = hi * zN;
    float3 pF0 = lo * zF, pF1 = hi * zF;

    float3 bmin = min(min(pN0, pN1), min(pF0, pF1));
    float3 bmax = max(max(pN0, pN1), max(pF0, pF1));
    bmin.z = zN;
    bmax.z = zF;

    uint base  = ci * gMisc.w;
    uint total = (uint)gMisc.z;
    uint n = 0;

    // g_decals は CPU 側で sortOrder 昇順にソート済み。昇順に積むだけで重ね順が決まる。
    [loop]
    for (uint i = 0; i < total && n < (uint)gMisc.w; ++i)
    {
        // view → world → デカールローカル（HLSL は行ベクトル規約 v*A*B = v*(A*B)）
        float4x4 viewToLocal = mul(gInvView, g_decals[i].invWorld);

        float3 clo =  1e30;
        float3 chi = -1e30;
        [unroll]
        for (uint k = 0; k < 8; ++k)
        {
            float3 c = float3((k & 1) ? bmax.x : bmin.x,
                              (k & 2) ? bmax.y : bmin.y,
                              (k & 4) ? bmax.z : bmin.z);
            float3 l = mul(float4(c, 1.0), viewToLocal).xyz;
            clo = min(clo, l);
            chi = max(chi, l);
        }
        if (any(chi < -0.5) || any(clo > 0.5)) continue;

        g_indexList[base + n] = i;
        ++n;
    }
    g_countList[ci] = n;
}
