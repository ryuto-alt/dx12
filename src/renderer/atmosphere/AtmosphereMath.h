#pragma once
// ===========================================================================
// AtmosphereMath — 物理ベース大気(A1)の CPU 参照実装と時刻系(太陽位置・光源の選択)の純関数
// ---------------------------------------------------------------------------
// ヘッダオンリー・標準ライブラリのみ。二つの役目がある。
//   1. アプリが毎フレーム使う「時刻 → 太陽の向き → 地表での太陽色・照度(透過率から)」(ApplicationAtmosphere.cpp)
//   2. GPU の LUT(shaders/atmosphere/)との一致を固定するテストの正解(tests/atmosphere_test.cpp)。
//      透過率は数値積分で、単散乱の放射輝度はレイマーチで独立に計算する(GPU の LUT を使わない)。
// HLSL(shaders/atmosphere/AtmosphereCommon.hlsli)は同じ媒質モデルの写し。片方を直したら両方直すこと。
//
// 座標: 惑星の中心を原点、y を上とした「大気空間」(km)。カメラは (0, R + h, 0)。
// ===========================================================================
#include <algorithm>
#include <array>
#include <cmath>

#include "renderer/atmosphere/AtmosphereSettings.h"

namespace dx12e::atmosphere
{

inline constexpr double kPi = 3.14159265358979323846;

struct Vec3d { double x = 0, y = 0, z = 0; };
inline Vec3d operator+(Vec3d a, Vec3d b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3d operator-(Vec3d a, Vec3d b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3d operator*(Vec3d a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(Vec3d a, Vec3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Len(Vec3d a) { return std::sqrt(Dot(a, a)); }
inline Vec3d Norm(Vec3d a) { const double l = Len(a); return l > 0 ? a * (1.0 / l) : Vec3d{0, 1, 0}; }

// 媒質係数(1/km。RGB)。
struct Rgb { double r = 0, g = 0, b = 0; };
inline Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
inline Rgb operator*(Rgb a, double s) { return {a.r * s, a.g * s, a.b * s}; }
inline Rgb operator*(Rgb a, Rgb b) { return {a.r * b.r, a.g * b.g, a.b * b.b}; }
inline Rgb Exp(Rgb a) { return {std::exp(a.r), std::exp(a.g), std::exp(a.b)}; }

struct Params
{
    double bottomRadius = 6360.0, topRadius = 6460.0;
    Rgb rayleighScattering{0.005802, 0.013558, 0.0331};
    double rayleighExpScale = -1.0 / 8.0;
    Rgb mieScattering{0.003996, 0.003996, 0.003996};
    Rgb mieAbsorption{0.000444, 0.000444, 0.000444};
    double mieExpScale = -1.0 / 1.2;
    double mieG = 0.8;
    Rgb ozoneAbsorption{0.00065, 0.001881, 0.000085};
    double ozoneCenter = 25.0, ozoneHalfWidth = 15.0;
    Rgb groundAlbedo{0.3, 0.3, 0.3};
    double multiScatteringFactor = 1.0;
};

inline Params MakeParams(const AtmosphereSettings& s)
{
    Params p;
    p.bottomRadius = std::max(1.0, static_cast<double>(s.planetRadiusKm));
    p.topRadius = p.bottomRadius + std::max(1.0, static_cast<double>(s.atmosphereHeightKm));
    p.rayleighScattering = {s.rayleighScattering[0], s.rayleighScattering[1], s.rayleighScattering[2]};
    p.rayleighExpScale = -1.0 / std::max(0.01, static_cast<double>(s.rayleighScaleHeightKm));
    p.mieScattering = {s.mieScattering[0], s.mieScattering[1], s.mieScattering[2]};
    p.mieAbsorption = {s.mieAbsorption[0], s.mieAbsorption[1], s.mieAbsorption[2]};
    p.mieExpScale = -1.0 / std::max(0.01, static_cast<double>(s.mieScaleHeightKm));
    p.mieG = std::clamp(static_cast<double>(s.mieG), -0.99, 0.99);
    p.ozoneAbsorption = {s.ozoneAbsorption[0], s.ozoneAbsorption[1], s.ozoneAbsorption[2]};
    p.ozoneCenter = s.ozoneCenterKm;
    p.ozoneHalfWidth = std::max(0.0, static_cast<double>(s.ozoneWidthKm));
    p.groundAlbedo = {s.groundAlbedo[0], s.groundAlbedo[1], s.groundAlbedo[2]};
    p.multiScatteringFactor = std::max(0.0, static_cast<double>(s.multiScatteringFactor));
    return p;
}

// 高度 h[km] での消散係数(散乱 + 吸収)。
inline Rgb ExtinctionAt(const Params& p, double h)
{
    const double dRay = std::exp(p.rayleighExpScale * h);
    const double dMie = std::exp(p.mieExpScale * h);
    double dOzo = 0.0;
    if (p.ozoneHalfWidth > 0.0) dOzo = std::clamp(1.0 - std::fabs(h - p.ozoneCenter) / p.ozoneHalfWidth, 0.0, 1.0);
    return p.rayleighScattering * dRay + (p.mieScattering + p.mieAbsorption) * dMie + p.ozoneAbsorption * dOzo;
}
inline Rgb ScatteringAt(const Params& p, double h, Rgb& rayOut, Rgb& mieOut)
{
    rayOut = p.rayleighScattering * std::exp(p.rayleighExpScale * h);
    mieOut = p.mieScattering * std::exp(p.mieExpScale * h);
    return rayOut + mieOut;
}

// 原点中心・半径 r の球と、o から d(単位)の半直線の最初の交点までの距離。無ければ -1。
inline double RaySphereNearest(Vec3d o, Vec3d d, double r)
{
    const double b = Dot(o, d);
    const double c = Dot(o, o) - r * r;
    if (c >= 0.0 && b >= 0.0) return -1.0;   // 球の外(または表面)から遠ざかる向き。★表面ちょうど(c=0)で上向きを「地面に当たる」と数えない
    const double disc = b * b - c;
    if (disc < 0.0) return -1.0;
    const double s = std::sqrt(disc);
    const double t0 = -b - s, t1 = -b + s;
    if (t0 >= 0.0) return t0;
    if (t1 >= 0.0) return t1;
    return -1.0;
}

// 光学的厚み(RGB)を o から d 方向へ tMax まで積分(中点則。steps を大きくすると真値に近づく)。
inline Rgb OpticalDepth(const Params& p, Vec3d o, Vec3d d, double tMax, int steps)
{
    Rgb sum{};
    const double dt = tMax / steps;
    for (int i = 0; i < steps; ++i)
    {
        const double t = (i + 0.5) * dt;
        const Vec3d pos = o + d * t;
        sum = sum + ExtinctionAt(p, Len(pos) - p.bottomRadius) * dt;
    }
    return sum;
}

// 高度 h[km] から天頂角の余弦 mu の向きへ、大気の外まで(地面に当たるなら 0)の透過率。
inline Rgb TransmittanceToSpace(const Params& p, double h, double mu, int steps = 512)
{
    const Vec3d o{0.0, p.bottomRadius + h, 0.0};
    const Vec3d d{std::sqrt(std::max(0.0, 1.0 - mu * mu)), mu, 0.0};
    if (RaySphereNearest(o, d, p.bottomRadius) >= 0.0) return {0, 0, 0};
    const double tTop = RaySphereNearest(o, d, p.topRadius);
    if (tTop <= 0.0) return {1, 1, 1};
    return Exp(OpticalDepth(p, o, d, tTop, steps) * -1.0);
}

// ---- 位相関数 ----
inline double RayleighPhase(double cosTheta) { return 3.0 / (16.0 * kPi) * (1.0 + cosTheta * cosTheta); }
inline double HgPhase(double g, double cosTheta)
{
    const double k = 1.0 / (4.0 * kPi);
    const double d = 1.0 + g * g - 2.0 * g * cosTheta;
    return k * (1.0 - g * g) / (d * std::sqrt(d));
}

// 単散乱の放射輝度(太陽の照度 = 1 あたり、RGB)。o から d 方向。地面に当たれば地表の拡散反射(ランバート、単一のバウンス)を足す。
// 多重散乱は含まない(GPU 側は multiScatteringFactor = 0 のときこれと比べる)。steps は視線方向の分割数(太陽側の透過率は数値積分)。
inline Rgb SingleScatteringRadiance(const Params& p, Vec3d o, Vec3d d, Vec3d sunDir, int steps, int sunSteps, bool includeGround)
{
    const double tBottom = RaySphereNearest(o, d, p.bottomRadius);
    const double tTop = RaySphereNearest(o, d, p.topRadius);
    double tMax = 0.0;
    if (tBottom < 0.0) { if (tTop < 0.0) return {0, 0, 0}; tMax = tTop; }
    else { tMax = (tTop > 0.0) ? std::min(tTop, tBottom) : tBottom; }

    const double cosTheta = Dot(sunDir, d);
    const double phaseR = RayleighPhase(cosTheta);
    const double phaseM = HgPhase(p.mieG, cosTheta);

    Rgb L{}, throughput{1, 1, 1};
    const double dt = tMax / steps;
    for (int i = 0; i < steps; ++i)
    {
        const double t = (i + 0.5) * dt;
        const Vec3d pos = o + d * t;
        const double h = Len(pos) - p.bottomRadius;
        Rgb ray, mie;
        const Rgb sc = ScatteringAt(p, h, ray, mie);
        const Rgb ext = ExtinctionAt(p, h);
        const Rgb stepT = Exp(ext * -dt);

        // 太陽側の透過率(地面の影になる点は 0)
        const Vec3d up = Norm(pos);
        const double muS = Dot(up, sunDir);
        Rgb tSun{0, 0, 0};
        {
            // 影判定は「地面に当たるか」(RaySphereNearest は o が球の外なら正しい)
            const Vec3d origin = pos + up * 0.01;
            if (RaySphereNearest(origin, sunDir, p.bottomRadius) < 0.0)
            {
                const double tt = RaySphereNearest(pos, sunDir, p.topRadius);
                tSun = (tt > 0.0) ? Exp(OpticalDepth(p, pos, sunDir, tt, sunSteps) * -1.0) : Rgb{1, 1, 1};
            }
        }
        (void)muS;
        const Rgb S = tSun * (ray * phaseR + mie * phaseM);
        // 区間内で消散を考慮した積分 (1 - exp(-ext dt)) / ext
        const Rgb Sint{ (ext.r > 1e-12) ? S.r * (1.0 - stepT.r) / ext.r : S.r * dt,
                        (ext.g > 1e-12) ? S.g * (1.0 - stepT.g) / ext.g : S.g * dt,
                        (ext.b > 1e-12) ? S.b * (1.0 - stepT.b) / ext.b : S.b * dt };
        L = L + throughput * Sint;
        throughput = throughput * stepT;
    }

    if (includeGround && tBottom > 0.0 && tMax == tBottom)
    {
        const Vec3d pos = o + d * tBottom;
        const Vec3d up = Norm(pos);
        const double ndl = std::clamp(Dot(up, sunDir), 0.0, 1.0);
        const Vec3d origin = pos + up * 0.01;
        Rgb tSun{0, 0, 0};
        if (ndl > 0.0 && RaySphereNearest(origin, sunDir, p.bottomRadius) < 0.0)
        {
            const double tt = RaySphereNearest(pos, sunDir, p.topRadius);
            tSun = (tt > 0.0) ? Exp(OpticalDepth(p, pos, sunDir, tt, sunSteps) * -1.0) : Rgb{1, 1, 1};
        }
        L = L + throughput * tSun * p.groundAlbedo * (ndl / kPi);
    }
    return L;
}

// =========================================================================
//  時刻系: 太陽・月の向きと、地表での光の色・照度
// =========================================================================

struct SunAngles
{
    double elevationDeg = 0.0;   // 地平線 = 0、天頂 = 90。負 = 地平線の下
    double azimuthDeg   = 0.0;   // 北 = 0、東 = 90(時計回り)
    double declinationDeg = 0.0; // 太陽の赤緯
};

// 現地太陽時 hour(0..24)・緯度・年内通日から太陽の高度・方位。
// 赤緯 = 23.45° · sin(360°/365 · (284 + N))(Cooper の式)。時角 H = (hour - 12) · 15°。大気差・均時差は無視する簡易式。
inline SunAngles ComputeSunAngles(double hour, double latitudeDeg, int dayOfYear)
{
    const double rad = kPi / 180.0;
    const double n = static_cast<double>(std::clamp(dayOfYear, 1, 366));
    const double decl = 23.45 * std::sin(rad * 360.0 / 365.0 * (284.0 + n));
    const double H = (hour - 12.0) * 15.0 * rad;
    const double phi = std::clamp(latitudeDeg, -90.0, 90.0) * rad;
    const double dl = decl * rad;
    // 局所座標(東 E / 北 N / 上 U)での太陽の向き
    const double E = -std::cos(dl) * std::sin(H);
    const double N = std::sin(dl) * std::cos(phi) - std::cos(dl) * std::sin(phi) * std::cos(H);
    const double U = std::sin(dl) * std::sin(phi) + std::cos(dl) * std::cos(phi) * std::cos(H);
    SunAngles a;
    a.elevationDeg = std::asin(std::clamp(U, -1.0, 1.0)) / rad;
    double az = std::atan2(E, N) / rad;
    if (az < 0.0) az += 360.0;
    a.azimuthDeg = az;
    a.declinationDeg = decl;
    return a;
}

// 高度・方位(北から時計回り)→ ワールドの「光源へ向かう」単位ベクトル。方位はエンジン規約(+Z = 0°、+X = 90°)+ northYaw。
inline Vec3d DirectionFromAngles(double elevationDeg, double azimuthDeg, double northYawDeg)
{
    const double rad = kPi / 180.0;
    const double el = elevationDeg * rad;
    const double az = (azimuthDeg + northYawDeg) * rad;
    const double ce = std::cos(el);
    return {ce * std::sin(az), std::sin(el), ce * std::cos(az)};
}

// 光源(太陽 or 月)の選択と地表での色・照度。
struct SkyLight
{
    Vec3d  toLight{0, 1, 0};     // 光源へ向かう単位ベクトル(ワールド)
    double sunElevationDeg = 90.0;
    bool   isMoon = false;       // true = 夜(月が光源)
    double illuminanceTopLux = 0.0;   // 大気上端の光源照度 [lux](太陽 or 月。薄明の重みつき)
    Rgb    colorTop{1, 1, 1};         // 上端での色(照度に掛ける RGB。太陽 = sunTint / 月 = moonTint)
    Rgb    illuminanceGround{0, 0, 0};// 地表(カメラ高度)の水平でない面の垂直入射照度 RGB [lux](透過率込み)
};

// 太陽が地平線の下 kMoonFadeStartDeg から月へ切り替え(kMoonFadeEndDeg で完全に月)。その間は太陽の照度を絞る。
inline constexpr double kMoonFadeStartDeg = -12.0;
inline constexpr double kMoonFadeEndDeg = -18.0;

// sunDir: 太陽の向き(ワールド)、camAltKm: カメラの標高[km]。
inline SkyLight ComputeSkyLight(const AtmosphereSettings& s, const Vec3d& sunDir, double camAltKm)
{
    SkyLight L;
    const Params p = MakeParams(s);
    L.sunElevationDeg = std::asin(std::clamp(sunDir.y, -1.0, 1.0)) * 180.0 / kPi;
    // 月への切替の重み(0 = 太陽 / 1 = 月)
    double moonW = 0.0;
    if (s.drawMoon)
    {
        const double t = (kMoonFadeStartDeg - L.sunElevationDeg) / (kMoonFadeStartDeg - kMoonFadeEndDeg);
        moonW = std::clamp(t, 0.0, 1.0);
        moonW = moonW * moonW * (3.0 - 2.0 * moonW);
    }
    L.isMoon = moonW >= 0.5;
    L.toLight = L.isMoon ? Vec3d{-sunDir.x, -sunDir.y, -sunDir.z} : sunDir;
    // 太陽は (1-w)^4 で急に絞り、月は w に比例して足す(切替の点 w=0.5 で空の明るさが跳ばないように)
    const double sunFade = (1.0 - moonW) * (1.0 - moonW) * (1.0 - moonW) * (1.0 - moonW);
    const double sunAmount = sunFade * s.sunIlluminance;
    const double moonAmount = moonW * s.moonIlluminance;
    L.illuminanceTopLux = L.isMoon ? moonAmount : sunAmount;
    L.colorTop = L.isMoon ? Rgb{s.moonTint[0], s.moonTint[1], s.moonTint[2]} : Rgb{s.sunTint[0], s.sunTint[1], s.sunTint[2]};
    const double mu = L.toLight.y;   // 大気空間の上 = ワールド +y
    const Rgb T = TransmittanceToSpace(p, std::max(camAltKm, 0.0), mu, 256);
    L.illuminanceGround = {L.illuminanceTopLux * L.colorTop.r * T.r, L.illuminanceTopLux * L.colorTop.g * T.g,
                           L.illuminanceTopLux * L.colorTop.b * T.b};
    return L;
}

inline double RgbLuminance(Rgb c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }

// 時刻 → 太陽の向き(時刻モード)。
inline Vec3d SunDirectionFromTime(const AtmosphereSettings& s)
{
    const SunAngles a = ComputeSunAngles(s.timeOfDay, s.latitudeDeg, s.dayOfYear);
    return DirectionFromAngles(a.elevationDeg, a.azimuthDeg, s.northYawDeg);
}

// 時間の進め(0..24 に折り返す)。
inline float AdvanceTimeOfDay(float hour, float hoursPerSecond, float dt)
{
    float h = hour + hoursPerSecond * dt;
    h = std::fmod(h, 24.0f);
    if (h < 0.0f) h += 24.0f;
    return h;
}

} // namespace dx12e::atmosphere
