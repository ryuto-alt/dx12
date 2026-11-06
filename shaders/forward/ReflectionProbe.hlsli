// ReflectionProbe.hlsli - 反射プローブ（UE の Box Reflection Capture 相当）のフォワード側サンプル。
//
//   ReflectionProbeSystem（src/renderer/ReflectionProbeSystem.cpp）が焼いた GGX プリフィルタ済みキューブ配列
//   （t33。1 プローブ = 6 スライス、ミップ = 粗さ）と、プローブの箱の情報（t34）を読む。
//   ForwardShade.hlsli の鏡面 IBL から呼ばれる（フォワード系 PS 全部 = Forward / Skinned / Terrain / Graph /
//   Foliage / VG resolve で共通）。
//
// ★前提: Lighting.hlsli（b1 の reflProbeParams）と g_iblSampler（s2。LINEAR CLAMP・mip 有）が先に宣言済み。
// ★reflProbeParams.x = プローブ数（0 なら t33/t34 を 1 テクセルも読まない＝従来の絵とビット一致）
//                 .y = 最大ミップ（粗さ 1 のミップ番号） .z = Multi-bounce AO（1/0） .w = 予約
// ★ヒープ上のディスクリプタは ClusteredLightCulling のテーブル（slot 11）の +12 / +13。

#ifndef REFLECTION_PROBE_HLSLI
#define REFLECTION_PROBE_HLSLI

struct ReflProbeGpu
{
    float4 posBlend;      // .xyz = 箱の中心（撮影点）  .w = ブレンド幅 [m]
    float4 extIntensity;  // .xyz = 箱の半径（ワールド軸平行） .w = 強さ
    float4 misc;          // .x = 視差補正(1/0)  .y = 配列スライス（プローブ番号。キューブ 1 個 = 6 面の先頭 / 6）
    float4 pad;
};

TextureCubeArray<float4>       g_reflProbeCubes : register(t33);
StructuredBuffer<ReflProbeGpu> g_reflProbes     : register(t34);

#define REFL_PROBE_MAX_BLEND 4u   // 1 画素で重みを取るプローブの上限（CPU が優先度順に並べて送る）

// ワールド位置 worldPos から方向 R（反射ベクトル）へ引いた放射輝度。
//   戻り値 .rgb = 重み付きの放射輝度（Σ w_i * color_i * intensity_i。正規化しない）
//          .a   = 重みの合計（0..1）。呼び出し側は「既存経路 * (1 - a) + .rgb」で合成する。
// roughness → ミップは線形（ReflectionProbeSystem のプリフィルタと同じ対応: mip m = roughness m/(N-1)）。
float4 SampleReflectionProbes(float3 worldPos, float3 R, float roughness)
{
    float3 acc    = float3(0.0, 0.0, 0.0);
    float  remain = 1.0;
    const uint count = min(uint(reflProbeParams.x), 16u);
    const float mip  = saturate(roughness) * reflProbeParams.y;
    uint used = 0u;
    [loop]
    for (uint i = 0u; i < count && remain > 0.001 && used < REFL_PROBE_MAX_BLEND; ++i)
    {
        const ReflProbeGpu p = g_reflProbes[i];
        const float3 c  = p.posBlend.xyz;
        const float3 he = p.extIntensity.xyz;
        const float3 inside = he - abs(worldPos - c);          // > 0 なら箱の中（軸ごとの縁までの距離）
        if (min(inside.x, min(inside.y, inside.z)) <= 0.0) continue;

        // 箱の縁から内側へ blend 幅で 0 → 1（blend 0 なら箱の中は全部 1）
        const float edge = min(inside.x, min(inside.y, inside.z));
        float w = saturate(edge / max(p.posBlend.w, 1e-3));
        w = min(w, remain);

        float3 dir = R;
        if (p.misc.x > 0.5)
        {
            // 視差補正: 画素から R へ飛ばして箱の壁に当たる点を、撮影点から見た方向に直す。
            const float3 Rs = R + (abs(R) < 1e-5) * 1e-5;
            const float3 t1 = (c + he - worldPos) / Rs;
            const float3 t2 = (c - he - worldPos) / Rs;
            const float3 tf = max(t1, t2);
            const float  d  = max(min(tf.x, min(tf.y, tf.z)), 0.0);
            dir = (worldPos + R * d) - c;
        }
        const float3 col = g_reflProbeCubes.SampleLevel(g_iblSampler, float4(dir, p.misc.y), mip).rgb;
        acc    += col * (w * p.extIntensity.w);
        remain -= w;
        ++used;
    }
    return float4(acc, 1.0 - remain);
}

// Multi-bounce（Jimenez 2016, "Practical Real-Time Strategies for Accurate Indirect Occlusion"）。
// 暗い隙間でも明るい色の面は周囲からの反射光で完全には暗くならない。visibility = AO、albedo 依存の 3 次式。
float3 UnoGtaoMultiBounce(float visibility, float3 albedo)
{
    const float3 a =  2.0404 * albedo - 0.3324;
    const float3 b = -4.7951 * albedo + 0.6417;
    const float3 c =  2.7552 * albedo + 0.6903;
    const float3 x = float3(visibility, visibility, visibility);
    return max(x, ((x * a + b) * x + c) * x);
}

#endif // REFLECTION_PROBE_HLSLI
