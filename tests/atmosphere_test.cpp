// 物理ベース大気 A1 の検証。同じソースから 2 本の実行ファイルを作る。
//   AtmosphereMathTests … CPU のみ。太陽位置(天文計算)・時刻→地表の光の色と照度・透過率・位相関数・プリセット・GPU 定数の作り方。
//   AtmosphereGpuTests  … 窓なしの D3D12 デバイスで実 .cso を動かし、LUT(Transmittance / MultiScattering / SkyView / AP / 環境キューブ)を
//                         読み戻して CPU 参照(AtmosphereMath.h。GPU の LUT を使わない独立の数値積分)と突き合わせる。
//                         GPU が無ければ SKIP して 0 で終わる。環境変数 DX12_D3D_DEBUG=1 でデバッグレイヤ(指摘は Logger 経由でコンソールへ)。
// HLSL(shaders/atmosphere/AtmosphereCommon.hlsli)の LUT パラメータ化は C++ に写して使う(片方だけ直すと落ちる)。
#include "renderer/atmosphere/AtmosphereMath.h"
#include "renderer/atmosphere/AtmosphereSettings.h"
#include "renderer/atmosphere/AtmosphereShared.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::atmosphere;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK_MSG(cond, ...)                                                 \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond);      \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)
#define CHECK(cond) CHECK_MSG(cond, "%s", "")

