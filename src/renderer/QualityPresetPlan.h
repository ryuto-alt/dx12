#pragma once
// ===========================================================================
// 品質プリセットの「計画」（W1-C7 RM-C3。ヘッダのみ・GPU 不要・Scene を持たない純関数）。
//   BuildQualityPlan(preset, caps) が、そのプリセットで「どの機能を入れるか」を機能の有無（bool / enum）で返す。
//   ★数値（予算・解像度の倍率・VFX の倍率・最大粒子数）は 1 つも書かない。値は QualityPreset.h
//     （budgets/quality_presets.json から生成した QualityPresetTable.inc）から引く。ここに書くのは
//     「Epic は RT 太陽影を使う」のような構成の選択だけ。
//   caps（DXR / DLSS の有無）で縮退する: DXR が無ければ RT の行を縮退の手段へ置き換え、DLSS が無ければ
//   合否の列を TAA native（無ければ TAAU）へ切り替える。縮退した行は degrades[] に載る（MCP の結果に出す）。
//   適用（Scene への書き込み）は core/QualityPresetScene.h。
// ===========================================================================
#include "renderer/QualityPreset.h"
#include "renderer/UpscalerMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace dx12e
{

// この実行で使える機能（Application が埋める）。
struct QualityCaps
{
    bool dxr  = false;   // 6 段ゲートを全部通った DXR（Application::m_dxrEnabled）
    bool dlss = false;   // DLSS が使える（NGX が初期化できた）
};

enum class QualityUpscaler : u8 { Off, Taa, Dlss };

// 縮退の 1 行。row は予算の行名（QualityPresetTable.inc と同じ）、text は表示用の説明（静的文字列）。
struct QualityDegradeNote
{
    const char* row  = "";
    const char* text = "";
};

struct QualityPlan
{
    QualityPresetId id   = QualityPresetId::High;
    QualityCaps     caps = {};

    // ---- アップスケーラ ----
    QualityUpscaler upscaler    = QualityUpscaler::Off;
    u8              dlssQuality = 1;      // upscale::DlssQuality（upscaler == Dlss のときだけ意味がある）
    bool            setRenderScale = false;   // true のとき空間レンダースケールを renderScale にする（予算の行があるプリセットだけ）
    float           renderScale    = 1.0f;

    // ---- 予算の合否の列（DLSS があれば Dlss、無ければ TaaNative、それも無ければ Taau）----
    QualityColumn   column = QualityColumn::Dlss;

    // ---- シェーディングと影 ----
    u8   shadingRevision = 1;             // 1 = V1（従来）/ 2 = UNO_SHADING_V2
    bool pcss            = false;         // ShadowPcssSettings::enabled
    bool epicDynamicCsm  = false;         // Epic の動的 CSM（2048 D16 x3 + pancaking）
    bool contactShadow   = false;
    // ---- スクリーン空間と GI ----
    bool ssao = false;                    // GTAO / SSAO
    bool ssr  = false;
    bool ssgi = false;                    // DDGI が有効なら OFF（BUG-04: 縞とホタルを見せない）
    bool ddgi = false;
    // ---- RT（caps.dxr が無ければ全部 false）----
    bool rtShadow        = false;
    bool rtReflect       = false;
    bool rtReflectHalfRes = true;

    // ---- 縮退（DXR / DLSS が無くて置き換えた行）----
    static constexpr size_t kMaxDegrades = 8;
    std::array<QualityDegradeNote, kMaxDegrades> degrades{};
    u8 degradeCount = 0;

    void AddDegrade(const char* row, const char* text)
    {
        if (degradeCount < kMaxDegrades) degrades[degradeCount++] = { row, text };
    }
};

namespace qualityplan
{
// 列が合否の対象か（Gate のときだけ。GateAfterW2BD / Record は記録のみ）。
inline bool IsGated(const QualityPreset& p, QualityColumn c)
{
    return p.hasBudget && p.Column(c).present && p.Column(c).gate == QualityGate::Gate;
}

// 空間スケールが DLSS の品質モードのどれに最も近いか（予算表の renderScale から決める。0.6667 → Quality、1.0 → DLAA）。
inline u8 DlssQualityForScale(float scale)
{
    using Q = upscale::DlssQuality;
    const Q all[] = { Q::Dlaa, Q::Quality, Q::Balanced, Q::Performance };
    Q best = Q::Quality;
    float bestD = 1e9f;
    for (Q q : all)
    {
        const float d = std::fabs(upscale::NominalScale(q) - scale);
        if (d < bestD) { bestD = d; best = q; }
    }
    return static_cast<u8>(best);
}

// 行名が予算の行にあるか（縮退の表の取りこぼしを test で検出するため）。
inline bool HasBudgetRow(QualityPresetId id, const char* row)
{
    size_t n = 0;
    const QualityBudgetRow* rows = GetBudgetRows(n);
    for (size_t i = 0; i < n; ++i)
        if (rows[i].preset == id && std::strcmp(rows[i].row, row) == 0) return true;
    return false;
}

// 予算表の縮退の文（.inc の UNO_QP_DEGRADE）。無ければ nullptr。
inline const char* TableDegradeText(QualityPresetId id, const char* row)
{
    size_t n = 0;
    const QualityDegradeRow* d = GetDegradeRows(n);
    for (size_t i = 0; i < n; ++i)
        if (d[i].preset == id && std::strcmp(d[i].row, row) == 0) return d[i].text;
    return nullptr;
}
} // namespace qualityplan

inline QualityPlan BuildQualityPlan(QualityPresetId id, QualityCaps caps)
{
    QualityPlan p;
    p.id   = (static_cast<size_t>(id) < static_cast<size_t>(QualityPresetId::Count)) ? id : QualityPresetId::Low;
    p.caps = caps;
    const QualityPreset& pre = GetPreset(p.id);

    // ---- 構成の選択（機能の有無。数値は書かない）----
    switch (p.id)
    {
    case QualityPresetId::Low:
        p.upscaler = QualityUpscaler::Off;
        break;
    case QualityPresetId::Medium:
        p.upscaler = QualityUpscaler::Taa;
        p.pcss = true; p.contactShadow = true; p.ssao = true;
        break;
    case QualityPresetId::High:
        // 従来のシェーディング（V1）のまま。RT は GI（DDGI）だけ。
        p.upscaler = caps.dlss ? QualityUpscaler::Dlss : QualityUpscaler::Taa;
        p.pcss = true; p.contactShadow = true; p.ssao = true; p.ssr = true;
        p.ddgi = caps.dxr;
        break;
    case QualityPresetId::Epic:
    case QualityPresetId::Cinematic:
        p.upscaler = caps.dlss ? QualityUpscaler::Dlss : QualityUpscaler::Taa;
        p.shadingRevision = 2;
        p.pcss = true; p.epicDynamicCsm = true; p.contactShadow = true; p.ssao = true; p.ssr = true;
        p.rtShadow  = caps.dxr;
        p.rtReflect = caps.dxr;
        p.ddgi      = caps.dxr;
        if (p.id == QualityPresetId::Cinematic)
        {
            p.rtReflectHalfRes = false;   // フル解像度の RT 反射（AO は SSAO のまま。RT-AO は仕様に無い）
        }
        break;
    case QualityPresetId::Count: break;
    }
    p.ssgi = (p.id != QualityPresetId::Low && p.id != QualityPresetId::Medium) && !p.ddgi;   // BUG-04: DDGI があれば SSGI は OFF

    // ---- 予算の列とアップスケーラの値（表から引く）----
    if (pre.hasBudget)
    {
        // 合否の列: DLSS を使うなら dlss、使わないなら taaNative（無ければ taau）。
        const bool useDlss = p.upscaler == QualityUpscaler::Dlss;
        const QualityColumn fallback = pre.Column(QualityColumn::TaaNative).present ? QualityColumn::TaaNative : QualityColumn::Taau;
        p.column = useDlss ? QualityColumn::Dlss : fallback;
        if (useDlss) p.dlssQuality = qualityplan::DlssQualityForScale(pre.Column(QualityColumn::Dlss).renderScale);
        // 空間スケールは DLSS を使わない側の列の値（DLSS のときも、DLSS を切ったときに備えて同じ値へ揃える）。
        p.setRenderScale = true;
        p.renderScale    = pre.Column(fallback).renderScale;
    }

    // ---- 縮退（DXR / DLSS が無いとき）----
    if (!caps.dxr)
    {
        const bool wantsRt = (p.id == QualityPresetId::Epic || p.id == QualityPresetId::Cinematic || p.id == QualityPresetId::High);
        if (wantsRt)
        {
            if (p.id != QualityPresetId::High)
            {
                p.AddDegrade("Raytracing",  "DXR が無い: TLAS を作らない（RT の各行を縮退へ置き換える）");
                p.AddDegrade("RtShadow",    "DXR が無い: RT 太陽影の代わりに動的 CSM が静的な物も描く");
                p.AddDegrade("RtReflect",   "DXR が無い: RT 反射は使わず SSR にする");
            }
            p.AddDegrade("Ddgi", "DXR が無い: DDGI の代わりに SSGI を使う");
            if (p.id == QualityPresetId::Cinematic)
            {
                // Cinematic だけの DXR 専用の行。RtSettings に機能が出るまでは構成に現れないが、表の行は縮退として示す。
                // 文は DXR が無い場合専用（表の文は「予算超過時」の縮退なので流用しない）。
                p.AddDegrade("RtGi",          "DXR が無い: RT 短距離 GI は使わず SSGI のまま");
                p.AddDegrade("RtLocalShadow", "DXR が無い: RT ローカル影は使わずシャドウアトラスのまま");
            }
        }
    }
    if (!caps.dlss && p.upscaler == QualityUpscaler::Taa && pre.hasBudget)
    {
        if (p.column == QualityColumn::TaaNative)
            p.AddDegrade("Upscale", "DLSS が無い: TAA（空間スケールは TAA native の列。合否の対象ではなく記録のみ）");
        else
            p.AddDegrade("Upscale", "DLSS が無い: TAA（空間スケールは TAAU の列）");
    }
    return p;
}

// 実効 VRAM 上限（GB）。hasVramCap なら min(capGb, デバイスの予算 - marginGb)、無ければ デバイスの予算 - marginGb。
inline float EffectiveVramCapGb(const QualityPreset& p, float deviceBudgetGb)
{
    const float byDevice = deviceBudgetGb - p.vramMarginGb;
    return p.hasVramCap ? (std::min)(p.vramCapGb, byDevice) : byDevice;
}

// 予算の行の合計（preset × column）。T-00 / quality_preset_test が葉の合計の上限を調べる。
inline float SumBudgetRows(QualityPresetId id, QualityColumn column)
{
    size_t n = 0;
    const QualityBudgetRow* rows = GetBudgetRows(n);
    float sum = 0.0f;
    for (size_t i = 0; i < n; ++i)
        if (rows[i].preset == id && rows[i].column == column) sum += rows[i].ms;
    return sum;
}

} // namespace dx12e
