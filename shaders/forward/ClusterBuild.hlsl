// ClusterBuild.hlsl - クラスタ AABB を view 空間で構築する compute（cs_6_0 / CSMain）。
//
// 1 スレッド = 1 クラスタ。3456 クラスタ ÷ 64 = 54 グループ。
// タイルの矩形は「画素座標」ではなく「グリッド比」で作るので、ビューポート解像度に依存しない
// （＝ビューポートがリサイズされても再構築が要らない。毎フレーム走らせても数マイクロ秒）。

#define CLUSTER_CS 1
#include "ClusterCommon.hlsli"

RWStructuredBuffer<ClusterAABB> g_clusters : register(u0);

// NDC(-1..1) → view 空間の「z=1 平面上の点」。LH 透視なので全レイは原点で交わる。
// proj._11 = 1/(aspect*tan(fovY/2)) / proj._22 = 1/tan(fovY/2) なので、その逆数を掛ける。
float3 ViewRay(float2 ndc)
{
    return float3(ndc.x * gMisc.x, ndc.y * gMisc.y, 1.0);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint ci = dtid.x;
    if (ci >= gGridDims.w) return;

    uint x =  ci % gGridDims.x;
    uint y = (ci / gGridDims.x) % gGridDims.y;
    uint z =  ci / (gGridDims.x * gGridDims.y);

    // タイルの NDC 矩形（D3D: 画素 y は下向き / NDC y は上向きなので y を反転）
    float2 t0 = float2(x,     y    ) / float2(gGridDims.xy);
    float2 t1 = float2(x + 1, y + 1) / float2(gGridDims.xy);
    float2 nMin = float2(t0.x * 2.0 - 1.0, 1.0 - t1.y * 2.0);
    float2 nMax = float2(t1.x * 2.0 - 1.0, 1.0 - t0.y * 2.0);

    // 指数分割のスライス境界（Tiago Sousa / DOOM 2016）
    float ratio = gZParams.y / gZParams.x;
    float zN = gZParams.x * pow(ratio, (float)z       / (float)gGridDims.z);
    float zF = gZParams.x * pow(ratio, (float)(z + 1) / (float)gGridDims.z);
    // 最終スライスだけは実カメラ far まで伸ばす（クラスタ用 far でクランプした先のライトを落とさない）
    if (z == gGridDims.z - 1) zF = max(zF, gZParams.z);

    // ViewRay は z=1 平面上なので、zN / zF 倍すれば view 空間の 4 隅になる
    float3 r00 = ViewRay(float2(nMin.x, nMin.y));
    float3 r10 = ViewRay(float2(nMax.x, nMin.y));
    float3 r01 = ViewRay(float2(nMin.x, nMax.y));
    float3 r11 = ViewRay(float2(nMax.x, nMax.y));

    float3 lo = min(min(r00, r10), min(r01, r11));
    float3 hi = max(max(r00, r10), max(r01, r11));

    // x,y は符号次第で近端/遠端のどちらが極値になるか変わるので両方見る
    float3 pN0 = lo * zN, pN1 = hi * zN;
    float3 pF0 = lo * zF, pF1 = hi * zF;

    ClusterAABB c;
    c.minPt = float4(min(min(pN0, pN1), min(pF0, pF1)), 0.0);
    c.maxPt = float4(max(max(pN0, pN1), max(pF0, pF1)), 0.0);
    // ViewRay.z == 1 なので z はスライス境界そのもの（丸め誤差を残さないため明示代入）
    c.minPt.z = zN;
    c.maxPt.z = zF;
    g_clusters[ci] = c;
}
