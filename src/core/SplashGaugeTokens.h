#pragma once

// ===========================================================================
// 起動画面 Graphite Gauge の見た目の定数表（W1-BD3。ヘッダオンリー。Win32 に依存しない）
// ---------------------------------------------------------------------------
// 値の出典: GG（Graphite Gauge）v3.1 の final/tokens.json（リポジトリには assets/editor/theme/gg/tokens.json として取り込み済み）
//   - 色: color.surface / color.text / color.accent / color.viewport.gridMinor
//   - 黄の予算: yellowBudget.boot（B1）。寸法: identity/gg/brand/boot.py の Geo（段階 L / M / S）と boot_impl_spec.md §1〜§2
// ここは BD レーンが持つ表で、tests/splash_gauge_renderer_test.cpp が tokens.json の色と突き合わせる（W2 で GG の生成物との一致テストへ移す）。
// 時刻は持たない（時刻の正は SplashCalibrateEvents.h と SplashGaugeMotion.h）。
// 寸法は論理 px（DIP。段階 L の 1600x900 を基準）。描画器が dpi（と fit）を掛けて物理 px にする。
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace dx12e::splash
{

struct GaugeRgba
{
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    constexpr std::uint32_t Rgb() const { return (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b; }
};

constexpr GaugeRgba GaugeHex(std::uint32_t rgb, std::uint8_t a = 255)
{
    return GaugeRgba{ static_cast<std::uint8_t>((rgb >> 16) & 0xFF), static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
                      static_cast<std::uint8_t>(rgb & 0xFF), a };
}

// 起動画面が使う色（トークン名 = tokens.json のパス）。これ以外の色は描かない（黄の検査の「トークン外の色 0」の根拠）。
struct GaugeColors
{
    GaugeRgba groove      = GaugeHex(0x0A0A09);          // color.surface.groove
    GaugeRgba bg0         = GaugeHex(0x131312);          // color.surface.bg0
    GaugeRgba bg2         = GaugeHex(0x222120);          // color.surface.bg2
    GaugeRgba border      = GaugeHex(0x2C2B28);          // color.surface.border
    GaugeRgba borderStrong = GaugeHex(0x46443F);         // color.surface.borderStrong
    GaugeRgba inLineHover = GaugeHex(0x8A857C);          // color.surface.inLineHover（終わった工程の上線）
    GaugeRgba t1          = GaugeHex(0xE6E2DA);          // color.text.t1
    GaugeRgba t2          = GaugeHex(0xCFCAC1);          // color.text.t2
    GaugeRgba t3          = GaugeHex(0xB4AFA6);          // color.text.t3
    GaugeRgba t4          = GaugeHex(0xA39E94);          // color.text.t4
    GaugeRgba accent      = GaugeHex(0xF0C23B);          // color.accent.accent（計測イエロー。線・トンボ・点だけ）
    GaugeRgba gridMinor   = GaugeHex(0xE6E2DA, 0x0E);    // color.viewport.gridMinor（方眼 40DIP。不透明度 0.55 を重ねる）
};
inline constexpr GaugeColors kGaugeColors{};

// 黄の予算（yellowBudget.boot B1）。起動画面で黄を許すのは次の要素だけ。
enum class GaugeYellow : std::uint8_t { Needle, LockTriangle, CornerTombo, Hub, StepTopLine, NeedleGhost };
inline constexpr std::array<GaugeYellow, 6> kGaugeYellowAllowed = {
    GaugeYellow::Needle, GaugeYellow::LockTriangle, GaugeYellow::CornerTombo, GaugeYellow::Hub, GaugeYellow::StepTopLine,
    GaugeYellow::NeedleGhost };
// 黄の画素比の上限（boot_impl_spec §7 の受け入れ条件: 黄の画素比 <= 1.0%、黄の面（9x9 の侵食後）= 0、トークン外の色 0）。
// tokens.json の yellowBudget.boot には数値が無い（文章のみ）ので、数値の出どころは boot_impl_spec §7。黄の判定は
// yellowBudget.yellowZone（OKLCH で C >= 0.07 かつ 75 <= h <= 105）。検査の実装は tools/（NB5 の splash_golden.py）に依頼する。
inline constexpr double kGaugeYellowPixelRatioMax = 0.010;
inline constexpr int kGaugeYellowFaceErodePx = 9;

// 段階（boot.py の Geo。k = 1.0 / 0.8 / 0.6）
enum class GaugeTier : std::uint8_t { L, M, S };

// 設計空間（論理 DIP）の寸法一式。段階ごとに k を掛けた値。
struct GaugeGeo
{
    GaugeTier tier = GaugeTier::L;
    float k = 1.0f;
    float W = 1600.f, H = 900.f;              // 窓（fit 前）
    float dcx = 520.f, dcy = 450.f;           // 文字盤の中心
    float K = 2.1f;                           // 主マーク（100 グリッド）の倍率
    float rBezel = 268.f, rTickOut = 250.f, rLabel = 214.f, rArc = 190.f, rDisc = 132.f;
    bool labels = true;                       // 目盛りの数字と中心線（L だけ）
    bool minor = true;                        // 細目盛り 1/100（S では 5 刻みだけ）
    bool grid = true;                         // 方眼 40DIP（S では描かない）
    bool titleBlockFull = true;               // 表題欄の「尺度」「公差」（L だけ）
    bool titleBlock = true;                   // 表題欄そのもの（S では無し。M は図番・状態だけ）
    int verLines = 3;                         // 左下の版情報の行数（L 3 / M 1 / S 1）
    bool stepBands = true;                    // 工程の 5 本の帯（S は「04 / 05 アセット」の 1 行）
};

constexpr GaugeGeo MakeGaugeGeo(GaugeTier t)
{
    GaugeGeo g;
    g.tier = t;
    const float k = t == GaugeTier::L ? 1.0f : (t == GaugeTier::M ? 0.8f : 0.6f);
    g.k = k;
    g.W = static_cast<float>(static_cast<int>(1600.0f * k));
    g.H = static_cast<float>(static_cast<int>(900.0f * k));
    g.dcx = 520.0f * k; g.dcy = 450.0f * k;
    g.K = 2.1f * k;
    g.rBezel = 268.0f * k; g.rTickOut = 250.0f * k; g.rLabel = 214.0f * k; g.rArc = 190.0f * k; g.rDisc = 132.0f * k;
    g.labels = t == GaugeTier::L;
    g.minor = t != GaugeTier::S;
    g.grid = t != GaugeTier::S;
    g.titleBlockFull = t == GaugeTier::L;
    g.titleBlock = t != GaugeTier::S;
    g.verLines = t == GaugeTier::L ? 3 : 1;
    g.stepBands = t != GaugeTier::S;
    return g;
}

// 段階の選択（boot_impl_spec §1.2）。作業領域（論理 DIP = 物理 px / dpiScale）に対して、
// 「幅 <= 作業領域の 60% かつ 高さ <= 60%」を満たす最大の段（L > M > S）。どれも入らなければ S を、作業領域の 80% に収まるよう
// 一様に縮める（fit。下限 0.8。それ以上は縮めず S のまま）。
struct GaugeLayout
{
    GaugeTier tier = GaugeTier::L;
    float fit = 1.0f;                          // S の一様縮小（0.8..1）。描画の倍率 = dpiScale * fit
    GaugeGeo geo = MakeGaugeGeo(GaugeTier::L);
    float WindowW() const { return geo.W * fit; }   // 論理 DIP（dpiScale 前）
    float WindowH() const { return geo.H * fit; }
};

inline GaugeLayout PickGaugeLayout(double workW, double workH)
{
    GaugeLayout out;
    for (GaugeTier t : { GaugeTier::L, GaugeTier::M, GaugeTier::S })
    {
        const GaugeGeo g = MakeGaugeGeo(t);
        if (g.W <= workW * 0.6 && g.H <= workH * 0.6)
        {
            out.tier = t; out.fit = 1.0f; out.geo = g;
            return out;
        }
    }
    const GaugeGeo g = MakeGaugeGeo(GaugeTier::S);
    double fit = 1.0;
    if (g.W > workW * 0.8 || g.H > workH * 0.8) fit = std::min(workW * 0.8 / g.W, workH * 0.8 / g.H);
    out.tier = GaugeTier::S;
    out.fit = static_cast<float>(std::max(0.8, std::min(1.0, fit)));
    out.geo = g;
    return out;
}

// 設計空間の固定寸法（段階 L。M / S は GaugeGeo::k を掛ける。文字は縮めない）
namespace gauge_layout
{
inline constexpr float kFrameInset = 28.0f;            // 枠・方眼の内側
inline constexpr float kGridStep = 40.0f;
inline constexpr float kGridOpacity = 0.55f;
inline constexpr float kTomboInset = 20.0f;
inline constexpr float kTomboArm = 24.0f;              // 腕の長さ（kTomboArm * frac * max(k, 0.8)）
inline constexpr float kTomboWidth = 2.0f;
inline constexpr float kColX = 900.0f, kColW = 600.0f;   // 右の列
inline constexpr float kEyebrowTop = 244.0f, kWordmarkTop = 280.0f, kWordmarkCap = 60.0f, kCatchTop = 372.0f;
inline constexpr float kLoadTop = 490.0f, kStepsTop = 586.0f;
inline constexpr float kNeedleW0 = 4.4f, kNeedleW1 = 2.4f;   // 主マークの指針の幅（根元 / 先端。100 グリッド）
inline constexpr float kNeedleWScaleRoot = 0.66f, kNeedleWScaleTip = 0.95f;   // 文字盤の指針は主マークより細い（boot.py）
inline constexpr float kNeedleHaloW = 3.0f;            // 指針の外周の bg0 の縁
// 残像線の本数・間隔・濃さは SplashGaugeMotion.h の gauge::kTrail*（BD6。純関数 TrailLineCount / TrailLineAt）
}

} // namespace dx12e::splash
