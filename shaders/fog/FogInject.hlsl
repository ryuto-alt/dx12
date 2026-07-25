// FogInject.hlsl - パス①: froxel ボリュームへ媒質パラメータを注入する（cs_6_0 / CSInject）。
//
// 出力 g_media:
//   .rgb = 散乱係数 σ_s（= density * albedo）
//   .a   = 消散係数 σ_t（= density。吸収 + 散乱）
//
// 高さ減衰つきの指数フォグ。ここを差し替えれば局所フォグボリュームや
// ノイズ密度（3D ノイズテクスチャ）へ拡張できる。

#include "FogCommon.hlsli"

RWTexture3D<float4> g_media : register(u0);

[numthreads(8, 8, 1)]
void CSInject(uint3 tid : SV_DispatchThreadID)
{
    // 90 は 8 の倍数ではないので境界チェックは必須。
    if (any(tid >= uint3(FROXEL_X, FROXEL_Y, FROXEL_Z))) return;

    float3 vp = FroxelToViewPos(float3(tid) + 0.5 + gJitter.xyz);
    float3 wp = FogViewToWorld(vp);

    // 高さ減衰つき指数フォグ。gHeightFalloff = 0 で高さ無依存。
    float h       = max(0.0, wp.y - gMisc.x);
    float density = gDensity * exp(-h * gHeightFalloff);

    float  sigmaT = max(density, 0.0);      // 消散係数
    float3 sigmaS = sigmaT * gAlbedo;       // 散乱係数

    g_media[tid] = float4(sigmaS, sigmaT);
}
