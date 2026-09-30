// FoliageVS.hlsli - 植生 F1: 全パス（本体 / 深度 / 影 / 速度）が共有する頂点変換。
//
// ★本体と深度プリパスで**同じ関数・同じ入力**から位置を作ること。深度がビット一致しないと、本体の LESS_EQUAL が
//   面を落として穴が開く（速度パスも同じ）。風の時刻は今フレームの値。速度パスだけ前フレームの時刻でも評価する。
//
// スロット 1 の per-instance ストリーム（64B。FoliageCull.hlsl の FEmit と同じ並び）:
//   TEXCOORD8..10 = transpose(world) の先頭 3 行（平行移動が .w）/ TEXCOORD11 = uint4(色 RGBA8, シード|風倍率, ディザ lo, ディザ hi)
#ifndef FOLIAGE_VS_HLSLI
#define FOLIAGE_VS_HLSLI

#include "FoliageWind.hlsli"

struct FoliageVSIn
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;       // 風データ（R=はばたき, G=位相, B=曲げ）+ A=AO。★PS へは渡さない
    float2 texCoord    : TEXCOORD0;
    float4 tangent     : TANGENT;
    float4 ir0         : TEXCOORD8;
    float4 ir1         : TEXCOORD9;
    float4 ir2         : TEXCOORD10;
    uint4  ipk         : TEXCOORD11;
};

struct FoliageVertex
{
    float3 wp;        // 風で変位した世界位置（今フレームの時刻）
    float3 wpPrev;    // 同（前フレームの時刻。FOLIAGE_WANT_PREV のときだけ有効）
    float3 nrm;       // 世界法線（正規化）
    float3 tan;       // 世界接線
    float4 color;     // PS へ渡す色（インスタンスの色ばらつき × レイヤー色 × AO）。a = 1
    float2 fade;      // ディザ（lo, hi）。ノイズが lo..hi の断片は捨てる
};

FoliageVertex FoliageTransformVertex(FoliageVSIn i, FoliageWindFrame wf, FoliageWindLayer wl,
                                     float3 layerTint, float aoStrength)
{
    FoliageVertex o;
    const float4 p4 = float4(i.position, 1.0);
    const float3 wp0 = float3(dot(i.ir0, p4), dot(i.ir1, p4), dot(i.ir2, p4));
    const float3 origin = float3(i.ir0.w, i.ir1.w, i.ir2.w);
    const float3x3 B = float3x3(i.ir0.xyz, i.ir1.xyz, i.ir2.xyz);   // 行 = 基底の行。world = B * local
    o.nrm = normalize(mul(B, i.normal));
    o.tan = normalize(mul(B, i.tangent.xyz));
    const float scale = length(float3(i.ir0.x, i.ir1.x, i.ir2.x));

    const uint seed24 = i.ipk.y & 0xFFFFFFu;
    const uint ws8 = i.ipk.y >> 24;
    const float windScale = (ws8 == 0u) ? 1.0 : float(ws8) * (1.0 / 128.0);
    const float phase01 = FoliageHash01(seed24);
    const float3 vc = i.color.rgb;

    o.wp = wp0 + FoliageWindDelta(wp0, origin, o.nrm, i.position.y, vc, phase01, windScale, scale, wf, wl, wf.time);
#ifdef FOLIAGE_WANT_PREV
    o.wpPrev = wp0 + FoliageWindDelta(wp0, origin, o.nrm, i.position.y, vc, phase01, windScale, scale, wf, wl, wf.prevTime);
#else
    o.wpPrev = o.wp;
#endif
    const float4 tint = FoliageUnpackColor8(i.ipk.x);
    o.color = float4(tint.rgb * layerTint * lerp(1.0, i.color.a, aoStrength), 1.0);
    o.fade = float2(asfloat(i.ipk.z), asfloat(i.ipk.w));
    return o;
}

#endif // FOLIAGE_VS_HLSLI