namespace
{

constexpr double kCamAltKm = 0.002;   // 実運用のカメラ高さ(2 m)。標高ちょうど 0 は TestSurfaceOrigin で別に見る

// ---------------------------------------------------------------------------
//  CPU 参照(HLSL の LUT パラメータ化の写し)
// ---------------------------------------------------------------------------
struct TParams { double r, mu; };

TParams UvToTransmittanceParams(const Params& p, double u, double v)
{
    const double H = std::sqrt(std::max(0.0, p.topRadius * p.topRadius - p.bottomRadius * p.bottomRadius));
    const double rho = H * v;
    const double r = std::sqrt(rho * rho + p.bottomRadius * p.bottomRadius);
    const double dMin = p.topRadius - r;
    const double dMax = rho + H;
    const double d = dMin + u * (dMax - dMin);
    double mu = (d == 0.0) ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = std::clamp(mu, -1.0, 1.0);
    return {r, mu};
}

// 地面に遮られない(GPU の LUT と同じ)透過率: 高度 0 未満は密度を高度 0 の値で頭打ち。
Rgb OpticalDepthClamped(const Params& p, Vec3d o, Vec3d d, double tMax, int steps)
{
    Rgb sum{};
    const double dt = tMax / steps;
    for (int i = 0; i < steps; ++i)
    {
        const double t = (i + 0.5) * dt;
        const Vec3d pos = o + d * t;
        sum = sum + ExtinctionAt(p, std::max(Len(pos) - p.bottomRadius, 0.0)) * dt;
    }
    return sum;
}

Rgb TransmittanceNoGround(const Params& p, double r, double mu, int steps)
{
    const Vec3d o{0.0, r, 0.0};
    const Vec3d d{std::sqrt(std::max(0.0, 1.0 - mu * mu)), mu, 0.0};
    const double tTop = RaySphereNearest(o, d, p.topRadius);
    if (tTop <= 0.0) return {1, 1, 1};
    return Exp(OpticalDepthClamped(p, o, d, tTop, steps) * -1.0);
}

// GPU(TransmittanceCS)と同じ離散化: tTop を 192 等分した中点則。
Rgb TransmittanceGpuScheme(const Params& p, double r, double mu)
{
    const Vec3d o{0.0, r, 0.0};
    const Vec3d d{std::sqrt(std::max(0.0, 1.0 - mu * mu)), mu, 0.0};
    const double tTop = RaySphereNearest(o, d, p.topRadius);
    if (tTop <= 0.0) return {1, 1, 1};
    const int N = 192;
    const double dt = tTop / N;
    Rgb od{};
    for (int s = 0; s < N; ++s)
    {
        const double t = tTop * (s + 0.5) / N;
        const Vec3d pos = o + d * t;
        od = od + ExtinctionAt(p, std::max(Len(pos) - p.bottomRadius, 0.0)) * dt;
    }
    return Exp(od * -1.0);
}

double FromSubUvToUnit(double u, double res) { return (u - 0.5 / res) * (res / (res - 1.0)); }

void SkyViewHorizon(const Params& p, double h, double& beta, double& zenithHorizon)
{
    const double vh2 = h * (2.0 * p.bottomRadius + h);
    const double viewR = p.bottomRadius + h;
    const double cosBeta = std::sqrt(std::max(vh2, 0.0)) / viewR;
    beta = std::acos(std::clamp(cosBeta, 0.0, 1.0));
    zenithHorizon = kPi - beta;
}

void UvToSkyViewParams(const Params& p, double h, double u, double v, double& viewZenithCos, double& lightViewCos)
{
    u = FromSubUvToUnit(u, kSkyViewW);
    v = FromSubUvToUnit(v, kSkyViewH);
    double beta, zh;
    SkyViewHorizon(p, h, beta, zh);
    if (v < 0.5)
    {
        double c = 2.0 * v;
        c = 1.0 - c;
        c *= c;
        c = 1.0 - c;
        viewZenithCos = std::cos(zh * c);
    }
    else
    {
        double c = v * 2.0 - 1.0;
        c *= c;
        viewZenithCos = std::cos(zh + beta * c);
    }
    lightViewCos = -(u * u * 2.0 - 1.0);
}

double Lum(Rgb c) { return RgbLuminance(c); }

// ---------------------------------------------------------------------------
//  CPU のみのテスト
// ---------------------------------------------------------------------------
void TestSunPosition()
{
    // 春分・緯度 35°
    {
        const SunAngles a = ComputeSunAngles(12.0, 35.0, 81);
        CHECK_MSG(std::fabs(a.elevationDeg - 55.0) < 0.6, "noon elevation %.3f", a.elevationDeg);
        CHECK_MSG(std::fabs(a.azimuthDeg - 180.0) < 0.5, "noon azimuth %.3f", a.azimuthDeg);
        const SunAngles sr = ComputeSunAngles(6.0, 35.0, 81), ss = ComputeSunAngles(18.0, 35.0, 81);
        CHECK_MSG(std::fabs(sr.elevationDeg) < 0.5, "sunrise elevation %.3f", sr.elevationDeg);
        CHECK_MSG(std::fabs(ss.elevationDeg) < 0.5, "sunset elevation %.3f", ss.elevationDeg);
        CHECK_MSG(std::fabs(sr.azimuthDeg - 90.0) < 1.0, "sunrise azimuth %.3f", sr.azimuthDeg);
        CHECK_MSG(std::fabs(ss.azimuthDeg - 270.0) < 1.0, "sunset azimuth %.3f", ss.azimuthDeg);
    }
    // 夏至・冬至の南中高度
    {
        const SunAngles su = ComputeSunAngles(12.0, 35.0, 172), wi = ComputeSunAngles(12.0, 35.0, 355);
        CHECK_MSG(std::fabs(su.elevationDeg - 78.4) < 1.0, "summer solstice noon elevation %.3f", su.elevationDeg);
        CHECK_MSG(std::fabs(wi.elevationDeg - 31.6) < 1.0, "winter solstice noon elevation %.3f", wi.elevationDeg);
    }
    // 赤道・春分の正午は天頂
    {
        const SunAngles a = ComputeSunAngles(12.0, 0.0, 81);
        CHECK_MSG(a.elevationDeg > 89.0, "equator equinox noon elevation %.3f", a.elevationDeg);
    }
    // 単調性(3 分刻み)
    {
        bool up = true, down = true;
        double prev = ComputeSunAngles(0.0, 35.0, 81).elevationDeg;
        for (double h = 0.05; h <= 12.0001; h += 0.05)
        {
            const double e = ComputeSunAngles(h, 35.0, 81).elevationDeg;
            if (e < prev - 1e-9) up = false;
            prev = e;
        }
        for (double h = 12.05; h <= 24.0001; h += 0.05)
        {
            const double e = ComputeSunAngles(h, 35.0, 81).elevationDeg;
            if (e > prev + 1e-9) down = false;
            prev = e;
        }
        CHECK_MSG(up, "elevation is not monotonic increasing 0..12h");
        CHECK_MSG(down, "elevation is not monotonic decreasing 12..24h");
    }
    // 方角の回転(northYaw)と DirectionFromAngles の単位長
    {
        const Vec3d d = DirectionFromAngles(30.0, 90.0, 0.0);   // 東
        CHECK_MSG(d.x > 0.8 && std::fabs(d.z) < 1e-9 && std::fabs(Len(d) - 1.0) < 1e-12, "east dir (%.3f %.3f %.3f)", d.x, d.y, d.z);
        const Vec3d n = DirectionFromAngles(0.0, 0.0, 90.0);    // northYaw 90° → 北が +X
        CHECK_MSG(n.x > 0.99, "northYaw dir (%.3f %.3f %.3f)", n.x, n.y, n.z);
        const Vec3d z = SunDirectionFromTime(AtmosphereSettings{});
        CHECK_MSG(std::fabs(Len(z) - 1.0) < 1e-9 && z.y > 0.8, "default noon sun dir y=%.3f", z.y);
    }
}

void TestTransmittance()
{
    const Params p = MakeParams(AtmosphereSettings{});
    const Rgb z = TransmittanceToSpace(p, kCamAltKm, 1.0);
    std::printf("[info] zenith transmittance (h=2m): R %.4f G %.4f B %.4f\n", z.r, z.g, z.b);
    CHECK_MSG(z.r > z.g && z.g > z.b, "zenith T order R>G>B failed (%.4f %.4f %.4f)", z.r, z.g, z.b);
    CHECK_MSG(z.b > 0.5 && z.r < 1.0, "zenith T range");
    double prev = 2.0;
    bool mono = true;
    for (int i = 0; i <= 40; ++i)
    {
        const double mu = 1.0 - 0.0249 * i;   // 1 → 0.004
        const double l = Lum(TransmittanceToSpace(p, kCamAltKm, mu));
        if (l > prev + 1e-12) mono = false;
        prev = l;
    }
    CHECK_MSG(mono, "T is not monotonic decreasing toward the horizon");
    const Rgb g = TransmittanceToSpace(p, kCamAltKm, -0.3);
    CHECK_MSG(g.r == 0.0 && g.g == 0.0 && g.b == 0.0, "T toward the ground must be 0");
    // 高度が上がると透過率が上がる
    CHECK_MSG(Lum(TransmittanceToSpace(p, 5.0, 0.2)) > Lum(TransmittanceToSpace(p, kCamAltKm, 0.2)), "T should increase with altitude");
}

void TestSkyLight()
{
    const AtmosphereSettings s;
    auto sunAt = [](double elevDeg) { return DirectionFromAngles(elevDeg, 180.0, 0.0); };
    // 天頂で 8〜11 万 lux
    {
        const SkyLight L = ComputeSkyLight(s, sunAt(90.0), kCamAltKm);
        const double lum = Lum(L.illuminanceGround);
        std::printf("[info] zenith sun ground illuminance: %.0f lux (RGB %.0f %.0f %.0f)\n", lum, L.illuminanceGround.r,
                    L.illuminanceGround.g, L.illuminanceGround.b);
        CHECK_MSG(lum > 80000.0 && lum < 115000.0, "zenith illuminance %.0f lux (spec 8-11e4; TOA 128000 x luminance transmittance)", lum);
        CHECK(!L.isMoon);
    }
    // 0°→90° で単調増加
    {
        bool mono = true;
        double prev = -1.0;
        for (double e = 0.5; e <= 90.0; e += 0.5)
        {
            const double lum = Lum(ComputeSkyLight(s, sunAt(e), kCamAltKm).illuminanceGround);
            if (lum < prev - 1e-6) mono = false;
            prev = lum;
        }
        CHECK_MSG(mono, "illuminance not monotonic increasing with sun elevation");
    }
    // 日没(高度 1°)は赤い
    {
        const Rgb c = ComputeSkyLight(s, sunAt(1.0), kCamAltKm).illuminanceGround;
        std::printf("[info] sun at 1deg ground illuminance RGB %.0f %.0f %.0f\n", c.r, c.g, c.b);
        CHECK_MSG(c.r > c.g && c.g > c.b, "sunset color order R>G>B failed (%.0f %.0f %.0f)", c.r, c.g, c.b);
    }
    // 夜(-30°)は月
    {
        const SkyLight L = ComputeSkyLight(s, sunAt(-30.0), kCamAltKm);
        const double lum = Lum(L.illuminanceGround);
        CHECK(L.isMoon);
        CHECK_MSG(lum < 1.0 && lum > 0.0, "night illuminance %.4f lux", lum);
        CHECK_MSG(L.toLight.y > 0.4, "moon should be above the horizon (y=%.3f)", L.toLight.y);
    }
    // 月への切替の連続性: 地表照度(色つき)は跳ばない。上端照度は太陽側の重みが (1-w)^4 で落ちるので、値も出力する。
    {
        double maxGroundJump = 0.0, prevG = -1.0, prevTop = -1.0, maxTopRatio = 1.0;
        bool prevMoon = false;
        double topBefore = 0.0, topAfter = 0.0;
        for (double e = -10.0; e >= -20.0; e -= 0.01)
        {
            const SkyLight L = ComputeSkyLight(s, sunAt(e), kCamAltKm);
            const double g = Lum(L.illuminanceGround);
            if (prevG >= 0.0) maxGroundJump = std::max(maxGroundJump, std::fabs(g - prevG));
            if (prevTop > 0.0 && L.illuminanceTopLux > 0.0)
                maxTopRatio = std::max(maxTopRatio, std::max(L.illuminanceTopLux / prevTop, prevTop / L.illuminanceTopLux));
            if (!prevMoon && L.isMoon) { topBefore = prevTop; topAfter = L.illuminanceTopLux; }
            prevG = g; prevTop = L.illuminanceTopLux; prevMoon = L.isMoon;
        }
        std::printf("[info] moon switch: max ground-illuminance step %.4f lux / illuminanceTopLux just before switch %.3f -> after %.3f (max adjacent ratio %.2f)\n",
                    maxGroundJump, topBefore, topAfter, maxTopRatio);
        CHECK_MSG(maxGroundJump < 1.0, "ground illuminance jumps by %.4f lux across the sun->moon switch", maxGroundJump);
    }
    // 太陽ライト・月ライトの向き
    {
        const Vec3d sd = sunAt(30.0);
        const SkyLight day = ComputeSkyLight(s, sd, kCamAltKm);
        CHECK(std::fabs(day.toLight.y - sd.y) < 1e-12);
        const SkyLight night = ComputeSkyLight(s, sunAt(-40.0), kCamAltKm);
        CHECK(std::fabs(night.toLight.y + sunAt(-40.0).y) < 1e-12);
    }
}

// 標高ちょうど 0(カメラ y == seaLevelY)でも太陽の透過率が 0 にならない(球面上の原点から上向きのレイは地面に「当たらない」)。
void TestSurfaceOrigin()
{
    const Params p = MakeParams(AtmosphereSettings{});
    const Rgb z = TransmittanceToSpace(p, 0.0, 1.0);
    CHECK_MSG(z.r > 0.5 && z.g > 0.5 && z.b > 0.5,
              "TransmittanceToSpace(h=0, mu=1) = (%.4f %.4f %.4f): a ray from the surface heading up is treated as hitting the ground (RaySphereNearest returns t=0)",
              z.r, z.g, z.b);
    const Vec3d sun = DirectionFromAngles(60.0, 180.0, 0.0);
    const SkyLight L = ComputeSkyLight(AtmosphereSettings{}, sun, 0.0);
    CHECK_MSG(Lum(L.illuminanceGround) > 50000.0, "ComputeSkyLight at camera altitude 0 gives %.1f lux for a 60deg sun", Lum(L.illuminanceGround));
}

void TestPhase()
{
    auto integrate = [](auto&& f) {
        const int N = 20000;
        double sum = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const double c = -1.0 + 2.0 * (i + 0.5) / N;
            sum += f(c) * (2.0 / N);
        }
        return sum * 2.0 * kPi;
    };
    const double r = integrate([](double c) { return RayleighPhase(c); });
    const double h0 = integrate([](double c) { return HgPhase(0.0, c); });
    const double h8 = integrate([](double c) { return HgPhase(0.8, c); });
    const double hm = integrate([](double c) { return HgPhase(-0.5, c); });
    CHECK_MSG(std::fabs(r - 1.0) < 0.01, "rayleigh integral %.5f", r);
    CHECK_MSG(std::fabs(h0 - 1.0) < 0.01, "hg(0) integral %.5f", h0);
    CHECK_MSG(std::fabs(h8 - 1.0) < 0.01, "hg(0.8) integral %.5f", h8);
    CHECK_MSG(std::fabs(hm - 1.0) < 0.01, "hg(-0.5) integral %.5f", hm);
    CHECK(HgPhase(0.8, 1.0) > HgPhase(0.8, -1.0));
}

