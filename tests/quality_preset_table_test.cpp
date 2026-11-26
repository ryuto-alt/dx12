// 品質プリセットの口（W1-C1）: 生成表（QualityPresetTable.inc）が QualityPreset.h に漏れなく読まれていること、
// 既定値（ShadingSettings / ShadowPcssSettings）が従来と同じであること。ヘッダのみ（GPU 不要）。
// C7 の quality_preset_test（合計の予算・VRAM の上限）は別。
#include "renderer/QualityPreset.h"
#include "renderer/ShadingSettings.h"
#include "renderer/ShadowPcssSettings.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dx12e;

namespace { int g_failures = 0, g_checks = 0; }
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

int main()
{
    // ---- 5 つ全部が引ける。名前が往復する ----
    for (u8 i = 0; i < static_cast<u8>(QualityPresetId::Count); ++i)
    {
        const QualityPreset& p = GetPreset(static_cast<QualityPresetId>(i));
        CHECK(static_cast<u8>(p.id) == i);
        CHECK(p.mcpName[0] != '\0' && p.label[0] != '\0');
        QualityPresetId back{};
        CHECK(ParseQualityPreset(p.mcpName, back) && back == p.id);
    }
    QualityPresetId dummy{};
    CHECK(!ParseQualityPreset("ultra", dummy));
    CHECK(!ParseQualityPreset(nullptr, dummy));
    CHECK(!ParseQualityPreset("", dummy));

    // ---- 生成表の反映（予算の行があるのは High / Epic / Cinematic。Low / Medium は無い）----
    CHECK(!GetPreset(QualityPresetId::Low).hasBudget);
    CHECK(!GetPreset(QualityPresetId::Medium).hasBudget);
    for (QualityPresetId id : { QualityPresetId::High, QualityPresetId::Epic, QualityPresetId::Cinematic })
    {
        const QualityPreset& p = GetPreset(id);
        CHECK(p.hasBudget);
        CHECK(p.targetFps > 0.0f && p.outW > 0 && p.outH > 0);
        CHECK(p.p95Ms > 0.0f && p.p99Ms >= p.p95Ms);
        CHECK(p.maxParticles > 0 && p.emissionMultiplier > 0.0f);
        CHECK(p.vfxSimMs > 0.0f && p.vfxDrawMs > 0.0f);
        CHECK(p.vramMarginGb > 0.0f && p.vramPrecheckGb > 0.0f);
        // DLSS 列は必ずある（W1 の合否は DLSS 列）
        CHECK(p.Column(QualityColumn::Dlss).present && p.Column(QualityColumn::Dlss).gate == QualityGate::Gate);
    }
    const QualityPreset& epic = GetPreset(QualityPresetId::Epic);
    CHECK(epic.Column(QualityColumn::Taau).present && epic.Column(QualityColumn::Taau).gate == QualityGate::GateAfterW2BD);
    CHECK(epic.Column(QualityColumn::TaaNative).present && epic.Column(QualityColumn::TaaNative).gate == QualityGate::Record);
    CHECK(epic.Column(QualityColumn::TaaNative).renderScale == 1.0f);
    CHECK(epic.Column(QualityColumn::Dlss).renderScale < 1.0f);
    CHECK(epic.hasVramCap);
    CHECK(!GetPreset(QualityPresetId::Cinematic).hasVramCap);

    // ---- 行: 全プリセット x 列に行がある。VFX の行が両方ある（W1-C1 が使う）----
    size_t nRows = 0;
    const QualityBudgetRow* rows = GetBudgetRows(nRows);
    CHECK(nRows > 0);
    int epicDlssVfxSim = 0, epicDlssVfxDraw = 0, epicTaauRows = 0, epicNativeRows = 0, epicDlssRows = 0;
    for (size_t i = 0; i < nRows; ++i)
    {
        CHECK(rows[i].preset != QualityPresetId::Count && rows[i].column != QualityColumn::Count);
        CHECK(rows[i].ms >= 0.0f);
        if (rows[i].preset != QualityPresetId::Epic) continue;
        if (rows[i].column == QualityColumn::Dlss)
        {
            ++epicDlssRows;
            if (std::strcmp(rows[i].row, "VfxSim") == 0)  ++epicDlssVfxSim;
            if (std::strcmp(rows[i].row, "VfxDraw") == 0) ++epicDlssVfxDraw;
        }
        if (rows[i].column == QualityColumn::Taau)      ++epicTaauRows;
        if (rows[i].column == QualityColumn::TaaNative) ++epicNativeRows;
    }
    CHECK(epicDlssVfxSim == 1 && epicDlssVfxDraw == 1);
    CHECK(epicDlssRows > 0 && epicDlssRows == epicTaauRows && epicDlssRows == epicNativeRows);   // 3 列の形（全行・全列）

    size_t nDeg = 0;
    const QualityDegradeRow* deg = GetDegradeRows(nDeg);
    CHECK(nDeg > 0);
    for (size_t i = 0; i < nDeg; ++i) CHECK(deg[i].preset != QualityPresetId::Count && deg[i].text[0] != '\0');

    // ---- 範囲外の id は安全に Low を返す ----
    CHECK(GetPreset(QualityPresetId::Count).id == QualityPresetId::Low);

    // ---- 既定値は従来と同じ（ゴールデンがビット一致する前提）----
    ShadingSettings sh;
    CHECK(sh.multiScatter && sh.shadingRevision == 1);
    ShadowPcssSettings pcss;
    CHECK(!pcss.enabled && !pcss.epicDynamicCsm);
    CHECK(std::fabs(pcss.lightTanAngle - 0.05f) < 1e-6f);

    std::printf("quality_preset_table_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
