#pragma once

// 時間方向アップスケーラ（off / taa / dlss）の純関数（ヘッダオンリー・GPU 非依存）。
// UpscalerPass / Application / MCP から使い、tests/upscaler_math_test.cpp で単体検証する。
//
// 用語:
//   表示解像度（display / out）… アップスケーラの出力 = ポスト後段（ブルーム・DoF・uber …）の解像度
//   内部解像度（render）       … ラスタライズ・G-Buffer・深度・速度・SSAO/SSR/GI/RT/フォグの解像度

#include "core/Types.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dx12e::upscale
{

// TaaSettings::upscaler の値（シーン JSON / MCP の文字列と 1:1）。
enum class Kind : int
{
    Taa  = 0,   // エンジン内蔵 TAA（従来。空間レンダースケールと組み合わせる）
    Dlss = 1,   // NVIDIA DLSS Super Resolution / DLAA（使えなければ実行時に Taa へ縮退）
};

// DLSS の品質モード（内部解像度 / 表示解像度の比）。
enum class DlssQuality : int
{
    Dlaa        = 0,   // 100%（アップスケールしない AA）
    Quality     = 1,   // 66.7%
    Balanced    = 2,   // 58%
    Performance = 3,   // 50%
};

inline const char* QualityName(DlssQuality q)
{
    switch (q)
    {
    case DlssQuality::Dlaa:        return "dlaa";
    case DlssQuality::Quality:     return "quality";
    case DlssQuality::Balanced:    return "balanced";
    case DlssQuality::Performance: return "performance";
    }
    return "quality";
}

inline bool ParseQuality(const char* s, DlssQuality& out)
{
    if (!s) return false;
    if (std::strcmp(s, "dlaa") == 0)        { out = DlssQuality::Dlaa;        return true; }
    if (std::strcmp(s, "quality") == 0)     { out = DlssQuality::Quality;     return true; }
    if (std::strcmp(s, "balanced") == 0)    { out = DlssQuality::Balanced;    return true; }
    if (std::strcmp(s, "performance") == 0) { out = DlssQuality::Performance; return true; }
    return false;
}

inline DlssQuality ClampQuality(int v)
{
    return static_cast<DlssQuality>(std::clamp(v, 0, 3));
}

// 品質モードの公称倍率（NGX の「最適な内部解像度」が取れないときの代わり。値は DLSS の公表値）。
inline float NominalScale(DlssQuality q)
{
    switch (q)
    {
    case DlssQuality::Dlaa:        return 1.0f;
    case DlssQuality::Quality:     return 2.0f / 3.0f;
    case DlssQuality::Balanced:    return 0.58f;
    case DlssQuality::Performance: return 0.5f;
    }
    return 2.0f / 3.0f;
}

// 公称倍率から内部解像度を出す（四捨五入・最小 16px・表示解像度を超えない）。
inline void NominalRenderSize(u32 outW, u32 outH, DlssQuality q, u32& rw, u32& rh)
{
    const double s = NominalScale(q);
    rw = static_cast<u32>(std::lround(static_cast<double>(outW) * s));
    rh = static_cast<u32>(std::lround(static_cast<double>(outH) * s));
    rw = std::clamp(rw, (std::min)(16u, outW), (std::max)(outW, 1u));
    rh = std::clamp(rh, (std::min)(16u, outH), (std::max)(outH, 1u));
}

// ジッタの位相数。目安は 8 ×（表示/内部）²（NVIDIA の DLSS ガイド・AMD FSR と同じ考え方）。
// DLAA は 8、Quality(1.5x) は 18、Balanced(1.72x) は 24、Performance(2x) は 32。上限 64。
inline u32 JitterPhaseCount(u32 renderW, u32 outW)
{
    if (renderW == 0 || outW == 0) return 8;
    const double r = static_cast<double>(outW) / static_cast<double>(renderW);
    const u32 n = static_cast<u32>(std::ceil(8.0 * r * r - 1e-9));
    return std::clamp(n, 8u, 64u);
}

// テクスチャのミップバイアス = log2(内部 / 表示)。内部 >= 表示（DLAA / ネイティブ）なら 0。
// アップスケール時は負（細かいミップを選ぶ）。アップスケーラが時間方向に積むので、
// 表示解像度ぶんのテクスチャ詳細を内部解像度のサンプルで拾う必要がある。
inline float TextureMipBias(u32 renderW, u32 outW)
{
    if (renderW == 0 || outW == 0 || renderW >= outW) return 0.0f;
    return static_cast<float>(std::log2(static_cast<double>(renderW) / static_cast<double>(outW)));
}

// NDC のジッタ（TaaJitter.h の ComputeTaaJitterNdc と同じ規約: jx = 2*hx/W, jy = -2*hy/H）を
// 内部解像度のピクセル単位（x 右 + / y 下 +）へ戻す。DLSS の InJitterOffset はこの単位。
struct JitterPx { float x; float y; };
inline JitterPx NdcJitterToPixels(float jxNdc, float jyNdc, u32 renderW, u32 renderH)
{
    return { jxNdc * 0.5f * static_cast<float>(renderW), -jyNdc * 0.5f * static_cast<float>(renderH) };
}

} // namespace dx12e::upscale
