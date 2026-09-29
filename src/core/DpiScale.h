#pragma once

// ===========================================================================
// 表示倍率（DPI）の純ロジック + 検証用オーバーライド（ヘッダオンリー。Win32 にも GPU にも依存しない）
// ---------------------------------------------------------------------------
// ・Windows の表示倍率は 100% / 125% / 150% / 175% / 200% / 225% / 250% / 300% ...（DPI = 96 × 倍率）。
//   プロセスは Per-Monitor V2（resources/dx12.manifest + main.cpp）なので、ウィンドウは物理 px で動き、
//   ImGui の座標も物理 px。エディタ UI の寸法は「論理 px（100% 表示の px）」で書き、
//   theme::Px()（editor/EditorTheme.h）で物理 px に直す。
// ・--dpi-scale <0.75〜3.0> はエディタ専用の検証用オーバーライド。指定時は OS の倍率を無視して
//   その倍率で全体を描く（Windows の設定は一切触らない）。
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace dx12e::dpi
{

constexpr float kMinScale = 0.75f;
constexpr float kMaxScale = 3.0f;
constexpr unsigned kBaseDpi = 96;

// 倍率を許容範囲へ丸める（NaN / 0 / 負は 1.0）。
inline float ClampScale(float s)
{
    if (!(s > 0.0f)) return 1.0f;
    return (std::min)(kMaxScale, (std::max)(kMinScale, s));
}

// DPI（96 = 100%）→ 倍率。0 は 1.0。
inline float DpiToScale(unsigned dpi)
{
    return dpi == 0 ? 1.0f : static_cast<float>(dpi) / static_cast<float>(kBaseDpi);
}
inline unsigned ScaleToDpi(float scale)
{
    return static_cast<unsigned>(std::lround(ClampScale(scale) * static_cast<float>(kBaseDpi)));
}

// "1.5" / "150%" / "1.25" を倍率へ。範囲外（0.75〜3.0 の外）・数字でない文字列は false。
inline bool ParseScale(std::string_view text, float& out)
{
    std::string s(text);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.empty()) return false;
    bool percent = false;
    if (s.back() == '%') { percent = true; s.pop_back(); }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    double scale = percent ? v / 100.0 : v;
    // "150" のように % を付けずに 100 以上の数を書いた場合はパーセント指定とみなす（0.75〜3.0 では曖昧さが無い）
    if (!percent && scale >= 50.0) scale /= 100.0;
    if (!(scale >= static_cast<double>(kMinScale) - 1e-6 && scale <= static_cast<double>(kMaxScale) + 1e-6)) return false;
    out = static_cast<float>(scale);
    return true;
}

// ---- 検証用オーバーライド（--dpi-scale。0 = OS の倍率に従う）----
inline float& OverrideRef() { static float v = 0.0f; return v; }
inline float  Override() { return OverrideRef(); }
inline bool   HasOverride() { return OverrideRef() > 0.0f; }
inline void   SetOverride(float scale) { OverrideRef() = scale > 0.0f ? ClampScale(scale) : 0.0f; }

// 実際に使う倍率（オーバーライドがあればそれ、無ければ OS の倍率）。
inline float EffectiveScale(float osScale)
{
    return HasOverride() ? Override() : ClampScale(osScale);
}

// 論理 px → 物理 px（丸め）。scale 1.0 は恒等。
inline int LogicalToPhysical(int logical, float scale)
{
    return scale == 1.0f ? logical : static_cast<int>(std::floor(static_cast<double>(logical) * scale + 0.5));
}

} // namespace dx12e::dpi
