#pragma once
// ===========================================================================
// 品質プリセット（W1-C1: 口だけ。本体の適用は C7）。
//   値の正は budgets/quality_presets.json。tools/gen_budgets.mjs が生成した QualityPresetTable.inc を
//   ここで読む。**C++ 側に数値を手で書かない**（Low / Medium は予算の行が無いので hasBudget=false）。
//   ヘッダのみ・constexpr。include するのは Types.h だけ（重ヘッダにしない）。
// ===========================================================================
#include "core/Types.h"

#include <array>
#include <cstddef>

namespace dx12e
{

enum class QualityPresetId : u8 { Low, Medium, High, Epic, Cinematic, Count };

// アップスケーラの列（.inc の dlss / taau / taaNative）。
enum class QualityColumn : u8 { Dlss, Taau, TaaNative, Count };

// 列の合否の扱い（.inc の gate）。
enum class QualityGate : u8 { None, Gate, GateAfterW2BD, Record };

// 予算の 1 行（preset × column × 行名）。row は予算の行名（GpuTimer::Scope の識別子とは限らない）。
struct QualityBudgetRow
{
    QualityPresetId preset;
    QualityColumn   column;
    const char*     row;
    float           ms;
};

// 予算の行が指す GpuTimer の葉（.inc の UNO_QP_LEAF。1 行に複数ありうる）。C7 の葉の合計検査・T-00 が使う。
struct QualityLeafRow
{
    QualityPresetId preset;
    const char*     row;
    const char*     leaf;
};

// 縮退の表（行ごとに 1 つ。列をまたいで共通）。
struct QualityDegradeRow
{
    QualityPresetId preset;
    const char*     row;
    const char*     text;
};

struct QualityUpscalerColumn
{
    bool        present     = false;   // その列が .inc にあるか
    QualityGate gate        = QualityGate::None;
    float       renderScale = 1.0f;
    float       totalMs     = 0.0f;
};

struct QualityPreset
{
    QualityPresetId id        = QualityPresetId::Low;
    const char*     key       = "";    // 生成表の識別子（例 "epic_1080p60"）。Low / Medium は MCP 名の "low" / "medium"
    const char*     label     = "";    // 表示名（"Epic" など）
    const char*     mcpName   = "";    // MCP / --quality の名前（小文字）
    bool            hasBudget = false; // 生成表に行があるか（Low / Medium は無い）

    // PRESET
    float targetFps = 0.0f;
    u32   outW = 0, outH = 0;
    float p95Ms = 0.0f, p99Ms = 0.0f;
    // VFX
    // 注意: hasBudget=false（Low / Medium）では maxParticles=0 は「上限なし」の意味。0 を上限として使わないこと
    //       （使う側は HasParticleCap() を見る）。emissionMultiplier=1.0 は「そのまま」。
    float emissionMultiplier = 1.0f;
    u32   maxParticles       = 0;
    float vfxSimMs = 0.0f, vfxDrawMs = 0.0f, vfxPeakMs = 0.0f;
    // VRAM（hasVramCap=false なら固定上限なし。実効上限 = min(capGb, 予算 - marginGb)）
    bool  hasVramCap = false;
    float vramCapGb = 0.0f, vramMarginGb = 0.0f, vramPrecheckGb = 0.0f;
    // アップスケーラの列
    std::array<QualityUpscalerColumn, static_cast<size_t>(QualityColumn::Count)> upscaler{};

    bool HasParticleCap() const { return hasBudget && maxParticles > 0; }

