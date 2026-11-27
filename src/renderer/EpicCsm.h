#pragma once
// EpicCsm.h — W1-C6 LS-02a: Epic の太陽影（RT が静的な物、動的 CSM が動的な物）の構成の純関数・定数。
//   ヘッダだけ（GPU / D3D 不要）。CPU の写しの性質は tests/shadow_bias_math_test.cpp が固定する。
//
//   構成: 2048 の D16 × 3 カスケード、pancaking（影 PSO の DepthClipEnable=FALSE と、CPU / VG / 植生のカリングから near 面を外す）、
//         受光側の法線オフセット（V2 の中だけ）、影を描く側のバイアス縮小（シーン全体が shadingRevision=2 のときだけ）、Gather の PCSS。
//   旧経路（2048 D32 × 4 の PCF）は既定のまま（epicDynamicCsm=false）。

#include "core/Types.h"
#include "renderer/Frustum.h"

#include <DirectXMath.h>
#include <dxgiformat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dx12e::epiccsm
{

inline constexpr u32   kLegacyCascades = 4;       // = Application::kNumCascades
inline constexpr u32   kCascades       = 3;       // Epic の動的 CSM
inline constexpr f32   kLegacyFarMax   = 400.0f;  // 旧経路のカスケード遠端の上限（ApplicationRender.cpp の従来値）
inline constexpr f32   kFarMax         = 200.0f;  // Epic の動的 CSM の遠端（遠景の静的物は RT が担当）

inline constexpr int   kLegacyDepthBias = 8000;   // 影 PSO の従来値（D32_FLOAT）
inline constexpr f32   kLegacySlopeBias = 2.0f;

inline constexpr u32 CascadeCount(bool epic) { return epic ? kCascades : kLegacyCascades; }
inline f32 CascadeFar(f32 camFar, bool epic) { return (std::min)(camFar, epic ? kFarMax : kLegacyFarMax); }

// ---- 深度のフォーマット（リソースは TYPELESS、DSV と SRV で型を付ける）----
struct DepthFormats { DXGI_FORMAT resource, dsv, srv; };
inline DepthFormats FormatsFor(bool epic)
{
    return epic ? DepthFormats{DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM}
                : DepthFormats{DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT};
}
inline u64 MapBytes(bool epic, u32 size)
{
    return static_cast<u64>(size) * size * CascadeCount(epic) * (epic ? 2u : 4u);
}

// ---- 影を描く側のラスタバイアス ----
// 従来: D32_FLOAT で DepthBias=8000。float 深度の 1 単位は z∈[0.5,1) で 2^-24 なので、深度の差にして 8000 * 2^-24 ≒ 4.77e-4。
// D16_UNORM の 1 単位は 2^-16 の固定なので、同じ深度差は 31 単位。scale（casterBiasScaleV2）はその倍率。
// SlopeScaledDepthBias は深度の傾きへの倍率（単位なし）なのでフォーマットに依らず scale を掛けるだけ。
struct CasterBias { int depthBias; f32 slopeBias; };
inline CasterBias CasterBiasFor(DXGI_FORMAT dsvFormat, f32 scale)
{
    if (!(scale > 0.0f)) scale = 0.0f;
    if (dsvFormat == DXGI_FORMAT_D16_UNORM)
    {
        const double refDelta = static_cast<double>(kLegacyDepthBias) / 16777216.0;   // 8000 * 2^-24
        const int units = static_cast<int>(std::lround(refDelta * 65535.0 * scale));
        return {(std::max)(units, scale > 0.0f ? 1 : 0), kLegacySlopeBias * scale};
    }
    return {static_cast<int>(std::lround(static_cast<double>(kLegacyDepthBias) * scale)), kLegacySlopeBias * scale};
}
// V2 の倍率を掛けるのは「Epic の構成かつシーン全体が shadingRevision=2」のときだけ（V1 の受光面にアクネを出さない）。
inline f32 CasterBiasScale(bool epic, int shadingRevision, f32 casterBiasScaleV2)
{
    return (epic && shadingRevision == 2) ? std::clamp(casterBiasScaleV2, 0.05f, 1.0f) : 1.0f;
}

// ---- pancaking ----
// DepthClipEnable=FALSE のラスタは、ニア / ファーの外の深度を [0,1] へ押し付ける（光に近い側の遮蔽物が z=0 に潰れて残る）。
inline f32 PancakeDepth(f32 z) { return std::clamp(z, 0.0f, 1.0f); }
// カリング用の視錐台から near 面（planes[4]）を外す: 常に内側になる平面 (0,0,0,1) へ差し替える。
inline void DropNearPlane(Frustum& f) { f.planes[4] = DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f); }
inline void DropNearPlane(f32 planes[6][4]) { planes[4][0] = 0.0f; planes[4][1] = 0.0f; planes[4][2] = 0.0f; planes[4][3] = 1.0f; }

// ---- 受光側の法線オフセット（V2 の中だけ）----
// worldPos += Ng * texelWorld[c] * (base + slope * (1 - |N.L|))。シェーダ（ForwardShade.hlsli の CalcShadowV2）と同じ式。
inline f32 ReceiverNormalOffset(f32 texelWorld, f32 base, f32 slope, f32 nDotL)
{
    const f32 c = std::clamp(std::fabs(nDotL), 0.0f, 1.0f);
    return texelWorld * (base + slope * (1.0f - c));
}

// base / slope（0..4）と PCSS のフィルタタップ数（8..32）を PfSlot::ShadingV2.w の 32 ビットへ詰める（各 8 ビット）。
// HLSL: base = (bits & 255) * 4/255、slope = ((bits >> 8) & 255) * 4/255、taps = (bits >> 16) & 255。
inline std::uint32_t PackReceiver(f32 base, f32 slope, u32 taps)
{
    auto q = [](f32 v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 4.0f) * (255.0f / 4.0f))); };
    return q(base) | (q(slope) << 8) | ((std::clamp<u32>(taps, 8u, 32u) & 0xFFu) << 16);
}
struct ReceiverParams { f32 base, slope; u32 taps; };
inline ReceiverParams UnpackReceiver(std::uint32_t bits)
{
    return {static_cast<f32>(bits & 0xFFu) * (4.0f / 255.0f), static_cast<f32>((bits >> 8) & 0xFFu) * (4.0f / 255.0f), (bits >> 16) & 0xFFu};
}

// ---- 使わないカスケードの番兵 ----
// Epic は 3 カスケードなので、配列の 4 本目は「どこを射影しても UV が範囲外 = 影なし（1.0）」になる行列にする。
// （クリップ x = 5 固定。ShadowPcss.hlsli の SampleShadowCascadeCommon が uv 範囲外で 1.0 を返す）
inline DirectX::XMFLOAT4X4 UnusedCascadeMatrix()
{
    DirectX::XMFLOAT4X4 m{};
    m._41 = 5.0f; m._44 = 1.0f;
    return m;
}

} // namespace dx12e::epiccsm