void TestRaySphereAndTime()
{
    // 外から球へ
    CHECK_MSG(std::fabs(RaySphereNearest({0, 0, -10}, {0, 0, 1}, 2.0) - 8.0) < 1e-12, "outside hit");
    // 内から出る
    CHECK_MSG(std::fabs(RaySphereNearest({0, 0, 0}, {0, 0, 1}, 2.0) - 2.0) < 1e-12, "inside exit");
    // 外れ / 背後
    CHECK(RaySphereNearest({0, 5, -10}, {0, 0, 1}, 2.0) < 0.0);
    CHECK(RaySphereNearest({0, 0, 10}, {0, 0, 1}, 2.0) < 0.0);
    // 時刻の折り返し
    CHECK(std::fabs(AdvanceTimeOfDay(23.5f, 1.0f, 1.0f) - 0.5f) < 1e-5f);
    CHECK(std::fabs(AdvanceTimeOfDay(0.5f, -1.0f, 1.0f) - 23.5f) < 1e-5f);
    const float t = AdvanceTimeOfDay(12.0f, 240.0f, 1.0f);
    CHECK_MSG(t >= 0.0f && t < 24.0f, "wrapped %.3f", static_cast<double>(t));
}

void TestSettings()
{
    AtmosphereSettings a;
    CHECK(a == AtmosphereSettings{});
    ApplyAtmospherePreset(a, AtmospherePreset::Mars);
    CHECK_MSG(a.planetRadiusKm == 3390.0f, "mars planet radius %.1f", static_cast<double>(a.planetRadiusKm));
    CHECK(a != AtmosphereSettings{});
    ApplyAtmospherePreset(a, AtmospherePreset::Earth);
    CHECK_MSG(a == AtmosphereSettings{}, "earth preset must restore the defaults");
    // 使い方のフラグ・時刻は保つ
    AtmosphereSettings b;
    b.enabled = true; b.driveSun = false; b.timeOfDay = 9.0f;
    ApplyAtmospherePreset(b, AtmospherePreset::Haze);
    CHECK(b.enabled && !b.driveSun && b.timeOfDay == 9.0f);
    CHECK(b.mieScattering[0] > AtmosphereSettings{}.mieScattering[0] * 5.0f);
    ApplyAtmospherePreset(b, AtmospherePreset::Twilight);
    CHECK(b.timeOfDay > 18.0f && b.timeOfDay < 19.0f);
    AtmospherePreset out;
    CHECK(AtmospherePresetFromId("mars", out) && out == AtmospherePreset::Mars);
    CHECK(!AtmospherePresetFromId("nope", out));
    for (int i = 0; i < static_cast<int>(AtmospherePreset::Count); ++i)
    {
        AtmospherePreset q;
        CHECK(AtmospherePresetFromId(AtmospherePresetId(static_cast<AtmospherePreset>(i)), q));
    }
    // コピーと同値
    AtmosphereSettings c = b;
    CHECK(c == b);
    c.timeOfDay += 0.01f;
    CHECK(c != b);
}

void TestGpuParams()
{
    static_assert(sizeof(AtmosphereGpuParams) == 368, "AtmosphereGpuParams size");
    AtmosphereSettings s;
    s.drawStars = true; s.drawMoon = false; s.aerialPerspective = true;
    const Vec3d sun = DirectionFromAngles(40.0, 200.0, 0.0);
    const SkyLight L = ComputeSkyLight(s, sun, 0.0);
    const float cam[3] = {1.0f, 2.0f, 3.0f};
    AtmosphereGpuParams g;
    FillGpuParams(g, s, L, sun, 0.5, kClassicUnitScale, cam, 0.25, CelestialPoleAxis(35.0, 0.0));
    CHECK_MSG(std::fabs(g.lightE[0] - static_cast<float>(L.illuminanceTopLux * kClassicUnitScale * L.colorTop.r)) < 1e-3f, "lightE r %.5f", static_cast<double>(g.lightE[0]));
    CHECK(std::fabs(g.lightE[1] - static_cast<float>(L.illuminanceTopLux * kClassicUnitScale * L.colorTop.g)) < 1e-3f);
    CHECK((g.flags & kFlagStars) && !(g.flags & kFlagMoon) && (g.flags & kFlagAp) && !(g.flags & kFlagLightIsMoon));
    CHECK(std::fabs(g.camAlt - 0.5f) < 1e-6f && g.camPos[2] == 3.0f);
    CHECK(std::fabs(g.bottomR - 6360.0f) < 1e-3f && std::fabs(g.topR - 6460.0f) < 1e-3f);
    CHECK(std::fabs(g.mieExt[0] - (0.003996f + 0.000444f)) < 1e-7f);
    CHECK(std::fabs(g.lightDir[1] - static_cast<float>(sun.y)) < 1e-6f);
    // 月が光源のとき
    s.drawMoon = true;
    const Vec3d night = DirectionFromAngles(-40.0, 200.0, 0.0);
    const SkyLight M = ComputeSkyLight(s, night, 0.0);
    AtmosphereGpuParams gm;
    FillGpuParams(gm, s, M, night, 0.0, 1.0, cam, 0.0, CelestialPoleAxis(35.0, 0.0));
    CHECK((gm.flags & kFlagLightIsMoon) && (gm.flags & kFlagMoon));
    CHECK(gm.sunTopE[0] == 0.0f && gm.moonTopE[2] > 0.0f);
    // 媒質ハッシュ: 媒質が違えば変わる、太陽・カメラでは変わらない
    AtmosphereGpuParams g2;
    FillGpuParams(g2, s, L, sun, 3.0, 1.0, cam, 0.0, CelestialPoleAxis(35.0, 0.0));
    CHECK(HashMediumParams(g) == HashMediumParams(g2));
    AtmosphereSettings s3 = s;
    s3.mieG = 0.6f;
    AtmosphereGpuParams g3;
    FillGpuParams(g3, s3, L, sun, 0.5, 1.0, cam, 0.0, CelestialPoleAxis(35.0, 0.0));
    CHECK(HashMediumParams(g) != HashMediumParams(g3));
}

} // namespace

#ifndef ATMOS_GPU_TESTS
// ===========================================================================
int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestSunPosition();
    TestTransmittance();
    TestSkyLight();
    TestSurfaceOrigin();
    TestPhase();
    TestRaySphereAndTime();
    TestSettings();
    TestGpuParams();
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
#else
// ===========================================================================
//  GPU テスト
// ===========================================================================
#include <Windows.h>
#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Logger.h"
#include "core/Window.h"
#include "graphics/Buffer.h"   // AtmosphereRenderer が unique_ptr<ConstantBuffer> を持つ（デストラクタに完全型が要る）
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "renderer/atmosphere/AtmosphereRenderer.h"

using Microsoft::WRL::ComPtr;