    const QualityUpscalerColumn& Column(QualityColumn c) const { return upscaler[static_cast<size_t>(c)]; }
};

namespace qualitydetail
{
constexpr char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool StrEqNoCase(const char* a, const char* b)
{
    while (*a && Lower(*a) == Lower(*b)) { ++a; ++b; }
    return Lower(*a) == Lower(*b);
}
constexpr bool StrEq(const char* a, const char* b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
constexpr QualityPresetId IdFromKey(const char* k)
{
    if (StrEq(k, "high_1080p60"))          return QualityPresetId::High;
    if (StrEq(k, "epic_1080p60"))          return QualityPresetId::Epic;
    if (StrEq(k, "cinematic_1080p30"))     return QualityPresetId::Cinematic;
    return QualityPresetId::Count;   // 未知のキー。呼ぶ側の constexpr 評価が範囲外アクセスでコンパイルエラーにする
}
constexpr QualityColumn ColumnFromName(const char* n)
{
    if (StrEq(n, "dlss"))      return QualityColumn::Dlss;
    if (StrEq(n, "taau"))      return QualityColumn::Taau;
    if (StrEq(n, "taaNative")) return QualityColumn::TaaNative;
    return QualityColumn::Count;
}
constexpr QualityGate GateFromName(const char* n)
{
    if (StrEq(n, "Gate"))          return QualityGate::Gate;
    if (StrEq(n, "GateAfterW2BD")) return QualityGate::GateAfterW2BD;
    if (StrEq(n, "Record"))        return QualityGate::Record;
    return QualityGate::None;
}

constexpr std::array<QualityPreset, static_cast<size_t>(QualityPresetId::Count)> MakePresets()
{
    std::array<QualityPreset, static_cast<size_t>(QualityPresetId::Count)> a{};
    a[0].id = QualityPresetId::Low;        a[0].key = "low";        a[0].label = "Low";        a[0].mcpName = "low";
    a[1].id = QualityPresetId::Medium;     a[1].key = "medium";     a[1].label = "Medium";     a[1].mcpName = "medium";
    a[2].id = QualityPresetId::High;       a[2].mcpName = "high";
    a[3].id = QualityPresetId::Epic;       a[3].mcpName = "epic";
    a[4].id = QualityPresetId::Cinematic;  a[4].mcpName = "cinematic";
#define UNO_QP_PRESET(preset, label_, targetFps_, outW_, outH_, p95_, p99_) \
    { QualityPreset& p = a[static_cast<size_t>(IdFromKey(#preset))]; p.key = #preset; p.label = label_; p.hasBudget = true; \
      p.targetFps = static_cast<float>(targetFps_); p.outW = outW_; p.outH = outH_; p.p95Ms = static_cast<float>(p95_); p.p99Ms = static_cast<float>(p99_); }
#define UNO_QP_UPSCALER(preset, column, gate_, renderScale_, totalMs_) \
    { QualityUpscalerColumn& c = a[static_cast<size_t>(IdFromKey(#preset))].upscaler[static_cast<size_t>(ColumnFromName(#column))]; \
      c.present = true; c.gate = GateFromName(#gate_); c.renderScale = static_cast<float>(renderScale_); c.totalMs = static_cast<float>(totalMs_); }
#define UNO_QP_VFX(preset, emission_, maxParticles_, simMs_, drawMs_, peakMs_) \
    { QualityPreset& p = a[static_cast<size_t>(IdFromKey(#preset))]; p.emissionMultiplier = static_cast<float>(emission_); \
      p.maxParticles = maxParticles_; p.vfxSimMs = static_cast<float>(simMs_); p.vfxDrawMs = static_cast<float>(drawMs_); p.vfxPeakMs = static_cast<float>(peakMs_); }
#define UNO_QP_VRAM(preset, hasCap_, capGb_, marginGb_, precheckGb_) \
    { QualityPreset& p = a[static_cast<size_t>(IdFromKey(#preset))]; p.hasVramCap = (hasCap_) != 0; p.vramCapGb = static_cast<float>(capGb_); \
      p.vramMarginGb = static_cast<float>(marginGb_); p.vramPrecheckGb = static_cast<float>(precheckGb_); }
#include "renderer/QualityPresetTable.inc"
#undef UNO_QP_PRESET
#undef UNO_QP_UPSCALER
#undef UNO_QP_VFX
#undef UNO_QP_VRAM
    return a;
}

inline constexpr auto kPresets = MakePresets();

inline constexpr QualityBudgetRow kBudgetRows[] = {
#define UNO_QP_ROW(preset, column, row, ms) { IdFromKey(#preset), ColumnFromName(#column), #row, static_cast<float>(ms) },
#include "renderer/QualityPresetTable.inc"
#undef UNO_QP_ROW
};

inline constexpr QualityLeafRow kLeafRows[] = {
#define UNO_QP_LEAF(preset, row, leaf) { IdFromKey(#preset), #row, leaf },
#include "renderer/QualityPresetTable.inc"
#undef UNO_QP_LEAF
};

inline constexpr QualityDegradeRow kDegradeRows[] = {
#define UNO_QP_DEGRADE(preset, row, text) { IdFromKey(#preset), #row, text },
#include "renderer/QualityPresetTable.inc"
#undef UNO_QP_DEGRADE
};
} // namespace qualitydetail

// ---- 公開 API ----

inline const QualityPreset& GetPreset(QualityPresetId id)
{
    const size_t i = static_cast<size_t>(id);
    return qualitydetail::kPresets[i < qualitydetail::kPresets.size() ? i : 0];
}

// 予算の行（全プリセット・全列）。
inline const QualityBudgetRow* GetBudgetRows(size_t& count)
{
    count = sizeof(qualitydetail::kBudgetRows) / sizeof(qualitydetail::kBudgetRows[0]);
    return qualitydetail::kBudgetRows;
}
inline const QualityLeafRow* GetLeafRows(size_t& count)
{
    count = sizeof(qualitydetail::kLeafRows) / sizeof(qualitydetail::kLeafRows[0]);
    return qualitydetail::kLeafRows;
}
inline const QualityDegradeRow* GetDegradeRows(size_t& count)
{
    count = sizeof(qualitydetail::kDegradeRows) / sizeof(qualitydetail::kDegradeRows[0]);
    return qualitydetail::kDegradeRows;
}

// MCP / --quality の名前から引く（大文字小文字は区別しない）。無ければ false。
inline bool ParseQualityPreset(const char* name, QualityPresetId& out)
{
    if (!name) return false;
    for (const QualityPreset& p : qualitydetail::kPresets)
        if (qualitydetail::StrEqNoCase(name, p.mcpName)) { out = p.id; return true; }
    return false;
}

} // namespace dx12e
