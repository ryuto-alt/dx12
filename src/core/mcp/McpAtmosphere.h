#pragma once
// ===========================================================================
// MCP の物理ベース大気 A1 まわりの JSON 変換（get/set_scene_settings の "atmosphere" と set_sun が共有する）。
// キー名・範囲は SceneSerializer.cpp の SerializeAtmosphere / LoadAtmosphereSettings と同じ。
// ===========================================================================
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/AtmosphereHost.h"
#include "renderer/atmosphere/AtmosphereMath.h"
#include "renderer/atmosphere/AtmosphereSettings.h"

namespace dx12e::mcpatmo
{
using json = nlohmann::json;

inline json Rgb3(const float* v) { return json::array({v[0], v[1], v[2]}); }

inline json ToJson(const AtmosphereSettings& a)
{
    return {
        {"enabled", a.enabled}, {"timeOfDay", a.timeOfDay}, {"timeSpeed", a.timeSpeed},
        {"latitudeDeg", a.latitudeDeg}, {"dayOfYear", a.dayOfYear}, {"northYawDeg", a.northYawDeg},
        {"sunMode", a.sunMode}, {"driveSun", a.driveSun}, {"driveIBL", a.driveIBL},
        {"drawStars", a.drawStars}, {"drawMoon", a.drawMoon},
        {"planetRadiusKm", a.planetRadiusKm}, {"atmosphereHeightKm", a.atmosphereHeightKm},
        {"rayleighScattering", Rgb3(a.rayleighScattering)}, {"rayleighScaleHeightKm", a.rayleighScaleHeightKm},
        {"mieScattering", Rgb3(a.mieScattering)}, {"mieAbsorption", Rgb3(a.mieAbsorption)},
        {"mieScaleHeightKm", a.mieScaleHeightKm}, {"mieG", a.mieG},
        {"ozoneAbsorption", Rgb3(a.ozoneAbsorption)}, {"ozoneCenterKm", a.ozoneCenterKm}, {"ozoneWidthKm", a.ozoneWidthKm},
        {"groundAlbedo", Rgb3(a.groundAlbedo)}, {"sunAngularRadius", a.sunAngularRadius},
        {"sunIlluminance", a.sunIlluminance}, {"sunTint", Rgb3(a.sunTint)},
        {"moonIlluminance", a.moonIlluminance}, {"moonTint", Rgb3(a.moonTint)},
        {"multiScatteringFactor", a.multiScatteringFactor}, {"seaLevelY", a.seaLevelY},
        {"nightSkyNits", a.nightSkyNits}, {"skyLuminanceScale", a.skyLuminanceScale},
        {"aerialPerspective", a.aerialPerspective}, {"apStartDepth", a.apStartDepth},
        {"apMaxDistanceKm", a.apMaxDistanceKm}, {"apStrength", a.apStrength},
        {"iblRebakeThresholdDeg", a.iblRebakeThresholdDeg}, {"iblRebakeMaxHz", a.iblRebakeMaxHz},
    };
}

// 受け付けるキー(preset を含む)。set_scene_settings の atmosphere オブジェクトと TS 側の known[] はこれと揃える。
inline const std::vector<std::string>& Keys()
{
    static const std::vector<std::string> k = {
        "preset", "enabled", "timeOfDay", "timeSpeed", "latitudeDeg", "dayOfYear", "northYawDeg", "sunMode", "driveSun", "driveIBL",
        "drawStars", "drawMoon", "planetRadiusKm", "atmosphereHeightKm", "rayleighScattering", "rayleighScaleHeightKm",
        "mieScattering", "mieAbsorption", "mieScaleHeightKm", "mieG", "ozoneAbsorption", "ozoneCenterKm", "ozoneWidthKm",
        "groundAlbedo", "sunAngularRadius", "sunIlluminance", "sunTint", "moonIlluminance", "moonTint",
        "multiScatteringFactor", "seaLevelY", "nightSkyNits", "skyLuminanceScale", "aerialPerspective", "apStartDepth",
        "apMaxDistanceKm", "apStrength", "iblRebakeThresholdDeg", "iblRebakeMaxHz"};
    return k;
}

// j の指定分だけ a へ適用する。preset を先に適用してから個別項目で上書きする。範囲外は LoadAtmosphereSettings と同じくクランプ。
// 失敗(未知キー / 型不正 / 未知のプリセット)なら false を返し err に理由を入れる(この場合 a は一部だけ変わっていることがある。呼び出し側は先に複製して検査すること)。
inline bool Apply(AtmosphereSettings& a, const json& j, std::string& err)
{
    if (!j.is_object()) { err = "atmosphere はオブジェクト"; return false; }
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        const auto& k = Keys();
        if (std::find(k.begin(), k.end(), it.key()) == k.end()) { err = "未知のキー: " + it.key(); return false; }
    }
    if (j.contains("preset") && !j["preset"].is_null())
    {
        AtmospherePreset p = AtmospherePreset::Earth;
        if (!j["preset"].is_string() || !AtmospherePresetFromId(j["preset"].get<std::string>(), p))
        {
            err = "preset は earth / mars / haze / twilight のどれか";
            return false;
        }
        ApplyAtmospherePreset(a, p);
    }
    bool ok = true;
    auto num = [&](const char* key, float& dst, float lo, float hi)
    {
        if (!ok || !j.contains(key) || j[key].is_null()) return;
        if (!j[key].is_number()) { err = std::string(key) + " は数値"; ok = false; return; }
        const float v = j[key].get<float>();
        dst = std::isfinite(v) ? std::clamp(v, lo, hi) : dst;
    };
    auto inum = [&](const char* key, int& dst, int lo, int hi)
    {
        if (!ok || !j.contains(key) || j[key].is_null()) return;
        if (!j[key].is_number()) { err = std::string(key) + " は数値"; ok = false; return; }
        dst = std::clamp(j[key].get<int>(), lo, hi);
    };
    auto flag = [&](const char* key, bool& dst)
    {
        if (!ok || !j.contains(key) || j[key].is_null()) return;
        if (!j[key].is_boolean()) { err = std::string(key) + " は true / false"; ok = false; return; }
        dst = j[key].get<bool>();
    };
    auto rgb = [&](const char* key, float* dst)
    {
        if (!ok || !j.contains(key) || j[key].is_null()) return;
        const auto& v = j[key];
        if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number())
        { err = std::string(key) + " は [r,g,b]"; ok = false; return; }
        for (int i = 0; i < 3; ++i)
        {
            const float f = v[i].get<float>();
            if (std::isfinite(f)) dst[i] = std::clamp(f, 0.0f, 1000.0f);
        }
    };
    flag("enabled", a.enabled);
    num("timeOfDay", a.timeOfDay, 0.0f, 24.0f);
    num("timeSpeed", a.timeSpeed, -240.0f, 240.0f);
    num("latitudeDeg", a.latitudeDeg, -90.0f, 90.0f);
    inum("dayOfYear", a.dayOfYear, 1, 366);
    num("northYawDeg", a.northYawDeg, -360.0f, 360.0f);
    inum("sunMode", a.sunMode, 0, 1);
    flag("driveSun", a.driveSun);
    flag("driveIBL", a.driveIBL);
    flag("drawStars", a.drawStars);
    flag("drawMoon", a.drawMoon);
    num("planetRadiusKm", a.planetRadiusKm, 100.0f, 100000.0f);
    num("atmosphereHeightKm", a.atmosphereHeightKm, 1.0f, 2000.0f);
    rgb("rayleighScattering", a.rayleighScattering);
    num("rayleighScaleHeightKm", a.rayleighScaleHeightKm, 0.1f, 500.0f);
    rgb("mieScattering", a.mieScattering);
    rgb("mieAbsorption", a.mieAbsorption);
    num("mieScaleHeightKm", a.mieScaleHeightKm, 0.1f, 500.0f);
    num("mieG", a.mieG, -0.99f, 0.99f);
    rgb("ozoneAbsorption", a.ozoneAbsorption);
    num("ozoneCenterKm", a.ozoneCenterKm, 0.0f, 1000.0f);
    num("ozoneWidthKm", a.ozoneWidthKm, 0.0f, 500.0f);
    rgb("groundAlbedo", a.groundAlbedo);
    num("sunAngularRadius", a.sunAngularRadius, 0.0005f, 0.2f);
    num("sunIlluminance", a.sunIlluminance, 0.0f, 1.0e7f);
    rgb("sunTint", a.sunTint);
    num("moonIlluminance", a.moonIlluminance, 0.0f, 1.0e5f);
    rgb("moonTint", a.moonTint);
    num("multiScatteringFactor", a.multiScatteringFactor, 0.0f, 4.0f);
    num("seaLevelY", a.seaLevelY, -1.0e5f, 1.0e5f);
    num("nightSkyNits", a.nightSkyNits, 0.0f, 1000.0f);
    num("skyLuminanceScale", a.skyLuminanceScale, 0.0f, 100.0f);
    flag("aerialPerspective", a.aerialPerspective);
    num("apStartDepth", a.apStartDepth, 0.0f, 1.0e6f);
    num("apMaxDistanceKm", a.apMaxDistanceKm, 1.0f, 2000.0f);
    num("apStrength", a.apStrength, 0.0f, 20.0f);
    num("iblRebakeThresholdDeg", a.iblRebakeThresholdDeg, 0.0f, 90.0f);
    num("iblRebakeMaxHz", a.iblRebakeMaxHz, 0.1f, 60.0f);
    return ok;
}

