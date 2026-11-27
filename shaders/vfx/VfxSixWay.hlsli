// VfxSixWay.hlsli - UnoFX 煙の 6-way ライティング（W1-BD7）。VfxFlipbook.h の SixWayLight と同じ式。
//   RLU: R = 右から、G = 左から、B = 上から、A = 不透明度。DBF: R = 下から、G = 奥（背面）から、B = 手前（カメラ側）から、A = 浸食場。
//   L = ビルボード空間（+x 右、+y 上、+z カメラ側）の単位ベクトル。表面から光へ。
#ifndef UNO_VFX_SIXWAY_HLSLI
#define UNO_VFX_SIXWAY_HLSLI
#include "VfxFlipbook.hlsli"

float FbSixWayLight(float3 L, float4 rlu, float4 dbf)
{
    return L.x * L.x * (L.x > 0.0 ? rlu.x : rlu.y)
         + L.y * L.y * (L.y > 0.0 ? rlu.z : dbf.x)
         + L.z * L.z * (L.z > 0.0 ? dbf.z : dbf.y);
}

float FbSixWayAmbient(float4 rlu, float4 dbf)
{
    return (rlu.x + rlu.y + rlu.z + dbf.x + dbf.y + dbf.z) * (1.0 / 6.0);
}

FbShaded FbShadeSmoke(float4 rlu, float4 dbf, float3 L, float tLife, float3 albedo, float3 ambient, float3 sun, float2 erode)
{
    float th = FbErodeThreshold(tLife, erode.x, erode.y);
    FbShaded o;
    o.a = saturate(rlu.w) * FbErosion(dbf.w, th, erode.y);
    o.rgb = albedo * (ambient * FbSixWayAmbient(rlu, dbf) + sun * FbSixWayLight(L, rlu, dbf));
    return o;
}
#endif
