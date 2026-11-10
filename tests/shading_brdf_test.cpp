// 拡張シェーディング（shaders/forward/BrdfExt.hlsli + Lighting.hlsli の UnoShadePunctualCtx）の CPU 写しの数値テスト。GPU 不要。
//
// 守っているもの:
//   1. 分布の正規化: ∫ D(h)(n·h) dω = 1（等方 GGX / 異方 GGX / Charlie）
//   2. 相反性: 基底のスペキュラ（等方・異方）と Charlie のシーンの葉は f(V,L) = f(L,V)
//   3. エネルギー上限（白い炉）: 方向アルベド ∫ f·cos dω ≤ 1
//        白い誘電体 / 白い金属（補償あり・なし）/ クリアコート付き / シーン付き / 異方性
//      補償ありの白い金属は「補償なしより明るく、1 に近い」
//   4. 量子化の往復（PackShadingExt → シェーダの UnoApplyShadingExt と同じ復元）
// ★シェーダの式を変えたらここも同じに直すこと（ここはフォワード / PT が同じ式であることの外部の物差し）。
#include "renderer/ShadingExt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

#define CHECK_MSG(cond, ...)                                                   \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) { std::printf("[FAIL] %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_failures; } \
    } while (0)

constexpr double kPi = 3.14159265358979323846;

