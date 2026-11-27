// ライティング・プリセットと SceneLookProfile の一本化（W1-C10 / BUG-16）。
// 夜・ホラー・室内などが、太陽とポストだけでなく空・IBL・露出補正・フォグ・大気の時刻も変えること、
// 往復（Undo 用の写し）、既存シーンが当てない限り不変なこと、表と LightingPreset の並びの一致を調べる。
// Scene（GPU 不要）だけで完結する。
#include "editor/LightingPresets.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る
#include "scene/Scene.h"
#include "scene/SceneLookProfile.h"

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
SceneLookPack Apply(const LightingPreset& p, const SceneLookPack& base)
{
    SceneLookPack k = base;
    k.post = ApplyLightingPresetToPost(p, k.post);
    return ApplySceneLook(LightingPresetLook(p.id), k);
}
bool SameSky(const SkyboxSettings& a, const SkyboxSettings& b)
{
    return a.envMapPath == b.envMapPath && a.iblIntensity == b.iblIntensity
        && a.skyboxIntensity == b.skyboxIntensity && a.drawSkybox == b.drawSkybox;
}
} // namespace

int main()
{
    // ---- 表の一貫性: 全プリセットにルックがあり、id が重複せず、並びが同じ ----
    CHECK(kLightingLookCount == kLightingPresetCount);
    std::set<std::string> ids;
    for (int i = 0; i < kLightingPresetCount; ++i)
    {
        const LightingPreset& p = kLightingPresets[i];
        CHECK(ids.insert(p.id).second);
        const SceneLookProfile* look = FindSceneLook(p.id);
        CHECK(look != nullptr);
        if (!look) continue;
        CHECK(std::strcmp(look->id, kLightingLooks[i].id) == 0);   // 並びも同じ
        CHECK(look->skyboxIntensity >= 0.0f && look->iblIntensity >= 0.0f);
        CHECK(look->fogDensity > 0.0f && look->fogDensity < 1.0f);
        // 時刻つきプリセットは、太陽の曲線と同じ時刻を大気へ渡す（空と太陽が食い違わない）
        CHECK((p.hour >= 0.0f) == (look->timeOfDay >= 0.0f));
        if (p.hour >= 0.0f) CHECK(look->timeOfDay == p.hour);
        // 手動 EV の補正は、従来モードの乗算（exposureOn なら exposure、無ければ 1）の log2 と同じ（見た目が揃う）
        const float mul = p.exposureOn ? p.exposure : 1.0f;
        CHECK(std::fabs(look->evManual - std::log2(mul)) < 0.01f);
    }
    CHECK(FindSceneLook("nonexistent") == nullptr && FindSceneLook(nullptr) == nullptr);

    const LightingPreset* day   = FindLightingPreset("day");
    const LightingPreset* night = FindLightingPreset("night");
    const LightingPreset* horror = FindLightingPreset("horror");
    CHECK(day && night && horror);

    // ---- 受け入れ: night で空・IBL・露出・フォグが変わる（物理大気 OFF・露出モード 1）----
    {
        Scene scene;
        scene.GetPostSettings().exposureMode = 1;
        const SceneLookPack before = CaptureSceneLookPack(scene);
        const SceneLookPack after  = Apply(*night, before);
        CHECK(after.sky.skyboxIntensity < before.sky.skyboxIntensity);   // 空
        CHECK(after.sky.iblIntensity    < before.sky.iblIntensity);      // IBL
        CHECK(after.post.evComp         < before.post.evComp);           // 露出（手動 EV: 小さな補正）
        CHECK(after.post.evComp == FindSceneLook("night")->evManual && after.post.evComp > -0.5f);
        CHECK(!before.fog.enabled && after.fog.enabled);                 // フォグ
        CHECK(after.fog.density == FindSceneLook("night")->fogDensity);
        // 乗算の露出（従来モード用）は二重に掛けない
        CHECK(after.post.exposureOn == before.post.exposureOn && after.post.exposure == before.post.exposure);
        // 触らない欄は保つ
        CHECK(after.sky.envMapPath == before.sky.envMapPath && after.sky.drawSkybox == before.sky.drawSkybox);
        CHECK(after.atmo == before.atmo);   // 大気 OFF のときは大気に触らない

        // Undo / Redo の写しの往復
        RestoreSceneLookPack(scene, after);
        CHECK(scene.GetVolumetricFogSettings().enabled && scene.GetSkyboxSettings().iblIntensity == after.sky.iblIntensity);
        RestoreSceneLookPack(scene, before);
        CHECK(!scene.GetVolumetricFogSettings().enabled && SameSky(scene.GetSkyboxSettings(), before.sky));
        CHECK(scene.GetPostSettings().evComp == before.post.evComp);
        // 冪等（同じプリセットを 2 回当てても同じ）
        const SceneLookPack a1 = Apply(*night, before);
        const SceneLookPack a2 = Apply(*night, a1);
        CHECK(SameSky(a1.sky, a2.sky) && a1.fog.enabled == a2.fog.enabled && a1.fog.density == a2.fog.density
              && a1.post.evComp == a2.post.evComp && a1.post.exposureMode == a2.post.exposureMode);
    }

    // ---- 露出モード 0（従来）は乗算の値、1/2 は露出補正だけ ----
    {
        PostProcessSettings legacy;   // exposureMode = 0
        const PostProcessSettings l = ApplyLightingPresetToPost(*horror, legacy);
        CHECK(l.exposureOn == horror->exposureOn && l.exposure == horror->exposure);
        PostProcessSettings autoPost;
        autoPost.exposureMode = 2;
        autoPost.exposureOn = false; autoPost.exposure = 1.0f;
        const PostProcessSettings a = ApplyLightingPresetToPost(*horror, autoPost);
        CHECK(!a.exposureOn && a.exposure == 1.0f);   // 乗算は触らない
        SceneLookPack k; k.post = autoPost;
        const SceneLookPack out = ApplySceneLook(LightingPresetLook("horror"), k);
        CHECK(out.post.evComp == FindSceneLook("horror")->evComp && out.post.evComp < -1.0f);   // 自動露出だけ大きな負の補正
        SceneLookPack km; km.post = autoPost; km.post.exposureMode = 1;
        CHECK(ApplySceneLook(LightingPresetLook("horror"), km).post.evComp == FindSceneLook("horror")->evManual);
        // モード 0 では evComp に触らない
        SceneLookPack k0; k0.post = legacy;
        CHECK(ApplySceneLook(LightingPresetLook("horror"), k0).post.evComp == legacy.evComp);
    }

    // ---- 物理大気 ON: 時刻つきは時刻を渡し、向き指定は sunMode=1 へ ----
    {
        SceneLookPack k;
        k.atmo.enabled = true; k.atmo.timeOfDay = 12.0f; k.atmo.sunMode = 0;
        const SceneLookPack n = Apply(*night, k);
        CHECK(n.atmo.timeOfDay == 1.0f && n.atmo.sunMode == 0);
        const SceneLookPack h = Apply(*horror, k);
        CHECK(h.atmo.timeOfDay == 12.0f && h.atmo.sunMode == 1);   // 時刻では太陽が動かない
    }

    // ---- 適用順に依らない: horror→night / indoor→day が、何も当てていないシーンへの night / day と一致 ----
    {
        SceneLookPack k;
        k.atmo.enabled = true; k.atmo.timeOfDay = 9.0f; k.atmo.sunMode = 0;
        k.post.exposureMode = 1;
        const SceneLookPack direct = Apply(*night, k);
        const SceneLookPack viaH   = Apply(*night, Apply(*horror, k));
        CHECK(viaH.atmo == direct.atmo && SameSky(viaH.sky, direct.sky) && viaH.post.evComp == direct.post.evComp
              && viaH.fog.enabled == direct.fog.enabled && viaH.fog.density == direct.fog.density);
        const LightingPreset* indoor = FindLightingPreset("indoor");
        const SceneLookPack dDirect = Apply(*day, k);
        const SceneLookPack dViaI   = Apply(*day, Apply(*indoor, k));
        CHECK(dViaI.atmo == dDirect.atmo && SameSky(dViaI.sky, dDirect.sky) && dViaI.post.evComp == dDirect.post.evComp);
    }

    // ---- 昼はフォグを切る（夜からの戻り）。フォグの他の値は触らない ----
    {
        SceneLookPack k;
        const SceneLookPack n = Apply(*night, k);
        const SceneLookPack d = Apply(*day, n);
        CHECK(!d.fog.enabled && d.fog.density == n.fog.density);
        CHECK(d.sky.iblIntensity == FindSceneLook("day")->iblIntensity);
    }

    // ---- 既存シーンは、当てない限り不変（既定の Scene が表の値を勝手に持たない）----
    {
        Scene scene;
        const SceneLookPack k = CaptureSceneLookPack(scene);
        CHECK(k.sky.iblIntensity == 1.0f && k.sky.skyboxIntensity == 1.0f && !k.fog.enabled && !k.atmo.enabled);
        // 太陽: 今の ApplyLightingPresetToSun は従来のまま（C10 は太陽の式を変えない）
        DirectionalLight base{};
        const DirectionalLight s = ApplyLightingPresetToSun(*night, base);
        CHECK(s.intensity == lightmath::SampleTimeOfDay(night->hour).intensity);
    }


    // =====================================================================
    // W1-C8: 新規シーンのルックのプロファイル（Epic / Classic）。caps ごとの分岐、既存シーンの不変、BUG-04。
    // =====================================================================
    {
        NewSceneProfile pr{};
        CHECK(ParseNewSceneProfile("epic", pr) && pr == NewSceneProfile::Epic);
        CHECK(ParseNewSceneProfile("classic", pr) && pr == NewSceneProfile::Classic);
        CHECK(!ParseNewSceneProfile("Epic", pr) && !ParseNewSceneProfile("", pr) && !ParseNewSceneProfile(nullptr, pr));
        CHECK(std::strcmp(NewSceneProfileName(NewSceneProfile::Epic), "epic") == 0
              && std::strcmp(NewSceneProfileName(NewSceneProfile::Classic), "classic") == 0);

        // 既存シーン（何も当てていない Scene）は Epic ではない。Classic を当てても 1 欄も変わらない。
        {
            Scene scene;
            CHECK(!SceneHasEpicLook(scene));
            const QualityScenePack q0 = CaptureQualityScenePack(scene);
            const SceneLookPack    l0 = CaptureSceneLookPack(scene);
            ApplySceneProfile(scene, NewSceneProfile::Classic, BuildQualityPlan(QualityPresetId::Epic, { true, true }), true);
            const QualityScenePack q1 = CaptureQualityScenePack(scene);
            const SceneLookPack    l1 = CaptureSceneLookPack(scene);
            CHECK(!SceneHasEpicLook(scene));
            CHECK(q1.shading.shadingRevision == q0.shading.shadingRevision && q1.taa.enabled == q0.taa.enabled
                  && q1.ssgi.enabled == q0.ssgi.enabled && q1.ddgi.enabled == q0.ddgi.enabled && q1.rt.shadowEnabled == q0.rt.shadowEnabled);
            CHECK(l1.atmo == l0.atmo && l1.post.tonemapper == l0.post.tonemapper && l1.post.exposureMode == l0.post.exposureMode);
        }

        struct CapsCase { bool dxr, dlss; };
        for (const CapsCase c : { CapsCase{true, true}, CapsCase{true, false}, CapsCase{false, true}, CapsCase{false, false} })
        {
            Scene scene;
            const QualityPlan plan = BuildQualityPlan(QualityPresetId::Epic, { c.dxr, c.dlss });
            // 新規シーンの直後: gi::ApplyNew が通った（dxr）ときは DDGI と SSGI が両方 ON になっている（ApplyNew の既定）
            scene.GetSsgiSettings().enabled = true;
            if (c.dxr) scene.GetDdgiSettings().enabled = true;
            ApplySceneProfile(scene, NewSceneProfile::Epic, plan, /*giNewApplied*/ c.dxr);

            CHECK(SceneHasEpicLook(scene));
            // 物理大気は ON、太陽の向きはライトのまま（W05-22 の方位 220° / 高度 38° を保つ）、色と強さは大気から
            const AtmosphereSettings& a = scene.GetAtmosphereSettings();
            CHECK(a.enabled && a.sunMode == 1 && a.driveSun && a.driveIBL);
            // 表示変換は UE Filmic、露出はヒストグラムの自動露出（手動 EV 1.0 のままでは昼が白飛びする）
            const PostProcessSettings& p = scene.GetPostSettings();
            CHECK(p.tonemapper == 3 && p.exposureMode == 2);
            CHECK(p.aeLowPercent > 0.0f && p.aeHighPercent < 1.0f && p.aeLowPercent < p.aeHighPercent);
            // V2 のシェーディングと Epic の太陽影
            CHECK(scene.GetShadingSettings().shadingRevision == 2);
            CHECK(scene.GetShadowPcssSettings().enabled && scene.GetShadowPcssSettings().epicDynamicCsm);
            CHECK(scene.GetShadingSettings().microShadow);
            // GTAO / SSR
            CHECK(scene.GetSSAOSettings().enabled && scene.GetSsrSettings().enabled);
            // caps ごとの分岐: DXR があれば RT 影 / RT 反射 / DDGI、無ければ全部 OFF
            CHECK(scene.GetRtSettings().shadowEnabled == c.dxr);
            CHECK(scene.GetRtSettings().reflectionsEnabled == c.dxr);
            CHECK(scene.GetDdgiSettings().enabled == c.dxr);
            // BUG-04: DDGI が有効なら SSGI は OFF、DDGI が無ければ SSGI を使う（GI を失わない）
            CHECK(scene.GetSsgiSettings().enabled == !scene.GetDdgiSettings().enabled);
            // アップスケーラ: DLSS があれば DLSS、無ければ TAA
            CHECK(scene.GetTaaSettings().enabled);
            CHECK(scene.GetTaaSettings().upscaler == static_cast<int>(c.dlss ? upscale::Kind::Dlss : upscale::Kind::Taa));
            // 品質プリセットの計画と一致している（Epic を選んだ状態と同じ）
            CHECK(QualityPlanMatchesScene(plan, scene));
        }

        // DXR はあるが gi::ApplyNew が通らなかった（DDGI の初期化失敗）: DDGI は ON にせず SSGI を使う
        {
            Scene scene;
            ApplySceneProfile(scene, NewSceneProfile::Epic, BuildQualityPlan(QualityPresetId::Epic, { true, false }), /*giNewApplied*/ false);
            CHECK(!scene.GetDdgiSettings().enabled && scene.GetSsgiSettings().enabled);
        }

        // 冪等: 2 回当てても同じ（新規シーンの組み立てと明示の再適用で食い違わない）
        {
            Scene a, b;
            const QualityPlan plan = BuildQualityPlan(QualityPresetId::Epic, { true, true });
            ApplySceneProfile(a, NewSceneProfile::Epic, plan, true);
            ApplySceneProfile(b, NewSceneProfile::Epic, plan, true);
            ApplySceneProfile(b, NewSceneProfile::Epic, plan, true);
            CHECK(a.GetAtmosphereSettings() == b.GetAtmosphereSettings());
            CHECK(a.GetPostSettings().exposureMode == b.GetPostSettings().exposureMode
                  && a.GetPostSettings().evComp == b.GetPostSettings().evComp
                  && a.GetSsgiSettings().enabled == b.GetSsgiSettings().enabled);
        }
    }

    std::printf("scene_look_profile_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
