#pragma once
// ShadingV2Math.h — UNO_SHADING_V2 のマイクロシャドウ / GTSO / 地平線オクルージョン（W1-C5 / LS-01 x MH-3a）の
//   CPU 側の写しと、PfSlot::ShadingV2 への詰め方。ヘッダだけ（GPU も D3D も要らない）。
//
//   ★正本は shaders/forward/Lighting.hlsli の UnoV2* 関数（-D UNO_SHADING_V2=1 のときだけ存在する）。
//     式を変えるときは両方を同時に直す（tests/shading_v2_math_test.cpp が CPU 側の性質を固定している）。
//
//   PfSlot::ShadingV2（_clusterReserved[2]）: x = shadingFlags（asuint）、y = microShadowK、z = horizonK、w = 予約（C6 / MH-2）。
//   shadingRevision != 2 のときは x = 0（全機能 OFF = V1 と同じ命令列の結果）。V2 の .cso でも x = 0 なら V1 と同じ絵になる。

#include "renderer/ShadingSettings.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace dx12e::shadingv2
{

// shadingFlags のビット（HLSL の UNO_V2_FLAG_* と 1 対 1）。
inline constexpr std::uint32_t kFlagMicroShadow = 1u << 0;
inline constexpr std::uint32_t kFlagHorizonOcc  = 1u << 1;
inline constexpr std::uint32_t kFlagGtso        = 1u << 2;
inline constexpr std::uint32_t kFlagMask        = 0xFFu;   // シェーダは下位 8 ビットだけ読む（上は将来の機能のために空ける）

inline constexpr float kMaxStrength = 2.0f;   // microShadowK / horizonK の上限（0 = 効果なし相当、1 = 標準）

inline float ClampStrength(float k)
{
    if (!(k >= 0.0f)) return 0.0f;   // NaN と負は 0
    return std::min(k, kMaxStrength);
}

// ★shadingFlags（PfSlot::ShadingV2 の x）を書くのはこの PackFlags だけ。後続の項目（LS-03 screenShadowMode、MH-6 weatherOn、
//   MH-14 emissiveExpComp、LS-08/09 atlasOn / rtLocalOn など）も必ずここへビットを足す（PackSlot が x を毎フレーム全部書き直すため、別の所で書くと消える）。
// 設定 → shadingFlags。shadingRevision が 2 でなければ 0（V2 の PSO が残っている 1 フレームでも V1 と同じ絵になる）。
inline std::uint32_t PackFlags(const ShadingSettings& s)
{
    if (s.shadingRevision != 2) return 0u;
    std::uint32_t f = 0u;
    if (s.microShadow)      f |= kFlagMicroShadow;
    if (s.horizonOcclusion) f |= kFlagHorizonOcc;
    if (s.gtso)             f |= kFlagGtso;
    return f;
}

// _clusterReserved[2] の x / y / z（w は触らない）。
struct SlotValue
{
    float x = 0.0f;   // asfloat(shadingFlags)
    float y = 0.0f;   // microShadowK
    float z = 0.0f;   // horizonK
};

inline SlotValue PackSlot(const ShadingSettings& s)
{
    SlotValue v;
    const std::uint32_t f = PackFlags(s);
    v.x = std::bit_cast<float>(f);
    v.y = ClampStrength(s.microShadowK);
    v.z = ClampStrength(s.horizonK);
    return v;
}

// ---- マイクロシャドウ（Chan 2018, "Material Advances in Call of Duty: WWII"）----
//   saturate(|N.L| + 2 ao^2 - 1)。ao = SSAO x 材質 AO。強さ k は 1 + k (ms - 1) を saturate（k=1 で標準、0 で恒等）。
//   掛ける先は太陽とクラスタライトの radiance（拡散も鏡面も）。IBL / 間接光には掛けない（AO が既に掛かっている）。
inline float MicroShadow(float NoL, float ao, float k)
{
    const float ms = std::clamp(std::fabs(NoL) + 2.0f * ao * ao - 1.0f, 0.0f, 1.0f);
    return std::clamp(1.0f + k * (ms - 1.0f), 0.0f, 1.0f);
}

// ---- 地平線オクルージョン（Filament / Jimenez 2016 の horizon fade）----
//   反射ベクトル R が幾何法線 Ng の面より下へ潜る分を落とす。saturate(1 + k R.Ng)^2。R が面の上なら 1。
//   法線マップで曲げた N の反射が、実際の面の下へ向かって「裏側の空」を映すのを防ぐ。IBL・反射プローブ・SSR の鏡面に掛ける。
inline float HorizonOcclusion(float rDotNg, float k)
{
    const float h = std::clamp(1.0f + k * rDotNg, 0.0f, 1.0f);
    return h * h;
}

// ---- GTSO（Ground-Truth-based Specular Occlusion。Jimenez 2016 "Practical Real-Time Strategies for Accurate Indirect Occlusion"）----
//   AO から可視円錐（半角 Av。cos^2 Av = 1 - ao）、粗さから反射ローブ円錐（半角 As。cos As = 2^(-3.32193 alpha^2)、alpha = roughness^2）を作り、
//   2 つの球面キャップの重なりの面積で鏡面の可視率を求める（Oat & Sander 2007 の smoothstep 近似）。
//   ★ここでの違い（意図した差）: 重なりを「同じローブと半球（cos = 0）の重なり」で割る。ao = 1（遮蔽なし）なら必ず 1 になり、
//     地平線で切れる分は HorizonOcclusion / スプリットサムの LUT が持つ（二重に暗くしない。掠める角度のフレネルの反射を落とさない）。
//   ベントノーマルを持たないため、軸は N と R の中間（近似）。NoR = N.R。
inline float SphericalCapsIntersection(float cosCap1, float cosCap2, float cosDistance)
{
    const float r1 = std::acos(std::clamp(cosCap1, -1.0f, 1.0f));
    const float r2 = std::acos(std::clamp(cosCap2, -1.0f, 1.0f));
    const float d  = std::acos(std::clamp(cosDistance, -1.0f, 1.0f));
    if (std::min(r1, r2) <= std::max(r1, r2) - d) return 1.0f - std::max(cosCap1, cosCap2);   // 片方がもう片方に含まれる
    if (r1 + r2 <= d) return 0.0f;                                                              // 離れている
    const float delta = std::fabs(r1 - r2);
    const float x = 1.0f - std::clamp((d - delta) / std::max(r1 + r2 - delta, 1.0e-4f), 0.0f, 1.0f);
    const float area = x * x * (3.0f - 2.0f * x);   // smoothstep(0, 1, x)
    return area * (1.0f - std::max(cosCap1, cosCap2));
}

inline float Gtso(float NoR, float ao, float roughness)
{
    const float alpha = std::max(roughness * roughness, 1.0e-4f);
    const float cosAs = std::exp2(-3.32193f * alpha * alpha);
    const float cosAv = std::sqrt(std::clamp(1.0f - ao, 0.0f, 1.0f));
    // 近似ベントノーマル: 軸を N と R の中間へ傾ける（真のベントノーマルが入る LS-07 まで）。軸と R の cos = sqrt((1 + NoR) / 2)。
    //   N のままだと掠める角度（NoR -> 0）で R が可視円錐の外へ出て so -> 0 になり、球の縁が黒くなる。
    const float cosD = std::sqrt(std::clamp(0.5f + 0.5f * NoR, 0.0f, 1.0f));
    const float iv = SphericalCapsIntersection(cosAv, cosAs, cosD);
    const float i0 = SphericalCapsIntersection(0.0f, cosAs, cosD);
    return i0 > 1.0e-4f ? std::clamp(iv / i0, 0.0f, 1.0f) : 1.0f;
}

} // namespace dx12e::shadingv2
