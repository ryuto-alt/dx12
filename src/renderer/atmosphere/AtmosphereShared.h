#pragma once
// ===========================================================================
// AtmosphereShared — C++ と HLSL(shaders/atmosphere/AtmosphereCommon.hlsli の cbuffer AtmosphereCB)で共有するレイアウトと、
// 「設定 + 光源 → GPU 定数」の純関数(GPU / DirectXMath に依存しない。テストが直接使う)。
// ★HLSL の cbuffer と行ごとに一致させること(static_assert が番人)。
// ===========================================================================
#include <cmath>
#include <cstdint>
#include <cstring>

#include "renderer/atmosphere/AtmosphereMath.h"
#include "renderer/atmosphere/AtmosphereSettings.h"

namespace dx12e::atmosphere
{

// LUT の解像度(AtmosphereCommon.hlsli の AT_* と一致)
inline constexpr uint32_t kTransmittanceW = 256, kTransmittanceH = 64;
inline constexpr uint32_t kMultiScatW = 32, kMultiScatH = 32;
inline constexpr uint32_t kSkyViewW = 384, kSkyViewH = 128;
inline constexpr uint32_t kApW = 32, kApH = 32, kApD = 32;

inline constexpr uint32_t kFlagStars = 1u, kFlagMoon = 2u, kFlagAp = 4u, kFlagLightIsMoon = 8u;

struct AtmosphereGpuParams
{
    float rayScat[3];      float rayExp;
    float mieScat[3];      float mieExp;
    float mieExt[3];       float mieG;
    float ozoneExt[3];     float ozoneCenter;
    float groundAlbedo[3]; float ozoneHalf;
    float bottomR, topR, camAlt, msFactor;
    float lightDir[3];     float sunAngRad;
    float lightE[3];       float skyScale;
    float sunDir[3];       float apMaxKm;
    float sunTopE[3];      float apStartKm;
    float moonTopE[3];     float moonAngRad;
    float poleAxis[3];     float starAngle;
    float nightSky[3];     float apStrength;
    float camPos[3];       uint32_t flags;
    float moonDir[3];      float pad0;
    float invViewProj[16];    // ジッタなし・HLSL の mul(v, M) 用に転置済み
    float invViewProjJ[16];   // ジッタあり
};
static_assert(sizeof(AtmosphereGpuParams) == 23 * 16, "AtmosphereCB(AtmosphereCommon.hlsli)と一致させること");

// 前フレームからの変更検出用。T/MS LUT の内容を決めるパラメータだけを混ぜる(太陽の向き・カメラ高度は含めない)。
inline uint64_t HashMediumParams(const AtmosphereGpuParams& p)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* d, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(d);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    mix(p.rayScat, sizeof(float) * 4);   // rayScat + rayExp
    mix(p.mieScat, sizeof(float) * 4);
    mix(p.mieExt, sizeof(float) * 4);
    mix(p.ozoneExt, sizeof(float) * 4);
    mix(p.groundAlbedo, sizeof(float) * 4);
    mix(&p.bottomR, sizeof(float) * 2);
    mix(&p.msFactor, sizeof(float));
    return h;
}

