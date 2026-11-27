// 品質プリセットの本体（W1-C7 RM-C3）: 計画（機能の有無と縮退）、生成表との一致、予算の合計（DLSS 列 / TAAU 列）、VRAM の上限、
// Scene への適用・一致判定・Undo 用の写しの往復。Scene（GPU 不要）だけで完結する。
#include "core/QualityPresetScene.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る
#include "scene/Scene.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace dx12e;

namespace { int g_failures = 0, g_checks = 0; }
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

namespace
{
const QualityPresetId kAll[] = { QualityPresetId::Low, QualityPresetId::Medium, QualityPresetId::High, QualityPresetId::Epic, QualityPresetId::Cinematic };
const QualityCaps kCaps[] = { {true, true}, {true, false}, {false, true}, {false, false} };

// 計画のシーン側の指紋（プリセットどうしが区別できること）。
std::string Sig(const QualityPlan& p)
{
    char b[96];
    std::snprintf(b, sizeof(b), "%d.%d|%d|%d%d%d|%d%d%d%d|%d%d%d", static_cast<int>(p.upscaler),
                  p.upscaler == QualityUpscaler::Dlss ? p.dlssQuality : 0,
                  p.shadingRevision, p.pcss, p.epicDynamicCsm, p.contactShadow, p.ssao, p.ssr, p.ssgi, p.ddgi,
                  p.rtShadow, p.rtReflect, p.rtReflectHalfRes);
    return b;
}
} // namespace