// 設定だけから求まる太陽の状態(時刻モード)。sunMode=1 のときは呼び出し側が向きを渡す。
inline json SunJson(const AtmosphereSettings& a, const atmosphere::Vec3d& sunDir, double camAltKm)
{
    using namespace atmosphere;
    const double el = std::asin(std::clamp(sunDir.y, -1.0, 1.0)) * 180.0 / kPi;
    double az = std::atan2(sunDir.x, sunDir.z) * 180.0 / kPi - a.northYawDeg;
    while (az < 0.0) az += 360.0;
    while (az >= 360.0) az -= 360.0;
    AtmosphereSettings s = a;
    if (s.sunMode == 1) s.drawMoon = false;
    const SkyLight L = ComputeSkyLight(s, sunDir, std::max(camAltKm, 0.002));   // 標高 0 ちょうどは地面に接して透過率が 0 になるので 2 m を下限にする
    return {{"sunElevationDeg", el}, {"sunAzimuthDeg", az}, {"isMoon", L.isMoon},
            {"groundIlluminanceLux", json::array({L.illuminanceGround.r, L.illuminanceGround.g, L.illuminanceGround.b})},
            {"groundIlluminanceLuxLuminance", RgbLuminance(L.illuminanceGround)}};
}


// get_scene_settings の atmosphereState。m_atmo が無い / 未初期化 / 大気 OFF でも壊れない(active=false と最小の値だけ返す)。
inline json StateJson(const AtmosphereSettings& a, const AtmoHost* h)
{
    json st = {{"active", false}};
    if (!a.enabled || !h || !h->timeValid) return st;
    st = SunJson(a, h->sunDir, h->camAltKm);
    st["active"] = true;
    st["unitScale"] = h->unitScale;
    st["iblRebakeCount"] = h->iblRebakeCount;
    st["iblBigCount"] = h->iblBigCount;
    st["iblRunning"] = h->iblRunning;
    st["lastSunDeltaDeg"] = h->lastSunDeltaDeg;
    json ms = json::object(), ran = json::object();
    if (h->renderer.IsReady())
    {
        for (u32 i = 0; i < AtmosphereRenderer::ScopeCount; ++i)
        {
            ms[AtmosphereRenderer::ScopeName(i)] = h->renderer.GetMs(i);
            ran[AtmosphereRenderer::ScopeName(i)] = h->renderer.ScopeRanRecently(i);
        }
        st["lutUpdates"] = {{"transMs", h->renderer.GetUpdateCount(0)}, {"skyView", h->renderer.GetUpdateCount(1)},
                            {"ap", h->renderer.GetUpdateCount(2)}, {"cube", h->renderer.GetUpdateCount(3)}};
    }
    st["gpuMs"] = ms;
    st["gpuMsRanLastFrame"] = ran;
    return st;
}

} // namespace dx12e::mcpatmo