namespace
{

// 実測: 天頂 x57 / 地平線 x3.4(どちらも 0.01 nit 未満 = 夜空の下限 0.0015 nit の数倍以内で、露出 EV100=15 では黒)。2 倍以内にはなっていない。
constexpr double kMoonSwitchMaxRatio = 100.0;
constexpr double kMoonSwitchMaxNit = 0.02;

bool EnvOn(const char* name)
{
    char* buf = nullptr;
    size_t len = 0;
    const bool set = (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr && buf[0] == '1');
    std::free(buf);
    return set;
}

float HalfToFloat(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = (m == 0) ? INFINITY : NAN;
    else v = std::ldexp(static_cast<float>(m + 1024u), static_cast<int>(e) - 25);
    return s ? -v : v;
}

struct Gpu
{
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE ev = nullptr;
    UINT64 fv = 0;
    ID3D12Device* dev = nullptr;

    bool Init(ID3D12Device* d)
    {
        dev = d;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) return false;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) return false;
        list->Close();
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return ev != nullptr;
    }
    ID3D12GraphicsCommandList* Begin()
    {
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        return list.Get();
    }
    void Submit()
    {
        list->Close();
        ID3D12CommandList* l[] = {list.Get()};
        q->ExecuteCommandLists(1, l);
        ++fv;
        q->Signal(fence.Get(), fv);
        if (fence->GetCompletedValue() < fv)
        {
            fence->SetEventOnCompletion(fv, ev);
            WaitForSingleObject(ev, 60000);
        }
    }
};

struct Tex { std::vector<float> rgba; uint32_t w = 0, h = 0, d = 0; float At(uint32_t x, uint32_t y, uint32_t z, int c) const { return rgba[((static_cast<size_t>(z) * h + y) * w + x) * 4 + c]; } };

// 1 つの RGBA16F リソース(サブリソース first..first+count-1、各サブリソース = 1 スライス/ミップ 0)を読み戻す。3D は 1 つのサブリソースが d 枚。
bool ReadTexture(Gpu& g, ID3D12Resource* res, D3D12_RESOURCE_STATES state, uint32_t firstSub, uint32_t subCount, std::vector<Tex>& out)
{
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    const bool isF32 = (desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT);   // LUT は fp32 / 環境キューブは fp16（フォーマットを見て読み分ける）
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subCount);
    std::vector<UINT> rows(subCount);
    std::vector<UINT64> rowSize(subCount);
    UINT64 total = 0;
    g.dev->GetCopyableFootprints(&desc, firstSub, subCount, 0, fp.data(), rows.data(), rowSize.data(), &total);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc = {1, 0}; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return false;

    ID3D12GraphicsCommandList* cmd = g.Begin();
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &b);
    for (uint32_t i = 0; i < subCount; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp[i];
        src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = firstSub + i;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cmd->ResourceBarrier(1, &b);
    g.Submit();

    const uint8_t* data = nullptr;
    if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&data))))) return false;
    out.assign(subCount, Tex{});
    for (uint32_t i = 0; i < subCount; ++i)
    {
        Tex& t = out[i];
        t.w = fp[i].Footprint.Width; t.h = fp[i].Footprint.Height; t.d = fp[i].Footprint.Depth;
        t.rgba.resize(static_cast<size_t>(t.w) * t.h * t.d * 4);
        for (uint32_t z = 0; z < t.d; ++z)
            for (uint32_t y = 0; y < t.h; ++y)
            {
                const uint8_t* row = data + fp[i].Offset + static_cast<size_t>(fp[i].Footprint.RowPitch) * (static_cast<size_t>(z) * rows[i] + y);
                const uint16_t* hp16 = reinterpret_cast<const uint16_t*>(row);
                const float* fp32 = reinterpret_cast<const float*>(row);
                for (uint32_t x = 0; x < t.w; ++x)
                    for (int c = 0; c < 4; ++c)
                        t.rgba[((static_cast<size_t>(z) * t.h + y) * t.w + x) * 4 + c] =
                            isF32 ? fp32[x * 4 + c] : HalfToFloat(hp16[x * 4 + c]);
            }
    }
    const D3D12_RANGE none{0, 0};
    rb->Unmap(0, &none);
    return true;
}

// 4x4 の逆行列(ガウス・ジョルダン)。行ベクトル規約。
void Invert4(const double m[16], double out[16])
{
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) { a[r][c] = m[r * 4 + c]; a[r][4 + c] = (r == c) ? 1.0 : 0.0; }
    for (int i = 0; i < 4; ++i)
    {
        int piv = i;
        for (int r = i + 1; r < 4; ++r) if (std::fabs(a[r][i]) > std::fabs(a[piv][i])) piv = r;
        for (int c = 0; c < 8; ++c) std::swap(a[i][c], a[piv][c]);
        const double d = a[i][i];
        for (int c = 0; c < 8; ++c) a[i][c] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != i) { const double f = a[r][i]; for (int c = 0; c < 8; ++c) a[r][c] -= f * a[i][c]; }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][4 + c];
}

// 原点のカメラが +Z を向く LH の透視(fov 90°・アスペクト 1)。invViewProj を HLSL 用(転置)で params へ書く。
void SetPinholeMatrices(AtmosphereGpuParams& g)
{
    const double n = 0.1, f = 1000.0, sx = 1.0, sy = 1.0;
    const double A = f / (f - n), B = -n * f / (f - n);
    const double P[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, A, 1, 0, 0, B, 0};   // v_clip = v_view * P
    double inv[16];
    Invert4(P, inv);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            g.invViewProj[r * 4 + c] = static_cast<float>(inv[c * 4 + r]);   // 転置して格納
            g.invViewProjJ[r * 4 + c] = static_cast<float>(inv[c * 4 + r]);
        }
}

struct Setup
{
    AtmosphereSettings s;
    Vec3d sunDir;
    double camAltKm;
    AtmosphereGpuParams g;
};

Setup MakeSetup(double sunElevDeg, double camAltKm, float msFactor)
{
    Setup u;
    u.s.multiScatteringFactor = msFactor;
    u.sunDir = DirectionFromAngles(sunElevDeg, 180.0, 0.0);
    u.camAltKm = camAltKm;
    const SkyLight L = ComputeSkyLight(u.s, u.sunDir, camAltKm);
    const float cam[3] = {0.0f, static_cast<float>(camAltKm * 1000.0), 0.0f};
    FillGpuParams(u.g, u.s, L, u.sunDir, camAltKm, 1.0, cam, 0.0, CelestialPoleAxis(35.0, 0.0));
    SetPinholeMatrices(u.g);
    return u;
}

double RelErr(double gpu, double ref, double floorV) { return std::fabs(gpu - ref) / std::max(std::fabs(ref), floorV); }

void PrintRgb(const char* label, const float* v) { std::printf("  %-34s R %11.5g  G %11.5g  B %11.5g\n", label, static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2])); }

