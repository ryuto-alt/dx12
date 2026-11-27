#pragma once
// ===========================================================================
// シーンのルック（空・IBL・露出・フォグ）の 1 つの表。
//   W1-C10（BUG-16）: ライティング・プリセット（editor/LightingPresets.h）が太陽とポストしか変えず、
//   「夜」にしても空・IBL・フォグが昼のままだった。プリセットが変える欄をここに足し、
//   W1-C8（シーンのルックのプロファイル。Epic など）も同じ構造体を使う（表は 1 つ）。
//
//   ★editor/LightingPresets.h は前方宣言だけでこの型を知る（ApplicationInternal.h 経由で 48 TU が読むので、
//     そこから Scene.h を引き込まない）。このヘッダを include するのは適用する側の .cpp だけ。
//   ★既存シーンには何も当てない。値を書くのは ApplySceneLook を呼んだときだけ。
//   Application / GPU / ImGui に依存しない（tests/scene_look_profile_test.cpp が Scene だけで検証する）。
// ===========================================================================
#include <cmath>
#include <cstring>

#include "renderer/PostProcessSettings.h"
#include "renderer/VolumetricFogSettings.h"
#include "renderer/atmosphere/AtmosphereSettings.h"
#include "scene/Scene.h"

namespace dx12e
{

struct SceneLookProfile
{
    const char* id;              // ライティング・プリセットの id と同じ（"night" など）

    // 空: 物理大気が ON のときだけ効く（時刻）。< 0 = 時刻を触らない（向きを直接指定するプリセット）。
    f32  timeOfDay;
    // 空・IBL の強さ（SkyboxSettings）。環境光の主役は IBL なので、夜は桁で落とす。
    f32  skyboxIntensity;
    f32  iblIntensity;
    // 露出補正[EV]。露出モード 2（自動露出）のときだけ使う（太陽・空・IBL の暗さに自動露出が追従して持ち上げるので、
    //   負の補正でルックの暗さを保つ）。
    f32  evComp;
    // 手動 EV（露出モード 1）用の補正[EV]。太陽・IBL・空はプリセットで既に暗いので、ここは
    //   モード 0 の exposure 乗算の log2（night 0.90 → -0.15 など）と同じ小さな値にして、二重に暗くしない。
    //   モード 0（従来）では LightingPreset の exposure 乗算を使う。
    f32  evManual;
    // フォグ（froxel ボリュメトリック）。fogOn=false はスイッチを切るだけ（他の値は触らない）。
    bool fogOn;
    f32  fogDensity;
    f32  fogAnisotropy;
    f32  fogHeightFalloff;
    DirectX::XMFLOAT3 fogAlbedo;
    DirectX::XMFLOAT3 fogAmbient;   // 環境散乱の色
};

// ライティング・プリセットごとのルック（LightingPreset と同じ並び・同じ id）。
inline const SceneLookProfile kLightingLooks[] = {
    // id       時刻   空     IBL    EV(自動) EV(手動) fog    濃さ    g     高減衰  albedo               ambient
    {"day",     12.0f, 1.00f, 1.00f,  0.0f, 0.0f, false, 0.020f, 0.30f, 0.10f, {1.00f, 1.00f, 1.00f}, {0.20f, 0.24f, 0.32f}},
    {"dusk",    17.5f, 1.00f, 0.90f, -0.5f, 0.0704f, true,  0.008f, 0.55f, 0.08f, {1.00f, 0.82f, 0.66f}, {0.22f, 0.16f, 0.14f}},
    {"night",    1.0f, 0.25f, 0.15f, -2.0f, -0.152f, true,  0.012f, 0.25f, 0.12f, {0.62f, 0.72f, 1.00f}, {0.03f, 0.04f, 0.07f}},
    {"indoor",  -1.0f, 0.60f, 0.50f,  0.0f, 0.0f, false, 0.020f, 0.30f, 0.10f, {1.00f, 0.92f, 0.80f}, {0.20f, 0.17f, 0.14f}},
    {"horror",  -1.0f, 0.08f, 0.06f, -2.5f, -0.322f, true,  0.050f, 0.20f, 0.25f, {0.55f, 0.62f, 0.70f}, {0.010f, 0.015f, 0.025f}},
    {"studio",  -1.0f, 0.60f, 1.00f,  0.0f, 0.0f, false, 0.020f, 0.30f, 0.10f, {1.00f, 1.00f, 1.00f}, {0.20f, 0.24f, 0.32f}},
};

inline constexpr int kLightingLookCount =
    static_cast<int>(sizeof(kLightingLooks) / sizeof(kLightingLooks[0]));

inline const SceneLookProfile* FindSceneLook(const char* id)
{
    if (!id) return nullptr;
    for (const SceneLookProfile& p : kLightingLooks)
        if (std::strcmp(p.id, id) == 0) return &p;
    return nullptr;
}

// プリセットが触る設定の丸ごとのコピー（適用の入力 / 出力 / Undo の before・after）。
struct SceneLookPack
{
    SkyboxSettings        sky;
    VolumetricFogSettings fog;
    AtmosphereSettings    atmo;
    PostProcessSettings   post;
};

inline SceneLookPack CaptureSceneLookPack(const Scene& s)
{
    SceneLookPack k;
    k.sky  = s.GetSkyboxSettings();
    k.fog  = s.GetVolumetricFogSettings();
    k.atmo = s.GetAtmosphereSettings();
    k.post = s.GetPostSettings();
    return k;
}

inline void RestoreSceneLookPack(Scene& s, const SceneLookPack& k)
{
    s.GetSkyboxSettings()        = k.sky;
    s.GetVolumetricFogSettings() = k.fog;
    s.GetAtmosphereSettings()    = k.atmo;
    s.GetPostSettings()          = k.post;
}

// プリセットのルックを base に当てた結果を返す（base は書き換えない）。
//   post の露出以外（ブルーム等）は LightingPreset 側（ApplyLightingPresetToPost）が決めるので、ここでは
//   露出モード 1（手動 EV）は evManual、2（自動）は evComp だけを書く。
inline SceneLookPack ApplySceneLook(const SceneLookProfile& look, const SceneLookPack& base)
{
    SceneLookPack out = base;

    out.sky.skyboxIntensity = look.skyboxIntensity;
    out.sky.iblIntensity    = look.iblIntensity;

    if (out.atmo.enabled)
    {
        if (look.timeOfDay >= 0.0f)
        {
            out.atmo.timeOfDay = look.timeOfDay;
            out.atmo.sunMode   = 0;   // 直前に向き指定のプリセットが sunMode=1 にしていても戻す（適用順に依らない）
        }
        else
            out.atmo.sunMode = 1;   // 向きを直接指定するプリセット: 時刻で太陽が動かないようにする（set_sun の方位指定と同じ）
    }

    if (out.post.exposureMode == 1)
        out.post.evComp = look.evManual;
    else if (out.post.exposureMode == 2)
        out.post.evComp = look.evComp;

    out.fog.enabled = look.fogOn;
    if (look.fogOn)
    {
        out.fog.density       = look.fogDensity;
        out.fog.anisotropy    = look.fogAnisotropy;
        out.fog.heightFalloff = look.fogHeightFalloff;
        out.fog.albedo        = look.fogAlbedo;
        out.fog.ambient       = look.fogAmbient;
    }
    return out;
}

// プリセット id からルックを引く（未知なら先頭=昼）。
inline const SceneLookProfile& LightingPresetLook(const char* presetId)
{
    const SceneLookProfile* p = FindSceneLook(presetId);
    return p ? *p : kLightingLooks[0];
}

} // namespace dx12e