struct V3 { double x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Norm(V3 a) { const double l = std::sqrt(Dot(a, a)); return a * (1.0 / l); }
double Sat(double v) { return std::clamp(v, 0.0, 1.0); }
double Lerp(double a, double b, double t) { return a + (b - a) * t; }

// ---- BrdfExt.hlsli の写し ------------------------------------------------------------
double D_GGX(double NoH, double r)
{
    const double a = r * r, a2 = a * a;
    double d = NoH * NoH * (a2 - 1.0) + 1.0;
    d = kPi * d * d;
    return a2 / std::max(d, 1e-7);
}
double G1(double NoX, double r) { const double k = (r + 1.0) * (r + 1.0) / 8.0; return NoX / (NoX * (1.0 - k) + k); }
double G1_IBL(double NoX, double r) { const double k = r * r / 2.0; return NoX / (NoX * (1.0 - k) + k); }
double Fres(double F0, double F90, double c) { return F0 + (F90 - F0) * std::pow(Sat(1.0 - c), 5.0); }
double D_Aniso(double NoH, double ToH, double BoH, double at, double ab)
{
    const double a2 = at * ab;
    const V3 v{ab * ToH, at * BoH, a2 * NoH};
    const double v2 = Dot(v, v);
    const double w2 = a2 / std::max(v2, 1e-20);
    return a2 * w2 * w2 / kPi;
}
double G1Aniso(double NoX, double ToX, double BoX, double at, double ab)
{
    const double s2 = std::max(1.0 - NoX * NoX, 0.0);
    const double a2 = (s2 > 1e-6) ? (at * at * ToX * ToX + ab * ab * BoX * BoX) / s2 : at * ab;
    return G1(NoX, std::sqrt(std::sqrt(std::max(a2, 1e-12))));
}
double D_Charlie(double sr, double NoH)
{
    const double r = std::max(sr, 1e-6), aG = r * r, invR = 1.0 / aG;
    const double s2 = std::max(1.0 - NoH * NoH, 0.0);
    return (2.0 + invR) * std::pow(s2, invR * 0.5) / (2.0 * kPi);
}
double SheenL(double x, double aG)
{
    const double t = (1.0 - aG) * (1.0 - aG);
    const double a = Lerp(21.5473, 25.3245, t), b = Lerp(3.82987, 3.32435, t), c = Lerp(0.19823, 0.16801, t);
    const double d = Lerp(-1.97760, -1.27393, t), e = Lerp(-4.32054, -4.85967, t);
    return a / (1.0 + b * std::pow(std::max(x, 1e-6), c)) + d * x + e;
}
double SheenLambda(double c, double aG)
{
    c = std::fabs(c);
    return (c < 0.5) ? std::exp(SheenL(c, aG)) : std::exp(2.0 * SheenL(0.5, aG) - SheenL(1.0 - c, aG));
}
double V_Charlie(double NoL, double NoV, double sr)
{
    const double r = std::max(sr, 1e-6), aG = r * r;
    const double den = (1.0 + SheenLambda(NoV, aG) + SheenLambda(NoL, aG)) * (4.0 * NoV * NoL);
    return Sat(1.0 / std::max(den, 1e-7));
}

// 半球の数値積分（L を cosθ・φ の格子で。dω = d(cosθ) dφ）
template <class F>
double IntegrateHemisphere(F f, int n = 512)
{
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
        {
            const double ct = (i + 0.5) / n, st = std::sqrt(1.0 - ct * ct), ph = 2.0 * kPi * (j + 0.5) / n;
            sum += f(V3{st * std::cos(ph), st * std::sin(ph), ct});
        }
    return sum * (2.0 * kPi) / (static_cast<double>(n) * n);
}

double Radical(unsigned bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return bits * 2.3283064365386963e-10;
}
// 等方 GGX の半ベクトル（局所。z = 法線）
V3 SampleGgxH(double u1, double u2, double r)
{
    const double a = r * r, a2 = a * a;
    const double c = std::sqrt((1.0 - u1) / (1.0 + (a2 - 1.0) * u1)), sn = std::sqrt(std::max(0.0, 1.0 - c * c));
    const double ph = 2.0 * kPi * u2;
    return {sn * std::cos(ph), sn * std::sin(ph), c};
}
V3 SampleAnisoH(double u1, double u2, double at, double ab)
{
    const double rr = std::sqrt(u1 / std::max(1.0 - u1, 1e-12)), ph = 2.0 * kPi * u2;
    return Norm(V3{-rr * at * std::cos(ph), -rr * ab * std::sin(ph), 1.0});
}
V3 Reflect(V3 V, V3 H) { return H * (2.0 * Dot(V, H)) - V; }

// split-sum LUT の E = A + B（IntegrateBRDF.hlsl と同じ: GGX の重要度サンプル・IBL の G・F0 = 1）
double E_Lut(double NoV, double r)
{
    const V3 V{std::sqrt(1.0 - NoV * NoV), 0.0, NoV};
    const unsigned n = 4096;
    double sum = 0.0;
    for (unsigned i = 0; i < n; ++i)
    {
        const V3 H = SampleGgxH((i + 0.0) / n, Radical(i), r);
        const V3 L = Reflect(V, H);
        if (L.z <= 0.0) continue;
        const double VoH = std::max(Dot(V, H), 0.0);
        sum += G1_IBL(NoV, r) * G1_IBL(L.z, r) * VoH / std::max(H.z * NoV, 1e-5);
    }
    return sum / n;
}
// LUT .a: 直接光の G（k = (r+1)²/8）での F0 = 1 の方向アルベド
double E_Direct(double NoV, double r)
{
    const V3 V{std::sqrt(1.0 - NoV * NoV), 0.0, NoV};
    const unsigned n = 4096;
    double sum = 0.0;
    for (unsigned i = 0; i < n; ++i)
    {
        const V3 H = SampleGgxH((i + 0.0) / n, Radical(i), r);
        const V3 L = Reflect(V, H);
        if (L.z <= 0.0) continue;
        const double VoH = std::max(Dot(V, H), 0.0);
        sum += G1(NoV, r) * G1(L.z, r) * VoH / std::max(H.z * NoV, 1e-5);
    }
    return sum / n;
}
double E_Sheen(double NoV, double sr)
{
    const V3 V{std::sqrt(1.0 - NoV * NoV), 0.0, NoV};
    return IntegrateHemisphere([&](V3 L) {
        const V3 H = Norm(V + L);
        return D_Charlie(sr, H.z) * V_Charlie(L.z, NoV, sr) * L.z;
    }, 256);
}

struct Mat
{
    double albedo = 1.0, metallic = 0.0, roughness = 0.5;
    double coat = 0.0, coatR = 0.04;
    double aniso = 0.0;           // 強さ（接線 = +X）
    double sheen = 0.0, sheenR = 0.5;
    bool   ms = false;
};

// UnoShadePunctualCtx(…, radiance = 1) の写し（N = +Z。コートの法線も +Z）。戻り値 = f·cos
double EvalCos(const Mat& m, V3 V, V3 L, double msComp, double coatMs, double sheenScaleV, double coatAttenV)
{
    const V3 N{0, 0, 1};
    const V3 H = Norm(V + L);
    const double NdotV = std::max(Dot(N, V), 0.001), NdotL = std::max(Dot(N, L), 0.0);
    const double NoH = std::max(Dot(N, H), 0.0), VoH = std::max(Dot(H, V), 0.0);
    const double F0 = Lerp(0.04, m.albedo, m.metallic);
    const double F = Fres(F0, 1.0, VoH);
    double spec;
    if (m.aniso != 0.0)
    {
        const double a = m.roughness * m.roughness, s = std::fabs(m.aniso);
        const double at = Lerp(a, 1.0, s * s), ab = a;
        spec = D_Aniso(NoH, H.x, H.y, at, ab) * G1Aniso(std::max(Dot(N, V), 0.0), V.x, V.y, at, ab) * G1Aniso(NdotL, L.x, L.y, at, ab) * F
             / (4.0 * NdotV * NdotL + 0.0001);
    }
    else
        spec = D_GGX(NoH, m.roughness) * G1(std::max(Dot(N, V), 0.0), m.roughness) * G1(NdotL, m.roughness) * F
             / (4.0 * NdotV * NdotL + 0.0001);
    const double kD = (1.0 - F) * (1.0 - m.metallic);
    double col = kD * m.albedo / kPi + spec * msComp;
    if (m.sheen > 0.0)
        col = col * sheenScaleV + m.sheen * D_Charlie(m.sheenR, NoH) * V_Charlie(NdotL, NdotV, m.sheenR);
    double o = col * NdotL;
    if (m.coat > 0.0)
    {
        const double Fc = 0.04 + 0.96 * std::pow(Sat(1.0 - VoH), 5.0);
        const double specC = D_GGX(NoH, m.coatR) * G1(NdotV, m.coatR) * G1(NdotL, m.coatR) * Fc / (4.0 * NdotV * NdotL + 0.0001) * coatMs;
        o = o * coatAttenV + m.coat * specC * NdotL;
    }
    return o;
}

// 白い炉: 視線 NoV の方向アルベド（UnoShadeLighting の文脈の作り方と同じ）。
//   積分は「拡散（コサイン）/ 基底の鏡面（GGX・異方 GGX）/ コート（GGX）」の混合の重要度サンプル（尖った葉でも外さない）。
double Albedo(const Mat& m, double NoV)
{
    const V3 V{std::sqrt(1.0 - NoV * NoV), 0.0, NoV};
    const double r = std::max(m.roughness, 0.04);
    const double F0 = Lerp(0.04, m.albedo, m.metallic);
    const double a0 = r * r;
    // 異方性: 視線の方位へ投影した α_v（G1Aniso と同じ）で LUT を引く。この試験の V は接線（+X）方向なので α_v = αt
    // 異方性: BrdfExt.hlsli の UnoBxAnisoEqRoughness / UnoBxAnisoMsNoV と同じ座標で E を引く
    const bool an = (m.aniso != 0.0);
    const double rMs = an ? std::sqrt(std::sqrt(Lerp(a0, 1.0, m.aniso * m.aniso) * a0)) : r;
    const double atv = Lerp(a0, 1.0, m.aniso * m.aniso);
    const double nvMs = an ? Lerp(NoV, 0.5, 1.0 - a0 / atv) : NoV;   // 異方なほど視線に依らない E（等方の極限では元の N·V）
    const double msComp = m.ms ? 1.0 + F0 * (1.0 / std::max(E_Direct(nvMs, rMs), 0.05) - 1.0) : 1.0;
    const double cr = std::max(m.coatR, 0.04);
    const double coatMs = (m.ms && m.coat > 0.0) ? 1.0 + 0.04 * (1.0 / std::max(E_Direct(NoV, cr), 0.05) - 1.0) : 1.0;
    const double sr = std::clamp(m.sheenR, 0.07, 1.0);
    const double sheenScaleV = (m.sheen > 0.0) ? 1.0 - m.sheen * E_Sheen(NoV, sr) : 1.0;
    const double coatAttenV = 1.0 - m.coat * (0.04 + 0.96 * std::pow(1.0 - NoV, 5.0));
    Mat mm = m; mm.roughness = r; mm.sheenR = sr; mm.coatR = cr;
    const double a = r * r, s2 = m.aniso * m.aniso;
    const double at = (m.aniso != 0.0) ? Lerp(a, 1.0, s2) : a, ab = a;
    const int lobes = (m.coat > 0.0) ? 3 : 2;
    const unsigned n = 1u << 18;
    double sum = 0.0;
    for (unsigned i = 0; i < n; ++i)
    {
        const double u1 = (i + 0.5) / n, u2 = Radical(i);
        V3 L;
        const int lobe = static_cast<int>(i % lobes);
        if (lobe == 0)
        {
            const double ph = 2.0 * kPi * u2, sr2 = std::sqrt(u1);
            L = V3{sr2 * std::cos(ph), sr2 * std::sin(ph), std::sqrt(1.0 - u1)};
        }
        else if (lobe == 1)
            L = Reflect(V, (m.aniso != 0.0) ? SampleAnisoH(u1, u2, at, ab) : SampleGgxH(u1, u2, r));
        else
            L = Reflect(V, SampleGgxH(u1, u2, cr));
        if (L.z <= 0.0) continue;
        const V3 H = Norm(V + L);
        const double VoH = std::max(Dot(V, H), 1e-6);
        const double Dbase = (m.aniso != 0.0) ? D_Aniso(H.z, H.x, H.y, at, ab) : D_GGX(H.z, r);
        double pdf = L.z / kPi + Dbase * H.z / (4.0 * VoH);
        if (lobes == 3) pdf += D_GGX(H.z, cr) * H.z / (4.0 * VoH);
        pdf /= lobes;
        sum += EvalCos(mm, V, L, msComp, coatMs, sheenScaleV, coatAttenV) / pdf;
    }
    return sum / n;
}

// θ（一様）× φ の格子で ∫ f(H) dω（分布の正規化用。尖った分布でも θ を細かく刻めば足りる）
template <class F>
double IntegrateTheta(F f, int nt, int np)
{
    double sum = 0.0;
    const double dt = 0.5 * kPi / nt, dp = 2.0 * kPi / np;
    for (int i = 0; i < nt; ++i)
    {
        const double th = (i + 0.5) * dt, st = std::sin(th), ct = std::cos(th);
        for (int j = 0; j < np; ++j)
        {
            const double ph = (j + 0.5) * dp;
            sum += f(V3{st * std::cos(ph), st * std::sin(ph), ct}) * st;
        }
    }
    return sum * dt * dp;
}

void Test_Normalization()
{
    for (double r : {0.2, 0.5, 0.9})
    {
        const double iso = IntegrateTheta([&](V3 H) { return D_GGX(H.z, r) * H.z; }, 20000, 1);
        CHECK_MSG(std::fabs(iso - 1.0) < 0.03, "等方 GGX の正規化 r=%.1f: %.4f", r, iso);
        const double a = r * r;
        const double an = IntegrateTheta([&](V3 H) { return D_Aniso(H.z, H.x, H.y, Lerp(a, 1.0, 0.64), a) * H.z; }, 8000, 2048);
        CHECK_MSG(std::fabs(an - 1.0) < 0.03, "異方 GGX の正規化 r=%.1f: %.4f", r, an);
        const double ch = IntegrateTheta([&](V3 H) { return D_Charlie(r, H.z) * H.z; }, 20000, 1);
        CHECK_MSG(std::fabs(ch - 1.0) < 0.03, "Charlie の正規化 r=%.1f: %.4f", r, ch);
    }
}

void Test_Reciprocity()
{
    const V3 V = Norm(V3{0.3, -0.5, 0.8}), L = Norm(V3{-0.6, 0.2, 0.7});
    for (double aniso : {0.0, 0.8})
    {
        Mat m; m.albedo = 0.0; m.metallic = 1.0; m.roughness = 0.4; m.aniso = aniso;   // 鏡面だけ（F は VoH で対称）
        const double a = EvalCos(m, V, L, 1.0, 1.0, 1.0, 1.0) / L.z;
        const double b = EvalCos(m, L, V, 1.0, 1.0, 1.0, 1.0) / V.z;
        CHECK_MSG(std::fabs(a - b) < 1e-3 * std::max(a, 1e-6) + 1e-6, "相反性 aniso=%.1f: %.6f vs %.6f", aniso, a, b);
    }
    const double s1 = D_Charlie(0.4, Norm(V + L).z) * V_Charlie(L.z, V.z, 0.4);
    const double s2 = D_Charlie(0.4, Norm(L + V).z) * V_Charlie(V.z, L.z, 0.4);
    CHECK_MSG(std::fabs(s1 - s2) < 1e-9, "シーンの相反性: %.6f vs %.6f", s1, s2);
}

void Test_Furnace()
{
    std::printf("白い炉（方向アルベド。NoV = 0.2 / 0.5 / 0.9）\n");
    for (double r : {0.1, 0.3, 0.5, 0.7, 1.0})
    {
        for (double nov : {0.2, 0.5, 0.9})
        {
            Mat metal; metal.metallic = 1.0; metal.roughness = r;
            const double m0 = Albedo(metal, nov);
            metal.ms = true;
            const double m1 = Albedo(metal, nov);
            Mat diel; diel.roughness = r;
            const double d0 = Albedo(diel, nov);
            diel.ms = true;
            const double d1 = Albedo(diel, nov);
            Mat coat = diel; coat.coat = 1.0; coat.coatR = 0.05;
            const double c1 = Albedo(coat, nov);
            Mat sheen = diel; sheen.sheen = 1.0; sheen.sheenR = 0.5;
            const double s1 = Albedo(sheen, nov);
            Mat an = metal; an.aniso = 0.8;
            const double a1 = Albedo(an, nov);
            std::printf("  r=%.1f NoV=%.1f  金属 %.3f → 補償 %.3f / 誘電体 %.3f → 補償 %.3f / +コート %.3f / +シーン %.3f / 異方金属(補償) %.3f\n",
                        r, nov, m0, m1, d0, d1, c1, s1, a1);
            CHECK_MSG(m1 >= m0 - 1e-4, "補償で暗くならない r=%.1f", r);
            CHECK_MSG(std::fabs(m1 - 1.0) <= 0.02, "補償ありの白い金属 ≈ 1 r=%.1f NoV=%.1f: %.3f", r, nov, m1);
            // 白い誘電体は元の BRDF（kD = 1 - F(V·H)）がグレージングで 1 をわずかに超える（従来から）。補償が足す分だけ見る
            CHECK_MSG(d1 - d0 <= 0.01, "誘電体の補償の上乗せ ≤ 0.01 r=%.1f NoV=%.1f: %.3f → %.3f", r, nov, d0, d1);
            CHECK_MSG(c1 <= std::max(d1, 1.0) + 0.01, "クリアコートで増えない r=%.1f NoV=%.1f: %.3f", r, nov, c1);
            CHECK_MSG(s1 <= std::max(d1, 1.0) + 0.005, "シーンで増えない r=%.1f NoV=%.1f: %.3f", r, nov, s1);
            CHECK_MSG(a1 <= 1.25 && a1 >= 0.9, "補償ありの異方性の金属 0.9..1.25 r=%.1f NoV=%.1f: %.3f", r, nov, a1);
        }
    }
    // 粗い金属は補償で明確に持ち上がる（単一散乱の損失は r=1 で 2〜3 割）
    Mat metal; metal.metallic = 1.0; metal.roughness = 1.0;
    const double m0 = Albedo(metal, 0.5);
    metal.ms = true;
    const double m1 = Albedo(metal, 0.5);
    CHECK_MSG(m1 - m0 > 0.1, "r=1 の金属は補償で 0.1 以上明るくなる: %.3f → %.3f", m0, m1);
}

void Test_Pack()
{
    ShadingExt e;
    CHECK_MSG(PackShadingExt(e) == PackedShadingExt{}, "既定は 8 DWORD とも 0");
    e.clearCoat = 0.75f; e.clearCoatRoughness = 0.2f; e.anisotropy = -0.5f; e.anisotropyRotation = 1.25f;
    e.sheenColor[0] = 1.0f; e.sheenColor[1] = 0.5f; e.sheenRoughness = 0.3f; e.ior = 1.45f;
    const DecodedShadingExt d = UnpackShadingExt(PackShadingExt(e));
    CHECK_MSG(std::fabs(d.clearCoat - 0.75f) < 1e-4f && std::fabs(d.clearCoatRoughness - 0.2f) < 1e-4f, "コートの往復");
    CHECK_MSG(std::fabs(d.anisotropy + 0.5f) < 1e-4f && std::fabs(d.anisotropyRotation - 1.25f) < 1e-3f, "異方性の往復");
    CHECK_MSG(std::fabs(d.sheenColor[1] - 0.5f) < 3e-3f && std::fabs(d.sheenRoughness - 0.3f) < 3e-3f, "シーンの往復");
    CHECK_MSG(std::fabs(d.f0[0] - IorToF0(1.45f)) < 1e-6f && d.f90 == 1.0f, "IOR → F0");
}
} // namespace

int main()
{
    Test_Normalization();
    Test_Reciprocity();
    Test_Furnace();
    Test_Pack();
    std::printf("ShadingBrdfTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