// 惑星の中心を原点、カメラの標高 camAltKm(>= 0)、太陽の向き sunDir(ワールド。y が上)、カメラ位置(m)から GPU 定数(行列以外)を作る。
// unitScale: 物理単位(lux/nit)なら 1、従来単位なら kClassicUnitScale。
inline void FillGpuParams(AtmosphereGpuParams& g, const AtmosphereSettings& s, const SkyLight& light, const Vec3d& sunDir,
                          double camAltKm, double unitScale, const float camPosWorld[3], double starAngle,
                          const Vec3d& poleAxis)
{
    std::memset(&g, 0, sizeof(g));
    const Params p = MakeParams(s);
    auto setv3 = [](float* d, double a, double b, double c) { d[0] = float(a); d[1] = float(b); d[2] = float(c); };
    setv3(g.rayScat, p.rayleighScattering.r, p.rayleighScattering.g, p.rayleighScattering.b);
    g.rayExp = float(p.rayleighExpScale);
    setv3(g.mieScat, p.mieScattering.r, p.mieScattering.g, p.mieScattering.b);
    g.mieExp = float(p.mieExpScale);
    setv3(g.mieExt, p.mieScattering.r + p.mieAbsorption.r, p.mieScattering.g + p.mieAbsorption.g,
                    p.mieScattering.b + p.mieAbsorption.b);
    g.mieG = float(p.mieG);
    setv3(g.ozoneExt, p.ozoneAbsorption.r, p.ozoneAbsorption.g, p.ozoneAbsorption.b);
    g.ozoneCenter = float(p.ozoneCenter);
    setv3(g.groundAlbedo, p.groundAlbedo.r, p.groundAlbedo.g, p.groundAlbedo.b);
    g.ozoneHalf = float(p.ozoneHalfWidth);
    g.bottomR = float(p.bottomRadius);
    g.topR = float(p.topRadius);
    g.camAlt = float(std::max(camAltKm, 0.0));
    g.msFactor = float(p.multiScatteringFactor);
    setv3(g.lightDir, light.toLight.x, light.toLight.y, light.toLight.z);
    g.sunAngRad = s.sunAngularRadius;
    // 光源の上端照度 RGB(シーン単位)= 照度 × 色 × 単位係数
    const double e = light.illuminanceTopLux * unitScale;
    setv3(g.lightE, e * light.colorTop.r, e * light.colorTop.g, e * light.colorTop.b);
    g.skyScale = s.skyLuminanceScale;
    setv3(g.sunDir, sunDir.x, sunDir.y, sunDir.z);
    g.apMaxKm = std::max(s.apMaxDistanceKm, 1.0f);
    g.apStartKm = std::max(s.apStartDepth, 0.0f) * 0.001f;
    // 太陽円盤の上端照度(月への切替で絞る)/ 月
    {
        const double sunTop = light.isMoon ? 0.0 : light.illuminanceTopLux;
        setv3(g.sunTopE, sunTop * s.sunTint[0] * unitScale, sunTop * s.sunTint[1] * unitScale, sunTop * s.sunTint[2] * unitScale);
        const double moonTop = light.isMoon ? light.illuminanceTopLux : 0.0;
        setv3(g.moonTopE, moonTop * s.moonTint[0] * unitScale, moonTop * s.moonTint[1] * unitScale, moonTop * s.moonTint[2] * unitScale);
        g.moonAngRad = 0.0045f;
    }
    setv3(g.poleAxis, poleAxis.x, poleAxis.y, poleAxis.z);
    g.starAngle = float(starAngle);
    // 夜空の下限: 青みを付けて(星明かり + 大気光)
    setv3(g.nightSky, s.nightSkyNits * 0.55 * unitScale, s.nightSkyNits * 0.75 * unitScale, s.nightSkyNits * 1.0 * unitScale);
    g.apStrength = s.apStrength;
    g.camPos[0] = camPosWorld[0]; g.camPos[1] = camPosWorld[1]; g.camPos[2] = camPosWorld[2];
    g.flags = (s.drawStars ? kFlagStars : 0u) | (s.drawMoon ? kFlagMoon : 0u) | (s.aerialPerspective ? kFlagAp : 0u)
            | (light.isMoon ? kFlagLightIsMoon : 0u);
    setv3(g.moonDir, -sunDir.x, -sunDir.y, -sunDir.z);   // 月は太陽の反対側(従来の夜の月と同じ流儀)
}

// 天の北極の向き(ワールド)。緯度 φ・north yaw から。星空の回転角 = 時角(hour - 12)·15° だけ回す。
inline Vec3d CelestialPoleAxis(double latitudeDeg, double northYawDeg)
{
    return DirectionFromAngles(latitudeDeg, 0.0, northYawDeg);   // 北の地平線から緯度だけ上
}

} // namespace dx12e::atmosphere