int main()
{
    // ---- 5 つ x 4 つの caps で計画が作れ、プリセットどうしが区別できる ----
    for (const QualityCaps& caps : kCaps)
    {
        std::set<std::string> sigs;
        for (QualityPresetId id : kAll)
        {
            const QualityPlan p = BuildQualityPlan(id, caps);
            CHECK(p.id == id);
            sigs.insert(Sig(p));
            if (!caps.dxr) CHECK(!p.rtShadow && !p.rtReflect && !p.ddgi);   // DXR が無いのに RT を入れない
            if (!caps.dlss) CHECK(p.upscaler != QualityUpscaler::Dlss);               // DLSS が無いのに DLSS を選ばない
            if (p.ddgi) CHECK(!p.ssgi);                                               // BUG-04: DDGI が有効なら SSGI は OFF
            CHECK(p.shadingRevision == 1 || p.shadingRevision == 2);
        }
        CHECK(sigs.size() == 5);   // どのプリセットも別の構成
    }

    // ---- Epic / Cinematic / High ----
    {
        const QualityPlan e = BuildQualityPlan(QualityPresetId::Epic, {true, true});
        CHECK(e.shadingRevision == 2 && e.epicDynamicCsm && e.rtShadow && e.rtReflect && e.ddgi && !e.ssgi);
        CHECK(e.upscaler == QualityUpscaler::Dlss);
        CHECK(e.dlssQuality == static_cast<u8>(upscale::DlssQuality::Quality));    // 表の renderScale（2/3）から決まる
        CHECK(e.column == QualityColumn::Dlss && qualityplan::IsGated(GetPreset(QualityPresetId::Epic), e.column));
        CHECK(e.degradeCount == 0);
        // DLSS が無い Epic = TAA native（renderScale は表の値）。合否の対象ではなく記録だけ
        const QualityPlan n = BuildQualityPlan(QualityPresetId::Epic, {true, false});
        CHECK(n.upscaler == QualityUpscaler::Taa && n.column == QualityColumn::TaaNative);
        CHECK(!qualityplan::IsGated(GetPreset(QualityPresetId::Epic), n.column));
        CHECK(n.setRenderScale && n.renderScale == GetPreset(QualityPresetId::Epic).Column(QualityColumn::TaaNative).renderScale);
        // Cinematic は DLAA。DLSS が無ければ TAA（表に taaNative の列が無いので taau の列）
        const QualityPlan c = BuildQualityPlan(QualityPresetId::Cinematic, {true, true});
        CHECK(c.dlssQuality == static_cast<u8>(upscale::DlssQuality::Dlaa) && !c.rtReflectHalfRes);
        const QualityPlan cn = BuildQualityPlan(QualityPresetId::Cinematic, {true, false});
        CHECK(cn.upscaler == QualityUpscaler::Taa && cn.column == QualityColumn::Taau);
        // Low / Medium は表の行が無いので解像度スケールを触らない
        CHECK(!BuildQualityPlan(QualityPresetId::Low, {true, true}).setRenderScale);
        CHECK(!BuildQualityPlan(QualityPresetId::Medium, {true, true}).setRenderScale);
        CHECK(BuildQualityPlan(QualityPresetId::High, {true, true}).shadingRevision == 1);   // High は従来のシェーディング
    }

    // ---- DXR が無いときの縮退の表: RT の行が全部ある・予算の行に実在する・文が空でない ----
    for (QualityPresetId id : { QualityPresetId::Epic, QualityPresetId::Cinematic })
    {
        const QualityPlan p = BuildQualityPlan(id, {false, true});
        std::set<std::string> rows;
        for (u8 i = 0; i < p.degradeCount; ++i)
        {
            rows.insert(p.degrades[i].row);
            CHECK(p.degrades[i].text[0] != '\0');
            CHECK(qualityplan::HasBudgetRow(id, p.degrades[i].row));
        }
        if (id == QualityPresetId::Cinematic)
            CHECK(rows.count("RtGi") && rows.count("RtLocalShadow"));
        CHECK(!rows.count("RtAo"));   // RT-AO はどのプリセットにも無い
        CHECK(rows.count("Raytracing") && rows.count("RtShadow") && rows.count("RtReflect") && rows.count("Ddgi"));
        CHECK(p.ssr && p.ssgi && !p.ddgi && p.epicDynamicCsm);   // RT 反射 → SSR、DDGI → SSGI、CSM が全部描く
    }
    {
        const QualityPlan p = BuildQualityPlan(QualityPresetId::High, {false, false});
        bool ddgiRow = false, upscaleRow = false;
        for (u8 i = 0; i < p.degradeCount; ++i)
        {
            ddgiRow    |= std::strcmp(p.degrades[i].row, "Ddgi") == 0;
            upscaleRow |= std::strcmp(p.degrades[i].row, "Upscale") == 0;
        }
        CHECK(ddgiRow && upscaleRow);
        CHECK(BuildQualityPlan(QualityPresetId::Low, {false, false}).degradeCount == 0);
        // Cinematic が DLSS 無しのときの Upscale 文は TAAU の列を名乗る（TAA native ではない）
        const QualityPlan cn = BuildQualityPlan(QualityPresetId::Cinematic, {true, false});
        for (u8 i = 0; i < cn.degradeCount; ++i)
            if (std::strcmp(cn.degrades[i].row, "Upscale") == 0) CHECK(std::strstr(cn.degrades[i].text, "TAAU") != nullptr);
    }

    // ---- 生成表との一致: 3 列の形、合否の列、予算の合計（Epic の葉の合計）、VRAM ----
    {
        const QualityPreset& epic = GetPreset(QualityPresetId::Epic);
        const float dlss = SumBudgetRows(QualityPresetId::Epic, QualityColumn::Dlss);
        const float taau = SumBudgetRows(QualityPresetId::Epic, QualityColumn::Taau);
        const float nat  = SumBudgetRows(QualityPresetId::Epic, QualityColumn::TaaNative);
        CHECK(dlss > 0.0f && dlss <= epic.p95Ms + 1e-3f);    // DLSS 列 15.5 以下（W1 の合否）
        CHECK(taau > 0.0f && taau <= 16.6f + 1e-3f);    // TAAU 列 16.6 以下
        CHECK(nat > 0.0f);                                   // TAA native は記録だけ（上限の判定はしない）
        CHECK(std::fabs(dlss - epic.Column(QualityColumn::Dlss).totalMs) < 0.05f);
        CHECK(std::fabs(taau - epic.Column(QualityColumn::Taau).totalMs) < 0.05f);
        CHECK(qualityplan::IsGated(epic, QualityColumn::Dlss));
        CHECK(!qualityplan::IsGated(epic, QualityColumn::Taau) && !qualityplan::IsGated(epic, QualityColumn::TaaNative));
        // 全行・全列がある（行名の集合が 3 列で一致）
        size_t n = 0;
        const QualityBudgetRow* rows = GetBudgetRows(n);
        std::set<std::string> byCol[3];
        for (size_t i = 0; i < n; ++i)
            if (rows[i].preset == QualityPresetId::Epic) byCol[static_cast<size_t>(rows[i].column)].insert(rows[i].row);
        CHECK(!byCol[0].empty() && byCol[0] == byCol[1] && byCol[0] == byCol[2]);
        CHECK(SumBudgetRows(QualityPresetId::Cinematic, QualityColumn::Dlss) <= GetPreset(QualityPresetId::Cinematic).p95Ms + 1e-3f);
        // VRAM: 8GB の GPU で、余白を引いた実効上限が予算の事前確認（precheck）以下
        for (QualityPresetId id : { QualityPresetId::Epic, QualityPresetId::High, QualityPresetId::Cinematic })
        {
            const QualityPreset& p = GetPreset(id);
            const float cap = EffectiveVramCapGb(p, 8.0f);
            CHECK(cap > 0.0f && cap <= 8.0f - p.vramMarginGb + 1e-4f);
            if (p.hasVramCap) CHECK(cap <= p.vramCapGb + 1e-4f && cap <= p.vramPrecheckGb + 1e-4f);   // Cinematic は固定の上限なし（予算 - 余白）
        }
        CHECK(EffectiveVramCapGb(epic, 4.0f) < epic.vramCapGb);   // 小さい GPU では予算が先に効く
    }

    // ---- Scene への適用・一致判定・往復 ----
    {
        Scene scene;
        const QualityScenePack original = CaptureQualityScenePack(scene);
        for (const QualityCaps& caps : kCaps)
            for (QualityPresetId id : kAll)
            {
                const QualityPlan p = BuildQualityPlan(id, caps);
                ApplyQualityPlanToScene(p, scene);
                CHECK(QualityPlanMatchesScene(p, scene));
                CHECK(scene.GetShadingSettings().shadingRevision == p.shadingRevision);
                CHECK(scene.GetRtSettings().shadowEnabled == p.rtShadow && scene.GetDdgiSettings().enabled == p.ddgi);
            }
        const QualityPlan epic = BuildQualityPlan(QualityPresetId::Epic, {true, true});
        ApplyQualityPlanToScene(epic, scene);
        scene.GetSsrSettings().enabled = false;   // 手で 1 つ変えたら不一致になる
        CHECK(!QualityPlanMatchesScene(epic, scene));
        // 写しの往復（Undo: 元へ戻す / Redo: 適用後へ戻す）
        ApplyQualityPlanToScene(epic, scene);
        const QualityScenePack after = CaptureQualityScenePack(scene);
        RestoreQualityScenePack(scene, original);
        CHECK(scene.GetShadingSettings().shadingRevision == 1 && !scene.GetRtSettings().shadowEnabled && !scene.GetTaaSettings().enabled);
        RestoreQualityScenePack(scene, after);
        CHECK(QualityPlanMatchesScene(epic, scene));
        // プリセットが触らない欄（シーンの内容）は変えない
        Scene s2;
        s2.GetSSAOSettings().radius = 0.77f;
        s2.GetRtSettings().aoRadius = 3.5f;
        s2.GetDdgiSettings().spacing = 3.0f;
        ApplyQualityPlanToScene(epic, s2);
        CHECK(s2.GetSSAOSettings().radius == 0.77f && s2.GetRtSettings().aoRadius == 3.5f && s2.GetDdgiSettings().spacing == 3.0f);
    }

    // ---- 範囲外の id は Low へ丸める（呼び出し側の SetQualityPreset が先に弾く）----
    CHECK(BuildQualityPlan(QualityPresetId::Count, {true, true}).id == QualityPresetId::Low);

    std::printf("quality_preset_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