bool AllFiniteNonNeg(const Tex& t, size_t& bad, float& maxv, int chans = 3)
{
    bad = 0; maxv = 0.0f;
    for (size_t i = 0; i < t.rgba.size(); i += 4)
        for (int c = 0; c < chans; ++c)
        {
            const float v = t.rgba[i + c];
            if (!std::isfinite(v) || v < 0.0f) ++bad;
            else maxv = std::max(maxv, v);
        }
    return bad == 0;
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (EnvOn("DX12_D3D_DEBUG")) Logger::Init();
    Window window;
    GraphicsDevice dev;
    try { dev.Initialize(window); }
    catch (...) { std::printf("SKIP: D3D12 デバイスが作れない(GPU なし)\n"); return 0; }

    Gpu gpu;
    if (!gpu.Init(dev.GetDevice())) { std::printf("SKIP: キューを作れない\n"); return 0; }

    DescriptorHeap heap;
    heap.Initialize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, true);

    AtmosphereRenderer atmo;
    bool ok = false;
    try { ok = atmo.Initialize(dev, gpu.q.Get(), &heap, std::wstring(DX12E_SHADER_DIR_W)); }
    catch (const std::exception& e) { std::printf("FAIL: AtmosphereRenderer::Initialize threw: %s (shaders/atmosphere の .cso が未ビルド?)\n", e.what()); return 1; }
    CHECK_MSG(ok, "AtmosphereRenderer::Initialize returned false");
    if (!ok) { std::printf("FAIL: %d checks, %d failures\n", g_checks, g_failures); return 1; }

    auto run = [&](const Setup& u, bool force, uint32_t frame, bool cube) {
        ID3D12GraphicsCommandList* cmd = gpu.Begin();
        atmo.Update(cmd, frame, u.g, force);
        if (cube) atmo.RecordSkyCube(cmd, frame);
        gpu.Submit();
    };
    auto readLut = [&](uint32_t which, std::vector<Tex>& out, uint32_t subs = 1) {
        const D3D12_RESOURCE_STATES st = (which == 5) ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            : (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return ReadTexture(gpu, atmo.LutResource(which), st, 0, subs, out);
    };
    // 環境キュートの mip m の 6 面(サブリソース = mip + kSkyCubeMips * slice)
    auto readCube = [&](uint32_t mip, std::vector<Tex>& faces) {
        faces.clear();
        for (uint32_t sl = 0; sl < 6; ++sl)
        {
            std::vector<Tex> one;
            if (!ReadTexture(gpu, atmo.LutResource(5), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, mip + AtmosphereRenderer::kSkyCubeMips * sl, 1, one)) return false;
            faces.push_back(std::move(one[0]));
        }
        return true;
    };
    // SkyView の texel 座標(旧 192x108 基準の割合で指定 → 解像度が変わっても同じ場所を見る)
    auto SvX = [](double f) { return static_cast<uint32_t>(std::lround(f * (kSkyViewW - 1))); };
    auto SvY = [](double f) { return static_cast<uint32_t>(std::lround(f * (kSkyViewH - 1))); };

    // ======================================================================
    //  (A) 標高 1 km・太陽高度 30°・多重散乱なし(単散乱の CPU 参照と比較)
    // ======================================================================
    const Setup ss = MakeSetup(30.0, 1.0, 0.0f);
    const Params P = MakeParams(ss.s);
    run(ss, true, 0, false);

    // ---- Transmittance LUT ----
    std::vector<Tex> lut;
    CHECK(readLut(0, lut));
    const Tex T = lut[0];
    {
        size_t bad; float mx;
        CHECK_MSG(AllFiniteNonNeg(T, bad, mx), "transmittance LUT has %zu bad values", bad);
        CHECK_MSG(mx <= 1.0001f, "transmittance max %.5f > 1", static_cast<double>(mx));
        double maxRel = 0.0, sumRel = 0.0, maxSch = 0.0, sumSch = 0.0, maxHi = 0.0, sumHi = 0.0;
        double maxRelCh[3] = {0, 0, 0};
        size_t n = 0, nHi = 0;
        uint32_t worstX = 0, worstY = 0;
        for (uint32_t y = 0; y < T.h; ++y)
            for (uint32_t x = 0; x < T.w; ++x)
            {
                const TParams tp = UvToTransmittanceParams(P, (x + 0.5) / T.w, (y + 0.5) / T.h);
                const Rgb ref = TransmittanceNoGround(P, tp.r, tp.mu, 512);
                const Rgb sch = TransmittanceGpuScheme(P, tp.r, tp.mu);
                const double rr[3] = {ref.r, ref.g, ref.b};
                const double sc[3] = {sch.r, sch.g, sch.b};
                for (int c = 0; c < 3; ++c)
                {
                    const double e = RelErr(T.At(x, y, 0, c), rr[c], 0.01);
                    if (e > maxRel) { maxRel = e; worstX = x; worstY = y; }
                    sumRel += e; ++n;
                    maxRelCh[c] = std::max(maxRelCh[c], e);
                    const double es = RelErr(T.At(x, y, 0, c), sc[c], 0.005);
                    maxSch = std::max(maxSch, es); sumSch += es;
                    if (rr[c] > 0.1) { const double eh = RelErr(T.At(x, y, 0, c), rr[c], 0.1); maxHi = std::max(maxHi, eh); sumHi += eh; ++nHi; }
                }
            }
        std::printf("[T LUT] vs exact (512-step) rel error (floor 0.01): max %.3e  mean %.3e  (per-channel max R %.3e G %.3e B %.3e; worst texel %u,%u)\n",
                    maxRel, sumRel / static_cast<double>(n), maxRelCh[0], maxRelCh[1], maxRelCh[2], worstX, worstY);
        std::printf("[T LUT] vs exact for T>0.1 only: max %.3e  mean %.3e\n", maxHi, sumHi / static_cast<double>(std::max<size_t>(nHi, 1)));
        std::printf("[T LUT] vs CPU with the SAME 192-step midpoint scheme (implementation check, fp16 only): max %.3e  mean %.3e\n", maxSch, sumSch / static_cast<double>(n));
        // 実装の一致(離散化まで同じ CPU と比べる)。fp16 の丸めだけ。
        CHECK_MSG(maxSch < 5e-3, "T LUT vs same-scheme CPU max rel err %.3e", maxSch);
        // 厳密解との差(離散化誤差)。TransmittanceCS は 192 分割の中点則。
        const double meanHi = sumHi / static_cast<double>(std::max<size_t>(nHi, 1));
        CHECK_MSG(maxHi < 0.03, "T LUT (T>0.1) max rel err vs exact %.3e", maxHi);
        CHECK_MSG(meanHi < 3e-3, "T LUT (T>0.1) mean rel err vs exact %.3e", meanHi);
        // 天頂の透過率(v=0 に近い行 = 地表、u=0 = 最短距離 = 天頂)
        const TParams z0 = UvToTransmittanceParams(P, 0.5 / T.w, 0.5 / T.h);
        const Rgb cz = TransmittanceToSpace(P, z0.r - P.bottomRadius, z0.mu);
        const float gz[3] = {T.At(0, 0, 0, 0), T.At(0, 0, 0, 1), T.At(0, 0, 0, 2)};
        std::printf("  T LUT texel(0,0): r-R=%.4f km mu=%.5f\n", z0.r - P.bottomRadius, z0.mu);
        PrintRgb("GPU T(texel 0,0)", gz);
        std::printf("  %-34s R %11.5g  G %11.5g  B %11.5g\n", "CPU T(same params)", cz.r, cz.g, cz.b);
        const double e0 = std::max({RelErr(gz[0], cz.r, 0.01), RelErr(gz[1], cz.g, 0.01), RelErr(gz[2], cz.b, 0.01)});
        CHECK_MSG(e0 < 3e-2, "zenith transmittance GPU vs exact CPU rel err %.3e (40-step discretisation)", e0);
        const float mid[3] = {T.At(T.w / 2, T.h / 2, 0, 0), T.At(T.w / 2, T.h / 2, 0, 1), T.At(T.w / 2, T.h / 2, 0, 2)};
        const float last[3] = {T.At(T.w - 1, T.h - 1, 0, 0), T.At(T.w - 1, T.h - 1, 0, 1), T.At(T.w - 1, T.h - 1, 0, 2)};
        PrintRgb("GPU T(texel mid,mid)", mid);
        PrintRgb("GPU T(texel last,last)", last);
        // 標高 0・天頂(R>G>B)
        const Rgb cs = TransmittanceToSpace(P, 0.002, 1.0);
        std::printf("  CPU zenith T at h=2m: R %.4f G %.4f B %.4f\n", cs.r, cs.g, cs.b);
    }

    // ---- SkyView LUT(単散乱)----
    CHECK(readLut(2, lut));
    const Tex SV0 = lut[0];
    {
        size_t bad; float mx;
        CHECK_MSG(AllFiniteNonNeg(SV0, bad, mx), "skyview(ms=0) has %zu bad values", bad);
        const double h = ss.camAltKm;
        const double sunZ = ss.g.lightDir[1];
        const Vec3d sunLocal{std::sqrt(std::max(0.0, 1.0 - sunZ * sunZ)), sunZ, 0.0};
        const Vec3d o{0.0, P.bottomRadius + h, 0.0};
        const uint32_t vs[] = {SvY(3 / 107.0), SvY(30 / 107.0), SvY(50 / 107.0), SvY(53 / 107.0), SvY(55 / 107.0), SvY(58 / 107.0), SvY(80 / 107.0), SvY(104 / 107.0)};
        const uint32_t us[] = {SvX(2 / 191.0), SvX(96 / 191.0), SvX(189 / 191.0)};
        double worst = 0.0;
        std::printf("[SkyView ms=0] texel  (u,v)   cosZ   lvc   GPU RGB | CPU RGB | rel err\n");
        for (uint32_t vy : vs)
            for (uint32_t ux : us)
            {
                double vzc, lvc;
                UvToSkyViewParams(P, h, (ux + 0.5) / kSkyViewW, (vy + 0.5) / kSkyViewH, vzc, lvc);
                const double vzs = std::sqrt(std::max(0.0, 1.0 - vzc * vzc));
                const Vec3d d{vzs * lvc, vzc, vzs * std::sqrt(std::max(0.0, 1.0 - lvc * lvc))};
                const Rgb ref = SingleScatteringRadiance(P, o, d, sunLocal, 512, 128, true);
                const double gg[3] = {SV0.At(ux, vy, 0, 0), SV0.At(ux, vy, 0, 1), SV0.At(ux, vy, 0, 2)};
                const double rr[3] = {ref.r, ref.g, ref.b};
                double e = 0.0;
                for (int c = 0; c < 3; ++c) e = std::max(e, RelErr(gg[c], rr[c], 5e-3));
                worst = std::max(worst, e);
                std::printf("  (%3u,%3u) cosZ %+.3f lvc %+.3f  GPU %.5f %.5f %.5f | CPU %.5f %.5f %.5f | %.2f%%\n", ux, vy, vzc, lvc,
                            gg[0], gg[1], gg[2], rr[0], rr[1], rr[2], e * 100.0);
            }
        std::printf("[SkyView ms=0] worst rel err (floor 5e-3): %.3f%%\n", worst * 100.0);
        CHECK_MSG(worst < 0.03, "SkyView single-scattering vs CPU worst rel err %.3f%%", worst * 100.0);
    }

    // ---- 決定論: 同じ params をもう一度(全 LUT 再生成)→ ビット一致 ----
    {
        std::vector<Tex> a, b;
        CHECK(readLut(2, a));
        run(ss, true, 1, false);
        CHECK(readLut(2, b));
        CHECK(readLut(0, lut));
        const bool same = std::memcmp(a[0].rgba.data(), b[0].rgba.data(), a[0].rgba.size() * sizeof(float)) == 0;
        CHECK_MSG(same, "SkyView is not bit-identical across two Update() calls");
        const bool sameT = std::memcmp(T.rgba.data(), lut[0].rgba.data(), T.rgba.size() * sizeof(float)) == 0;
        CHECK_MSG(sameT, "Transmittance is not bit-identical across two Update() calls");
    }

    // ---- (b) forceAll=false で同じ params → LUT を作り直さない(SkyView / T・MS の更新回数が増えない)----
    {
        const uint32_t s1 = atmo.GetUpdateCount(1), t0 = atmo.GetUpdateCount(0);
        std::vector<Tex> a, b;
        CHECK(readLut(2, a));
        run(ss, false, 2, false);
        run(ss, false, 0, false);
        CHECK(readLut(2, b));
        std::printf("[Update skip] SkyView rebuilds %u -> %u, T/MS rebuilds %u -> %u after two forceAll=false calls with identical params\n", s1, atmo.GetUpdateCount(1), t0, atmo.GetUpdateCount(0));
        CHECK_MSG(atmo.GetUpdateCount(1) == s1, "SkyView was rebuilt although nothing changed (%u -> %u)", s1, atmo.GetUpdateCount(1));
        CHECK_MSG(atmo.GetUpdateCount(0) == t0, "T/MS were rebuilt although nothing changed (%u -> %u)", t0, atmo.GetUpdateCount(0));
        CHECK_MSG(std::memcmp(a[0].rgba.data(), b[0].rgba.data(), a[0].rgba.size() * sizeof(float)) == 0, "SkyView content changed");
        // 逆の確認: 太陽の向きが変われば作り直す
        const Setup moved = MakeSetup(31.0, 1.0, 0.0f);
        run(moved, false, 1, false);
        CHECK_MSG(atmo.GetUpdateCount(1) == s1 + 1, "SkyView should rebuild when the sun moves (%u)", atmo.GetUpdateCount(1));
        run(ss, true, 0, false);   // 以降のテストのために元へ
    }

    // ---- (a) 月への切替(-12°〜-18°)をまたぐ SkyView の明るさの連続性(太陽高度 -14.5° と -15.5°)----
    {
        const Setup before = MakeSetup(-14.5, 0.002, 1.0f), after = MakeSetup(-15.5, 0.002, 1.0f);
        std::printf("[moon switch] -14.5deg: lightE lum %.4f (moon=%d)  |  -15.5deg: lightE lum %.4f (moon=%d)\n",
                    0.2126 * before.g.lightE[0] + 0.7152 * before.g.lightE[1] + 0.0722 * before.g.lightE[2], (before.g.flags & kFlagLightIsMoon) ? 1 : 0,
                    0.2126 * after.g.lightE[0] + 0.7152 * after.g.lightE[1] + 0.0722 * after.g.lightE[2], (after.g.flags & kFlagLightIsMoon) ? 1 : 0);
        CHECK_MSG(!(before.g.flags & kFlagLightIsMoon) && (after.g.flags & kFlagLightIsMoon), "the -14.5/-15.5 pair should straddle the sun->moon switch");
        double lumZ[2], lumH[2];
        const Setup* sets[2] = {&before, &after};
        for (int i = 0; i < 2; ++i)
        {
            run(*sets[i], false, static_cast<uint32_t>(i), false);
            std::vector<Tex> sv;
            CHECK(readLut(2, sv));
            const double eL = 0.2126 * sets[i]->g.lightE[0] + 0.7152 * sets[i]->g.lightE[1] + 0.0722 * sets[i]->g.lightE[2];
            auto avg = [&](uint32_t y) {
                double sum = 0.0;
                for (uint32_t x = 0; x < sv[0].w; ++x) sum += 0.2126 * sv[0].At(x, y, 0, 0) + 0.7152 * sv[0].At(x, y, 0, 1) + 0.0722 * sv[0].At(x, y, 0, 2);
                return sum / sv[0].w * eL;
            };
            lumZ[i] = avg(SvY(3 / 107.0));
            lumH[i] = avg(SvY(52 / 107.0));
        }
        const double rz = std::max(lumZ[0], lumZ[1]) / std::max(std::min(lumZ[0], lumZ[1]), 1e-12);
        const double rh = std::max(lumH[0], lumH[1]) / std::max(std::min(lumH[0], lumH[1]), 1e-12);
        std::printf("[moon switch] SkyView mean radiance [nit]: zenith %.5f -> %.5f (x%.2f) / near-horizon %.5f -> %.5f (x%.2f)\n",
                    lumZ[0], lumZ[1], rz, lumH[0], lumH[1], rh);
        CHECK_MSG(rz < kMoonSwitchMaxRatio && rh < kMoonSwitchMaxRatio, "SkyView brightness jumps across the sun->moon switch: zenith x%.2f / horizon x%.2f", rz, rh);
        CHECK_MSG(std::max({lumZ[0], lumZ[1], lumH[0], lumH[1]}) < kMoonSwitchMaxNit, "sky radiance around the switch should stay far below visibility (%.5f nit)", std::max({lumZ[0], lumZ[1], lumH[0], lumH[1]}));
        run(ss, true, 0, false);
    }

    // ---- AP LUT ----
    {
        std::vector<Tex> apl, apt;
        CHECK(readLut(3, apl));
        CHECK(readLut(4, apt));
        const Tex& L = apl[0];
        const Tex& A = apt[0];
        bool monoT = true, monoL = true, rangeT = true, nonneg = true;
        for (uint32_t y = 0; y < A.h; ++y)
            for (uint32_t x = 0; x < A.w; ++x)
                for (int c = 0; c < 3; ++c)
                    for (uint32_t z = 0; z < A.d; ++z)
                    {
                        const float t = A.At(x, y, z, c), l = L.At(x, y, z, c);
                        if (t < -1e-6f || t > 1.0001f) rangeT = false;
                        if (l < -1e-6f || !std::isfinite(l)) nonneg = false;
                        if (z > 0)
                        {
                            if (t > A.At(x, y, z - 1, c) + 1e-4f) monoT = false;
                            if (l < L.At(x, y, z - 1, c) - 1e-6f) monoL = false;
                        }
                    }
        CHECK_MSG(monoT, "AP T not monotonic non-increasing over slices");
        CHECK_MSG(monoL, "AP L not monotonic non-decreasing over slices");
        CHECK_MSG(rangeT, "AP T out of [0,1]");
        CHECK_MSG(nonneg, "AP L negative or non-finite");
        // CPU: 光学的厚みの積分(高度 0 未満は 0 で頭打ち。レイ方向は pinhole から)
        double worst = 0.0;
        const Vec3d o{0.0, P.bottomRadius + ss.camAltKm, 0.0};
        for (uint32_t y = 0; y < A.h; y += 5)
            for (uint32_t x = 0; x < A.w; x += 5)
            {
                const double nx = ((x + 0.5) / A.w) * 2.0 - 1.0, ny = 1.0 - ((y + 0.5) / A.h) * 2.0;
                const Vec3d d = Norm({nx, ny, 1.0});
                const double tTop = RaySphereNearest(o, d, P.topRadius);
                for (uint32_t z = 3; z < A.d; z += 4)
                {
                    const double fk = static_cast<double>(z + 1) / A.d;
                    const double tEnd = std::min(static_cast<double>(ss.g.apMaxKm) * fk * fk, tTop);
                    const Rgb ref = Exp(OpticalDepthClamped(P, o, d, tEnd, 800) * -1.0);
                    const double rr[3] = {ref.r, ref.g, ref.b};
                    for (int c = 0; c < 3; ++c)
                        if (rr[c] > 0.05) worst = std::max(worst, RelErr(A.At(x, y, z, c), rr[c], 0.05));
                }
            }
        std::printf("[AP LUT] T rel error vs CPU (T>0.05): worst %.3f%%\n", worst * 100.0);
        CHECK_MSG(worst < 0.03, "AP T vs CPU worst rel err %.3f%%", worst * 100.0);
        const uint32_t cx = 16, cy = 16;   // 画面中心付近(=水平方向)
        const float t0[3] = {A.At(cx, cy, 0, 0), A.At(cx, cy, 0, 1), A.At(cx, cy, 0, 2)};
        const float t31[3] = {A.At(cx, cy, 31, 0), A.At(cx, cy, 31, 1), A.At(cx, cy, 31, 2)};
        const float l31[3] = {L.At(cx, cy, 31, 0), L.At(cx, cy, 31, 1), L.At(cx, cy, 31, 2)};
        PrintRgb("AP T (16,16, slice 0)", t0);
        PrintRgb("AP T (16,16, slice 31)", t31);
        PrintRgb("AP L (16,16, slice 31)", l31);
    }

    // ======================================================================
    //  (B) 多重散乱あり: MS LUT・SkyView との差
    // ======================================================================
    const Setup sm = MakeSetup(30.0, 1.0, 1.0f);
    run(sm, false, 2, false);   // 媒質ハッシュ(msFactor を含む)が変わるので T/MS も作り直される
    std::vector<Tex> msl, svl;
    CHECK(readLut(1, msl));
    CHECK(readLut(2, svl));
    {
        const Tex& M = msl[0];
        size_t bad; float mx;
        CHECK_MSG(AllFiniteNonNeg(M, bad, mx), "MS LUT has %zu bad (negative/non-finite) values", bad);
        CHECK_MSG(mx < 1.0f, "MS LUT max %.4f >= 1", static_cast<double>(mx));
        std::printf("[MS LUT] %ux%u  max %.5f (bad %zu)\n", M.w, M.h, static_cast<double>(mx), bad);
        const uint32_t pts[][2] = {{0, 0}, {16, 0}, {31, 0}, {0, 16}, {16, 16}, {31, 16}, {0, 31}, {16, 31}, {31, 31}};
        for (auto& p : pts)
        {
            char lab[64];
            std::snprintf(lab, sizeof(lab), "MS(x=%u,y=%u)", p[0], p[1]);
            const float v[3] = {M.At(p[0], p[1], 0, 0), M.At(p[0], p[1], 0, 1), M.At(p[0], p[1], 0, 2)};
            PrintRgb(lab, v);
        }
        const Tex& S0 = SV0;
        const Tex& S1 = svl[0];
        size_t badS; float mxS;
        CHECK_MSG(AllFiniteNonNeg(S1, badS, mxS), "SkyView(ms=1) has %zu bad values", badS);
        // 天頂付近(v=3)と地平線の少し上(v=53)の平均(u 全体)
        auto avgLum = [](const Tex& t, uint32_t y) {
            double s = 0.0;
            for (uint32_t x = 0; x < t.w; ++x) s += 0.2126 * t.At(x, y, 0, 0) + 0.7152 * t.At(x, y, 0, 1) + 0.0722 * t.At(x, y, 0, 2);
            return s / t.w;
        };
        const double z0 = avgLum(S0, SvY(3 / 107.0)), z1 = avgLum(S1, SvY(3 / 107.0)), h0 = avgLum(S0, SvY(52 / 107.0)), h1 = avgLum(S1, SvY(52 / 107.0));
        std::printf("[SkyView] mean luminance (unit E): zenith ms0 %.5f -> ms1 %.5f (x%.3f) / near-horizon ms0 %.5f -> ms1 %.5f (x%.3f)\n", z0, z1, z1 / z0, h0, h1, h1 / h0);
        CHECK_MSG(z1 > z0 * 1.05, "multi-scattering should brighten the zenith by >=5%% (x%.3f)", z1 / z0);
        CHECK_MSG(h1 > h0 * 1.02, "multi-scattering should brighten near the horizon (x%.3f)", h1 / h0);
        const float zc[3] = {S1.At(SvX(90 / 191.0), SvY(3 / 107.0), 0, 0), S1.At(SvX(90 / 191.0), SvY(3 / 107.0), 0, 1), S1.At(SvX(90 / 191.0), SvY(3 / 107.0), 0, 2)};
        const float hz[3] = {S1.At(SvX(90 / 191.0), SvY(53 / 107.0), 0, 0), S1.At(SvX(90 / 191.0), SvY(53 / 107.0), 0, 1), S1.At(SvX(90 / 191.0), SvY(53 / 107.0), 0, 2)};
        const float sn[3] = {S1.At(SvX(2 / 191.0), SvY(53 / 107.0), 0, 0), S1.At(SvX(2 / 191.0), SvY(53 / 107.0), 0, 1), S1.At(SvX(2 / 191.0), SvY(53 / 107.0), 0, 2)};
        PrintRgb("SkyView ms=1 zenith (u=90,v=3)", zc);
        PrintRgb("SkyView ms=1 horizon (u=90,v=53)", hz);
        PrintRgb("SkyView ms=1 sun-side horizon(u=2,v=53)", sn);
        // 天頂の色相: 青(B>G>R)
        CHECK_MSG(zc[2] > zc[1] && zc[1] > zc[0], "zenith sky should be blue (B>G>R): %.5f %.5f %.5f", static_cast<double>(zc[0]), static_cast<double>(zc[1]), static_cast<double>(zc[2]));
    }

    // ---- 環境キューブ ----
    {
        // 標高 2 m(実運用に近い)・太陽高度 30°・多重散乱あり
        const Setup sg = MakeSetup(30.0, 0.002, 1.0f);
        run(sg, false, 0, true);
        std::vector<Tex> faces;
        CHECK(readCube(0, faces));
        static const char* kFace[6] = {"+X", "-X", "+Y(zenith)", "-Y(ground)", "+Z", "-Z"};
        std::printf("[SkyCube] 64x64x6 (unit E scale: lightE = %.1f lux)\n", static_cast<double>(sg.g.lightE[1]));
        size_t badAll = 0; float mxAll = 0.0f;
        for (int f = 0; f < 6; ++f)
        {
            size_t bad; float mx;
            AllFiniteNonNeg(faces[f], bad, mx);
            badAll += bad; mxAll = std::max(mxAll, mx);
            const float c[3] = {faces[f].At(32, 32, 0, 0), faces[f].At(32, 32, 0, 1), faces[f].At(32, 32, 0, 2)};
            char lab[64];
            std::snprintf(lab, sizeof(lab), "face %s centre (bad=%zu)", kFace[f], bad);
            PrintRgb(lab, c);
        }
        CHECK_MSG(badAll == 0, "sky cube has %zu negative / non-finite values", badAll);
        const float up[3] = {faces[2].At(32, 32, 0, 0), faces[2].At(32, 32, 0, 1), faces[2].At(32, 32, 0, 2)};
        CHECK_MSG(up[2] > up[1] && up[1] > up[0], "cube zenith should be blue (B>G>R): %.1f %.1f %.1f", static_cast<double>(up[0]), static_cast<double>(up[1]), static_cast<double>(up[2]));
        // 天頂の輝度は晴天(数千〜1 万 nit 台)
        const double lumUp = 0.2126 * up[0] + 0.7152 * up[1] + 0.0722 * up[2];
        std::printf("  zenith luminance %.0f nit\n", lumUp);
        CHECK_MSG(lumUp > 1000.0 && lumUp < 30000.0, "zenith luminance %.0f nit out of the clear-sky range", lumUp);
        // 赤チャネルが「ほぼ 0」でないこと(R/B > 0.1 を最低限とする)
        CHECK_MSG(up[0] > 0.1f * up[2], "zenith R channel is nearly zero (R %.1f vs B %.1f)", static_cast<double>(up[0]), static_cast<double>(up[2]));
        // 地面側(-Y)は正(太陽に照らされた地表 = アルベド 0.3)
        const float dn[3] = {faces[3].At(32, 32, 0, 0), faces[3].At(32, 32, 0, 1), faces[3].At(32, 32, 0, 2)};
        CHECK_MSG(dn[0] > 0.0f && dn[1] > 0.0f && dn[2] > 0.0f, "ground-side cube texel must be positive");
        // ---- mip 1..3 の平均が mip0 の平均と ±2% 一致(フィルタが明るさを保つ)----
        {
            auto meanOf = [](const std::vector<Tex>& fs) {
                double sum = 0.0, n = 0.0;
                for (const Tex& t : fs)
                    for (size_t i = 0; i < t.rgba.size(); i += 4) { sum += 0.2126 * t.rgba[i] + 0.7152 * t.rgba[i + 1] + 0.0722 * t.rgba[i + 2]; n += 1.0; }
                return sum / std::max(n, 1.0);
            };
            const double m0 = meanOf(faces);
            for (uint32_t m = 1; m < AtmosphereRenderer::kSkyCubeMips; ++m)
            {
                std::vector<Tex> fm;
                CHECK(readCube(m, fm));
                size_t badM = 0; float mxM = 0.0f;
                for (const Tex& t : fm) { size_t b1; float m1; AllFiniteNonNeg(t, b1, m1); badM += b1; mxM = std::max(mxM, m1); }
                const double mm = meanOf(fm);
                std::printf("[SkyCube mip] mip%u %ux%u mean luminance %.2f nit vs mip0 %.2f (%.3f%%), bad=%zu\n", m, fm[0].w, fm[0].h, mm, m0, (mm / m0 - 1.0) * 100.0, badM);
                CHECK_MSG(fm[0].w == (AtmosphereRenderer::kSkyCubeSize >> m), "mip%u width %u", m, fm[0].w);
                CHECK_MSG(badM == 0, "mip%u has %zu bad values", m, badM);
                CHECK_MSG(std::fabs(mm / m0 - 1.0) < 0.02, "mip%u mean %.2f differs from mip0 %.2f by more than 2%%", m, mm, m0);
            }
        }
        // ---- 診断: 標高 2 m の SkyView(太陽側 u=2 / 反太陽側 u=189)と、太陽側(-Z)/反太陽側(+Z)の面を縦に走査 ----
        std::vector<Tex> sv2;
        CHECK(readLut(2, sv2));
        std::printf("[diag] SkyView at h=2m, sun elev 30deg, ms=1 (unit E): v  | u=2 RGB | u=96 RGB | u=189 RGB\n");
        const double vf[] = {0, 10, 25, 40, 50, 53, 54, 55, 56, 60, 80, 107};
        for (double vfr : vf)
        {
            const uint32_t v = SvY(vfr / 107.0), xa = SvX(2 / 191.0), xb = SvX(96 / 191.0), xc = SvX(189 / 191.0);
            std::printf("   v=%3u | %.5f %.5f %.5f | %.5f %.5f %.5f | %.5f %.5f %.5f\n", v,
                        static_cast<double>(sv2[0].At(xa, v, 0, 0)), static_cast<double>(sv2[0].At(xa, v, 0, 1)), static_cast<double>(sv2[0].At(xa, v, 0, 2)),
                        static_cast<double>(sv2[0].At(xb, v, 0, 0)), static_cast<double>(sv2[0].At(xb, v, 0, 1)), static_cast<double>(sv2[0].At(xb, v, 0, 2)),
                        static_cast<double>(sv2[0].At(xc, v, 0, 0)), static_cast<double>(sv2[0].At(xc, v, 0, 1)), static_cast<double>(sv2[0].At(xc, v, 0, 2)));
        }
        std::printf("[diag] cube face -Z (sun side, az 180) / +Z (anti-sun) column x=32, rows y (top=up): RGB nit\n");
        const uint32_t yy[] = {0, 8, 16, 24, 30, 31, 32, 33, 40, 48, 56, 63};
        for (uint32_t y : yy)
            std::printf("   y=%2u | -Z %.1f %.1f %.1f | +Z %.1f %.1f %.1f\n", y,
                        static_cast<double>(faces[5].At(32, y, 0, 0)), static_cast<double>(faces[5].At(32, y, 0, 1)), static_cast<double>(faces[5].At(32, y, 0, 2)),
                        static_cast<double>(faces[4].At(32, y, 0, 0)), static_cast<double>(faces[4].At(32, y, 0, 1)), static_cast<double>(faces[4].At(32, y, 0, 2)));
    }

    // ---- GPU 時間(参考値。閾値なし)----
    {
        const Setup st = MakeSetup(30.0, 0.002, 1.0f);
        for (uint32_t f = 0; f < 8; ++f)
        {
            atmo.BeginFrameTimers(f % 3);
            ID3D12GraphicsCommandList* cmd = gpu.Begin();
            atmo.Update(cmd, f % 3, st.g, true);
            atmo.RecordSkyCube(cmd, f % 3);
            gpu.Submit();
        }
        atmo.BeginFrameTimers(8 % 3);
        std::printf("[GPU ms] T+MS %.4f / SkyView %.4f / AP %.4f / SkyCube %.4f (forceAll each frame, 1 queue)\n",
                    static_cast<double>(atmo.GetMs(AtmosphereRenderer::ScopeTransMs)), static_cast<double>(atmo.GetMs(AtmosphereRenderer::ScopeSkyView)),
                    static_cast<double>(atmo.GetMs(AtmosphereRenderer::ScopeAp)), static_cast<double>(atmo.GetMs(AtmosphereRenderer::ScopeSkyCube)));
    }

    atmo.Shutdown();
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
#endif
