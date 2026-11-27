#pragma once
// ===========================================================================
// 品質プリセットの計画（renderer/QualityPresetPlan.h）を Scene の設定へ当てる / 一致を調べる / Undo 用に写す。
//   Application に依存しない（GPU 不要）ので tests/quality_preset_test.cpp が Scene だけで検証できる。
//   触るのは「品質」の欄だけ（有効 / 無効・シェーディングの版・アップスケーラ）。半径や強さのような
//   シーンの内容（ルック）は書き換えない。
// ===========================================================================
#include "renderer/QualityPresetPlan.h"
#include "scene/Scene.h"

namespace dx12e
{

// Undo 用: プリセットが触りうる設定の丸ごとのコピー（POD の設定構造体だけ）。
struct QualityScenePack
{
    TaaSettings           taa;
    ShadingSettings       shading;
    ShadowPcssSettings    pcss;
    SSAOSettings          ssao;
    ContactShadowSettings contact;
    SsrSettings           ssr;
    SsgiSettings          ssgi;
    RtSettings            rt;
    DdgiSettings          ddgi;
};

inline QualityScenePack CaptureQualityScenePack(const Scene& s)
{
    QualityScenePack k;
    k.taa     = s.GetTaaSettings();
    k.shading = s.GetShadingSettings();
    k.pcss    = s.GetShadowPcssSettings();
    k.ssao    = s.GetSSAOSettings();
    k.contact = s.GetContactShadowSettings();
    k.ssr     = s.GetSsrSettings();
    k.ssgi    = s.GetSsgiSettings();
    k.rt      = s.GetRtSettings();
    k.ddgi    = s.GetDdgiSettings();
    return k;
}

inline void RestoreQualityScenePack(Scene& s, const QualityScenePack& k)
{
    s.GetTaaSettings()           = k.taa;
    s.GetShadingSettings()       = k.shading;
    s.GetShadowPcssSettings()    = k.pcss;
    s.GetSSAOSettings()          = k.ssao;
    s.GetContactShadowSettings() = k.contact;
    s.GetSsrSettings()           = k.ssr;
    s.GetSsgiSettings()          = k.ssgi;
    s.GetRtSettings()            = k.rt;
    s.GetDdgiSettings()          = k.ddgi;
}

// 計画をシーンの設定へ書く（render scale は Scene の外なので呼び出し側）。
inline void ApplyQualityPlanToScene(const QualityPlan& p, Scene& s)
{
    TaaSettings& t = s.GetTaaSettings();
    t.enabled = p.upscaler != QualityUpscaler::Off;
    if (p.upscaler == QualityUpscaler::Taa)  t.upscaler = static_cast<int>(upscale::Kind::Taa);
    if (p.upscaler == QualityUpscaler::Dlss) { t.upscaler = static_cast<int>(upscale::Kind::Dlss); t.dlssQuality = p.dlssQuality; }
    s.GetShadingSettings().shadingRevision = p.shadingRevision;
    ShadowPcssSettings& pc = s.GetShadowPcssSettings();
    pc.enabled        = p.pcss;
    pc.epicDynamicCsm = p.epicDynamicCsm;
    s.GetContactShadowSettings().enabled = p.contactShadow;
    s.GetSSAOSettings().enabled          = p.ssao;
    s.GetSsrSettings().enabled           = p.ssr;
    s.GetSsgiSettings().enabled          = p.ssgi;
    s.GetDdgiSettings().enabled          = p.ddgi;
    RtSettings& r = s.GetRtSettings();
    r.shadowEnabled     = p.rtShadow;
    r.reflectionsEnabled = p.rtReflect;
    r.reflectionHalfRes = p.rtReflectHalfRes;
    r.aoEnabled         = false;   // RT-AO はどのプリセットでも使わない（AO は SSAO）
}

// 現在のシーンが計画と一致しているか（プリセットを選んだ後に手で変えたら false）。
inline bool QualityPlanMatchesScene(const QualityPlan& p, const Scene& s)
{
    const TaaSettings& t = s.GetTaaSettings();
    if (t.enabled != (p.upscaler != QualityUpscaler::Off)) return false;
    if (p.upscaler == QualityUpscaler::Taa  && t.upscaler != static_cast<int>(upscale::Kind::Taa)) return false;
    if (p.upscaler == QualityUpscaler::Dlss && (t.upscaler != static_cast<int>(upscale::Kind::Dlss) || t.dlssQuality != p.dlssQuality)) return false;
    const ShadowPcssSettings& pc = s.GetShadowPcssSettings();
    const RtSettings& r = s.GetRtSettings();
    return s.GetShadingSettings().shadingRevision == p.shadingRevision
        && pc.enabled == p.pcss && pc.epicDynamicCsm == p.epicDynamicCsm
        && s.GetContactShadowSettings().enabled == p.contactShadow
        && s.GetSSAOSettings().enabled == p.ssao && s.GetSsrSettings().enabled == p.ssr
        && s.GetSsgiSettings().enabled == p.ssgi && s.GetDdgiSettings().enabled == p.ddgi
        && r.shadowEnabled == p.rtShadow && r.reflectionsEnabled == p.rtReflect
        && r.reflectionHalfRes == p.rtReflectHalfRes && !r.aoEnabled;
}

} // namespace dx12e
