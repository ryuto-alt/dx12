#pragma once
// ===========================================================================
// WaterMath — 水面 W1 の CPU 参照実装（純関数・GPU 非依存。テストと GPU の一致確認の基準）。
// ---------------------------------------------------------------------------
//   ・波の合成（決定論。WaterBody の少数パラメータ + 種から最大 12 本のゲルストナー波を作る）と、その評価（変位 / 法線 / ヤコビアン）
//   ・光学: 誘電体のフレネル（偏光平均）/ スネルの屈折 / Beer-Lambert の透過率
//   ・形: 多角形の解析 / 点の内外判定 / ワールド AABB
//   ・プリセット（湖 / 海 / 川 / プール / 沼）
//   ・ディテール法線テクスチャの手続き生成（256²・タイル可能・外部アセットなし。RGBA8: R,G=傾き B=泡ノイズ A=コースティクス）
// ★shaders/water/WaterCommon.hlsli の式と一致させること（tests/water_gpu_test.cpp が GPU と突き合わせる）。
// ===========================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ecs/Components.h"

namespace dx12e::water
{

inline constexpr int   kMaxWaves = 12;
inline constexpr float kGravity = 9.81f;
inline constexpr int   kMaxPolyPoints = 16;
inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr int   kDetailTexSize = 256;

// GPU に送る 1 本の波（float4 × 2 = 32 バイト）
struct Wave
{
    float dirX, dirZ, k, omega;   // 進行方向（単位ベクトル）/ 波数 2π/λ / 角周波数
    float amp, steep, phase, pad; // 振幅 A / 急峻さ Q（水平変位 = Q·A）/ 初期位相 / 予約
};
static_assert(sizeof(Wave) == 32, "Wave は GPU の float4 x 2 と一致させる");

// ---- 決定論ハッシュ（プラットフォーム非依存）----
inline uint32_t Hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline float Hash01(uint32_t seed, uint32_t idx, uint32_t salt)
{
    const uint32_t h = Hash32(seed * 0x9e3779b1U + Hash32(idx * 0x85ebca6bU + salt * 0xc2b2ae35U + 0x27d4eb2fU));
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);   // 24bit → [0,1)
}

inline float Clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

// ---------------------------------------------------------------------------
// 波の合成。waveAmplitude <= 0 なら 0 本（平水面）。戻り値 = 本数（0..kMaxWaves）。
// ---------------------------------------------------------------------------
inline int GenerateWaves(const WaterBody& b, Wave* out)
{
    if (!(b.waveAmplitude > 1.0e-6f)) return 0;
    const int   n = std::min(std::max(b.waveCount, 1), kMaxWaves);
    const float lambda0 = std::max(b.wavelength, 0.05f);
    const float baseAng = b.windDirection * (kPi / 180.0f);
    const float spread = Clamp(b.directionSpread, 0.0f, 180.0f) * (kPi / 180.0f);
    const float choppy = Clamp(b.choppiness, 0.0f, 1.0f);
    const uint32_t seed = static_cast<uint32_t>(b.waveSeed);
    for (int i = 0; i < n; ++i)
    {
        const float u = (static_cast<float>(i) + Hash01(seed, static_cast<uint32_t>(i), 1)) / static_cast<float>(n);   // 層化: 各波が別の波長帯を受け持つ
        const float lam = lambda0 * std::exp2(0.7f - 2.6f * u);                                               // 1.6λ0 .. 0.26λ0
        const float k = 2.0f * kPi / lam;
        const float ang = baseAng + spread * (Hash01(seed, static_cast<uint32_t>(i), 2) + Hash01(seed, static_cast<uint32_t>(i), 3) - 1.0f);   // 三角分布
        float amp = b.waveAmplitude * (lam / lambda0) * (0.75f + 0.5f * Hash01(seed, static_cast<uint32_t>(i), 4));
        amp = std::min(amp, 0.5f / k);                                                                        // 傾き k·A <= 0.5（急すぎる波を作らない）
        // 急峻さ: Σ Q_i·k_i·A_i <= choppiness <= 1 ⇒ ヤコビアンは常に正（波が裏返らない）
        const float qk = static_cast<float>(n) * k * amp;
        Wave w{};
        w.steep = (qk > 1.0e-6f) ? std::min(1.0f, choppy / qk) : 1.0f;
        w.dirX = std::cos(ang);
        w.dirZ = std::sin(ang);
        w.k = k;
        w.omega = b.waveSpeed * std::sqrt(kGravity * k);
        w.amp = amp;
        w.phase = 2.0f * kPi * Hash01(seed, static_cast<uint32_t>(i), 5);
        out[i] = w;
    }
    return n;
}

// ---------------------------------------------------------------------------
// ゲルストナー波の評価（Lagrange 座標 (x0, z0) での変位・法線・ヤコビアン）。
//   θ_i = k D·((x0,z0) − flow·t) − ω t + φ
//   変位: (Σ Q A Dx cosθ, Σ A sinθ, Σ Q A Dz cosθ)
//   法線: (−Σ Dx k A cosθ, 1 − Σ Q k A sinθ, −Σ Dz k A cosθ) を正規化
//   ヤコビアン J = (1+∂Px/∂x)(1+∂Pz/∂z) − (∂Px/∂z)(∂Pz/∂x)。J < 1 で表面が圧縮される = 波頭（泡の元）
// ---------------------------------------------------------------------------
struct GerstnerSample
{
    float dx = 0, dy = 0, dz = 0;   // 変位
    float nx = 0, ny = 1, nz = 0;   // 正規化済みの法線
    float jac = 1;                  // ヤコビアン
    float slopeX = 0, slopeZ = 0;   // 非正規化の傾き（-nx/ny 相当の元）
};

inline GerstnerSample EvalGerstner(const Wave* w, int n, float x0, float z0, float t, float flowX, float flowZ)
{
    GerstnerSample s;
    float nx = 0, nz = 0, nyMinus = 0;
    float jxx = 0, jzz = 0, jxz = 0;
    for (int i = 0; i < n; ++i)
    {
        const Wave& v = w[i];
        const float theta = v.k * (v.dirX * (x0 - flowX * t) + v.dirZ * (z0 - flowZ * t)) - v.omega * t + v.phase;
        const float sn = std::sin(theta), cs = std::cos(theta);
        const float qa = v.steep * v.amp;
        s.dx += qa * v.dirX * cs;
        s.dz += qa * v.dirZ * cs;
        s.dy += v.amp * sn;
        const float ka = v.k * v.amp;
        nx -= v.dirX * ka * cs;
        nz -= v.dirZ * ka * cs;
        nyMinus += v.steep * ka * sn;
        jxx -= v.steep * ka * v.dirX * v.dirX * sn;
        jzz -= v.steep * ka * v.dirZ * v.dirZ * sn;
        jxz -= v.steep * ka * v.dirX * v.dirZ * sn;
    }
    const float ny = 1.0f - nyMinus;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    const float il = len > 1.0e-8f ? 1.0f / len : 0.0f;
    s.nx = nx * il; s.ny = ny * il; s.nz = nz * il;
    s.jac = (1.0f + jxx) * (1.0f + jzz) - jxz * jxz;
    s.slopeX = nx; s.slopeZ = nz;
    return s;
}

// ---------------------------------------------------------------------------
// 光学
// ---------------------------------------------------------------------------
// 誘電体境界のフレネル反射率（偏光平均）。cosI = 入射側の cos、eta = n2/n1（透過側 / 入射側）。全反射で 1。
inline float FresnelDielectric(float cosI, float eta)
{
    cosI = Clamp(cosI, 0.0f, 1.0f);
    const float sinT2 = (1.0f - cosI * cosI) / (eta * eta);
    if (sinT2 >= 1.0f) return 1.0f;
    const float cosT = std::sqrt(1.0f - sinT2);
    const float rs = (cosI - eta * cosT) / (cosI + eta * cosT);
    const float rp = (eta * cosI - cosT) / (eta * cosI + cosT);
    return 0.5f * (rs * rs + rp * rp);
}

// スネルの屈折。I = 入射方向（表面へ向かう単位ベクトル）、N = 入射側を向く法線、eta = n1/n2。全反射なら false（out は 0）。
inline bool Refract(const float I[3], const float N[3], float eta, float out[3])
{
    const float cosi = -(I[0] * N[0] + I[1] * N[1] + I[2] * N[2]);
    const float k = 1.0f - eta * eta * (1.0f - cosi * cosi);
    if (k < 0.0f) { out[0] = out[1] = out[2] = 0.0f; return false; }
    const float a = eta * cosi - std::sqrt(k);
    for (int i = 0; i < 3; ++i) out[i] = eta * I[i] + a * N[i];
    return true;
}

inline float Transmittance(float sigma, float thickness) { return std::exp(-sigma * thickness); }

// 水柱の消散係数（RGB）: 吸収 + 濁りが足す散乱・吸収。turbidity は 0..1。
inline void ExtinctionRgb(const WaterBody& b, float out[3])
{
    const float t = Clamp(b.turbidity, 0.0f, 1.0f);
    out[0] = b.absorption.x + t * 0.9f;
    out[1] = b.absorption.y + t * 0.9f;
    out[2] = b.absorption.z + t * 0.9f;
}

// ---------------------------------------------------------------------------
// 形
// ---------------------------------------------------------------------------
// "x z x z ..." を読む（区切りは空白 / カンマ / セミコロン）。最大 kMaxPolyPoints 点。3 点未満は空。
inline std::vector<float> ParsePolygon(const std::string& s)
{
    std::vector<float> v;
    const char* p = s.c_str();
    while (*p && static_cast<int>(v.size()) < kMaxPolyPoints * 2)
    {
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (!*p) break;
        char* end = nullptr;
        const double d = std::strtod(p, &end);
        if (end == p) break;
        v.push_back(static_cast<float>(d));
        p = end;
    }
    if (v.size() % 2 == 1) v.pop_back();
    if (v.size() < 6) v.clear();
    return v;
}

// エンティティのワールド行列（行ベクトル規約）から、水面の XZ 変換（ローカル → ワールド）と逆変換を作る。
struct WaterFrame
{
    float ox = 0, oz = 0;                 // ローカル原点のワールド XZ
    float ax = 1, az = 0, bx = 0, bz = 1; // ローカル x 軸 / z 軸（ワールド XZ での向き × スケール）
    float i00 = 1, i01 = 0, i10 = 0, i11 = 1;   // ワールド → ローカルの 2x2（(dx,dz) * 行列 = (lx,lz)）
    float level = 0;                      // 水面の高さ（ワールド y。heightOffset 込み）
};

inline WaterFrame MakeFrame(const DirectX::XMFLOAT4X4& m, float heightOffset)
{
    WaterFrame f;
    f.ox = m._41; f.oz = m._43;
    f.ax = m._11; f.az = m._13;   // ローカル x 軸 → ワールド (x, z)
    f.bx = m._31; f.bz = m._33;   // ローカル z 軸
    const float det = f.ax * f.bz - f.az * f.bx;
    if (std::fabs(det) < 1.0e-9f) { f.ax = 1; f.az = 0; f.bx = 0; f.bz = 1; }
    const float d2 = f.ax * f.bz - f.az * f.bx;
    // 行ベクトル: world = l * A, A = [[ax, az], [bx, bz]] → l = world * A^-1
    f.i00 =  f.bz / d2; f.i01 = -f.az / d2;
    f.i10 = -f.bx / d2; f.i11 =  f.ax / d2;
    f.level = m._42 + heightOffset;
    return f;
}

inline void ToLocal(const WaterFrame& f, float wx, float wz, float& lx, float& lz)
{
    const float dx = wx - f.ox, dz = wz - f.oz;
    lx = dx * f.i00 + dz * f.i10;
    lz = dx * f.i01 + dz * f.i11;
}
inline void ToWorld(const WaterFrame& f, float lx, float lz, float& wx, float& wz)
{
    wx = f.ox + lx * f.ax + lz * f.bx;
    wz = f.oz + lx * f.az + lz * f.bz;
}

// ローカル座標の点が水域の内側か（shape 0 は常に true）。poly は ParsePolygon の結果。
inline bool InsideBody(const WaterBody& b, const std::vector<float>& poly, float lx, float lz)
{
    switch (b.shape)
    {
        case 0: return true;
        case 1: return std::fabs(lx) <= 0.5f * b.size.x && std::fabs(lz) <= 0.5f * b.size.y;
        case 2:
        {
            const float rx = 0.5f * std::max(b.size.x, 1.0e-4f), rz = 0.5f * std::max(b.size.y, 1.0e-4f);
            return (lx * lx) / (rx * rx) + (lz * lz) / (rz * rz) <= 1.0f;
        }
        case 3:
        {
            const int n = static_cast<int>(poly.size() / 2);
            if (n < 3) return false;
            bool in = false;
            for (int i = 0, j = n - 1; i < n; j = i++)
            {
                const float xi = poly[i * 2], zi = poly[i * 2 + 1], xj = poly[j * 2], zj = poly[j * 2 + 1];
                if (((zi > lz) != (zj > lz)) && (lx < (xj - xi) * (lz - zi) / (zj - zi) + xi)) in = !in;
            }
            return in;
        }
        default: return true;
    }
}

// ワールド XZ の AABB（shape 0 は巨大）。minmax = {minX, minZ, maxX, maxZ}
inline void BodyAabbXZ(const WaterBody& b, const std::vector<float>& poly, const WaterFrame& f, float minmax[4])
{
    if (b.shape == 0)
    {
        minmax[0] = minmax[1] = -1.0e9f; minmax[2] = minmax[3] = 1.0e9f;
        return;
    }
    float pts[2 * kMaxPolyPoints + 8];
    int n = 0;
    if (b.shape == 3)
    {
        for (size_t i = 0; i + 1 < poly.size() && n < kMaxPolyPoints; i += 2) { pts[n * 2] = poly[i]; pts[n * 2 + 1] = poly[i + 1]; ++n; }
    }
    else
    {
        const float hx = 0.5f * b.size.x, hz = 0.5f * b.size.y;
        const float c[4][2] = {{-hx, -hz}, {hx, -hz}, {hx, hz}, {-hx, hz}};
        for (int i = 0; i < 4; ++i) { pts[n * 2] = c[i][0]; pts[n * 2 + 1] = c[i][1]; ++n; }
    }
    minmax[0] = minmax[1] = 1.0e30f; minmax[2] = minmax[3] = -1.0e30f;
    for (int i = 0; i < n; ++i)
    {
        float wx, wz;
        ToWorld(f, pts[i * 2], pts[i * 2 + 1], wx, wz);
        minmax[0] = std::min(minmax[0], wx); minmax[1] = std::min(minmax[1], wz);
        minmax[2] = std::max(minmax[2], wx); minmax[3] = std::max(minmax[3], wz);
    }
}

// 波の最大の鉛直変位（AABB の余裕・カリング用）
inline float MaxWaveHeight(const Wave* w, int n)
{
    float s = 0;
    for (int i = 0; i < n; ++i) s += w[i].amp;
    return s;
}

// ---------------------------------------------------------------------------
// プリセット（形・高さ・種・時間は変えない。見た目のパラメータだけを差し替える）。名前は小文字の英語。日本語名も受ける。
// ---------------------------------------------------------------------------
inline const char* const kPresetNames[] = {"lake", "ocean", "river", "pool", "swamp"};

inline std::string NormalizePresetName(const std::string& s)
{
    std::string t;
    for (char c : s) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (t == "湖") return "lake";
    if (t == "海") return "ocean";
    if (t == "川") return "river";
    if (t == "プール" || t == "swimmingpool") return "pool";
    if (t == "沼") return "swamp";
    return t;
}

inline bool ApplyPreset(WaterBody& b, const std::string& nameIn)
{
    const std::string name = NormalizePresetName(nameIn);
    WaterBody d;   // 既定値へ戻してから差し替える（形・高さ・種・時間は温存）
    d.shape = b.shape; d.size = b.size; d.polygon = b.polygon; d.heightOffset = b.heightOffset;
    d.waveSeed = b.waveSeed; d.timeOffset = b.timeOffset; d.timeScale = b.timeScale; d.enabled = b.enabled;
    if (name == "lake")
    {
        d.waveCount = 8; d.waveAmplitude = 0.10f; d.wavelength = 8.0f; d.windDirection = 30.0f; d.directionSpread = 40.0f;
        d.choppiness = 0.25f; d.detailStrength = 0.55f; d.detailTile = 3.0f;
        d.absorption = {0.52f, 0.12f, 0.07f}; d.scatterColor = {0.020f, 0.085f, 0.105f}; d.turbidity = 0.04f;
        d.roughness = 0.02f; d.foamCrest = 0.10f; d.foamShore = 0.55f; d.foamWidth = 0.7f; d.shoreFade = 0.45f; d.causticIntensity = 0.4f;
    }
    else if (name == "ocean")
    {
        d.waveCount = 12; d.waveAmplitude = 0.75f; d.wavelength = 34.0f; d.windDirection = 20.0f; d.directionSpread = 32.0f;
        d.choppiness = 0.65f; d.waveSpeed = 1.0f; d.detailStrength = 0.6f; d.detailTile = 4.0f;
        d.absorption = {0.42f, 0.065f, 0.026f}; d.scatterColor = {0.010f, 0.070f, 0.105f}; d.turbidity = 0.02f;
        d.roughness = 0.02f; d.foamCrest = 0.75f; d.foamShore = 0.7f; d.foamWidth = 1.2f; d.shoreFade = 0.6f; d.causticIntensity = 0.3f;
        d.shape = 0;   // 無限平面（外洋）
    }
    else if (name == "river")
    {
        d.waveCount = 8; d.waveAmplitude = 0.035f; d.wavelength = 3.5f; d.windDirection = 0.0f; d.directionSpread = 25.0f;
        d.choppiness = 0.2f; d.waveSpeed = 1.2f; d.detailStrength = 1.0f; d.detailTile = 2.2f; d.detailSpeed = 0.0f;
        d.absorption = {0.65f, 0.22f, 0.14f}; d.scatterColor = {0.030f, 0.075f, 0.070f}; d.turbidity = 0.25f;
        d.roughness = 0.03f; d.foamCrest = 0.10f; d.foamShore = 0.85f; d.foamWidth = 0.9f; d.shoreFade = 0.3f; d.causticIntensity = 0.5f;
        d.flow = {2.0f, 0.0f};
    }
    else if (name == "pool")
    {
        d.waveCount = 6; d.waveAmplitude = 0.012f; d.wavelength = 1.4f; d.windDirection = 15.0f; d.directionSpread = 60.0f;
        d.choppiness = 0.10f; d.detailStrength = 0.30f; d.detailTile = 1.6f; d.detailSpeed = 0.03f;
        d.absorption = {0.26f, 0.040f, 0.012f}; d.scatterColor = {0.050f, 0.180f, 0.215f}; d.turbidity = 0.0f;
        d.roughness = 0.008f; d.foamCrest = 0.0f; d.foamShore = 0.0f; d.foamWidth = 0.3f; d.shoreFade = 0.02f; d.causticIntensity = 1.0f;
    }
    else if (name == "swamp")
    {
        d.waveCount = 6; d.waveAmplitude = 0.02f; d.wavelength = 3.0f; d.windDirection = 60.0f; d.directionSpread = 70.0f;
        d.choppiness = 0.1f; d.detailStrength = 0.25f; d.detailTile = 2.5f; d.detailSpeed = 0.02f;
        d.absorption = {1.10f, 0.42f, 0.70f}; d.scatterColor = {0.045f, 0.060f, 0.030f}; d.turbidity = 0.55f;
        d.roughness = 0.05f; d.foamCrest = 0.0f; d.foamShore = 0.25f; d.foamWidth = 0.5f; d.shoreFade = 0.25f; d.causticIntensity = 0.0f;
    }
    else return false;
    b = d;
    return true;
}

// ---------------------------------------------------------------------------
// ディテール法線テクスチャ（手続き生成。決定論・タイル可能）。RGBA8: R,G = 傾き（0.5 中心）/ B = 泡ノイズ / A = コースティクス。
// ---------------------------------------------------------------------------
namespace detail
{
inline float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
// 周期 P の勾配ノイズ（Perlin 型の 2D）。格子点の勾配をハッシュで決める。
inline void Grad(uint32_t seed, int ix, int iy, int period, float& gx, float& gy)
{
    ix = ((ix % period) + period) % period;
    iy = ((iy % period) + period) % period;
    const float a = 2.0f * kPi * Hash01(seed, static_cast<uint32_t>(ix + iy * 4096), 77);
    gx = std::cos(a); gy = std::sin(a);
}
inline float PerlinPeriodic(uint32_t seed, float x, float y, int period)
{
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    float g00x, g00y, g10x, g10y, g01x, g01y, g11x, g11y;
    Grad(seed, x0, y0, period, g00x, g00y);
    Grad(seed, x0 + 1, y0, period, g10x, g10y);
    Grad(seed, x0, y0 + 1, period, g01x, g01y);
    Grad(seed, x0 + 1, y0 + 1, period, g11x, g11y);
    const float n00 = g00x * fx + g00y * fy;
    const float n10 = g10x * (fx - 1) + g10y * fy;
    const float n01 = g01x * fx + g01y * (fy - 1);
    const float n11 = g11x * (fx - 1) + g11y * (fy - 1);
    const float u = Fade(fx), v = Fade(fy);
    return (n00 + (n10 - n00) * u) + ((n01 + (n11 - n01) * u) - (n00 + (n10 - n00) * u)) * v;
}
inline float Fbm(uint32_t seed, float u, float v, int baseFreq, int octaves, float gain)
{
    float sum = 0, amp = 0.5f, norm = 0;
    int freq = baseFreq;
    for (int o = 0; o < octaves; ++o)
    {
        sum += amp * PerlinPeriodic(seed + static_cast<uint32_t>(o) * 131u, u * freq, v * freq, freq);
        norm += amp;
        amp *= gain;
        freq *= 2;
    }
    return sum / norm;
}
}   // namespace detail

// size x size の RGBA8 を out(4*size*size バイト)へ。
inline void GenerateDetailTexture(uint8_t* out, int size = kDetailTexSize)
{
    std::vector<float> h(static_cast<size_t>(size) * size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            h[static_cast<size_t>(y) * size + x] = detail::Fbm(1337u, (x + 0.5f) / size, (y + 0.5f) / size, 4, 5, 0.55f);
    auto H = [&](int x, int y) { return h[static_cast<size_t>(((y % size) + size) % size) * size + ((x % size) + size) % size]; };
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            // 傾き（中心差分）。振幅は「テクセルあたりの傾き」を 1 に正規化した後 0.5 中心に詰める
            const float gx = (H(x + 1, y) - H(x - 1, y)) * 0.5f * size * (1.0f / 4.0f);
            const float gy = (H(x, y + 1) - H(x, y - 1)) * 0.5f * size * (1.0f / 4.0f);
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            // 泡ノイズ: 高コントラストの fbm（0..1）
            const float fn = detail::Fbm(9001u, u, v, 6, 4, 0.6f) * 0.5f + 0.5f;
            // コースティクス: 二重の ridged fbm の積（明るい網目）
            const float r1 = 1.0f - std::fabs(detail::Fbm(4242u, u, v, 5, 3, 0.5f) * 2.2f);
            const float r2 = 1.0f - std::fabs(detail::Fbm(777u, u + 0.31f, v + 0.17f, 5, 3, 0.5f) * 2.2f);
            const float ca = std::pow(Clamp(std::max(r1, 0.0f) * std::max(r2, 0.0f), 0.0f, 1.0f), 1.6f);
            uint8_t* p = out + (static_cast<size_t>(y) * size + x) * 4;
            auto q = [](float f) { return static_cast<uint8_t>(std::lround(Clamp(f, 0.0f, 1.0f) * 255.0f)); };
            p[0] = q(0.5f + gx * 0.5f);
            p[1] = q(0.5f + gy * 0.5f);
            p[2] = q(fn);
            p[3] = q(ca * 1.6f);
        }
    }
}

// ---------------------------------------------------------------------------
// クリップマップ風のリング計画（純関数。テストが「隙間も重なりも無い」を確かめる）。
//   レベル L のセル = cell0 * 2^L、格子は世界座標に吸着する。中心 cx_L = floor(cam / (2 c_L)) * 2 c_L（自分のセルの 2 倍の倍数）。
//   レベル L の穴 = 1 つ細かいレベルの範囲 = 中心 cx_{L-1} = floor(cam / c_L) * c_L から ±(m/2) c_L。cx_{L-1} - cx_L は軸ごとに 0 か 1 セルなので
//   穴の位置はレベル L の格子でちょうど 4 通り（holeDx, holeDz ∈ {0,1}）。→ 各レベルの「四角 - 穴」が互いに隙間なく重ならない。
// ---------------------------------------------------------------------------
inline constexpr int   kGridHalf = 48;      // 1 レベルの片側セル数（= 2*kGridHalf x 2*kGridHalf セル。偶数）
inline constexpr float kCell0 = 0.20f;      // レベル 0 のセル（m）
inline constexpr int   kMaxLevels = 12;

struct RingPlan
{
    int   level = 0;
    float cell = 0.0f;
    float cx = 0.0f, cz = 0.0f;     // 中心（ワールド）
    int   holeDx = 0, holeDz = 0;   // 穴の中心のずれ（レベル L のセル単位。0 か 1）
    bool  full = false;             // レベル 0（穴なし）
};

inline double FloorDiv(double v, double d) { return std::floor(v / d); }

// 描くリングの計画。bodyAabb = {minX, minZ, maxX, maxZ}（世界）、farZ = 描く最大距離。戻り値 = 個数。
// 水域の AABB と交わらないレベルは省く。カメラから穴の縁までが farZ を超えるレベル以降は打ち切る。
inline int PlanRings(double camX, double camZ, float farZ, const float bodyAabb[4], RingPlan* out, int maxOut = kMaxLevels)
{
    int n = 0;
    for (int L = 0; L < kMaxLevels && n < maxOut; ++L)
    {
        const double cell = static_cast<double>(kCell0) * std::exp2(static_cast<double>(L));
        const double innerR = (L == 0) ? 0.0 : (kGridHalf / 2 - 1) * cell;   // 穴の縁までのカメラからの最小距離の見積もり
        if (L > 0 && innerR > static_cast<double>(farZ) * 1.05) break;
        const double cx = FloorDiv(camX, 2.0 * cell) * 2.0 * cell;
        const double cz = FloorDiv(camZ, 2.0 * cell) * 2.0 * cell;
        const double half = kGridHalf * cell;
        // 水域の AABB と外周の四角が交わるか
        if (bodyAabb[2] < cx - half || bodyAabb[0] > cx + half || bodyAabb[3] < cz - half || bodyAabb[1] > cz + half) continue;
        RingPlan p;
        p.level = L; p.cell = static_cast<float>(cell); p.cx = static_cast<float>(cx); p.cz = static_cast<float>(cz);
        p.full = (L == 0);
        if (L > 0)
        {
            const double pcx = FloorDiv(camX, cell) * cell;   // 1 つ細かいレベルの中心
            const double pcz = FloorDiv(camZ, cell) * cell;
            p.holeDx = static_cast<int>(std::llround((pcx - cx) / cell));
            p.holeDz = static_cast<int>(std::llround((pcz - cz) / cell));
        }
        out[n++] = p;
    }
    return n;
}

// 点 (x, z) がレベル p の「四角 - 穴」の中か（テスト用）
inline bool RingContains(const RingPlan& p, double x, double z)
{
    const double half = kGridHalf * static_cast<double>(p.cell);
    if (std::fabs(x - p.cx) >= half || std::fabs(z - p.cz) >= half) return false;
    if (p.full) return true;
    const double hh = (kGridHalf / 2) * static_cast<double>(p.cell);
    const double hx = p.cx + p.holeDx * static_cast<double>(p.cell);
    const double hz = p.cz + p.holeDz * static_cast<double>(p.cell);
    return !(std::fabs(x - hx) < hh && std::fabs(z - hz) < hh);
}

// 水面の高さ（ワールド y）を (x, z) で求める（1 回の不動点反復で Lagrange 座標へ戻す）。水中判定用。
inline float SurfaceHeightAt(const WaterBody& b, const Wave* w, int n, const WaterFrame& fr, float x, float z, float t)
{
    if (n <= 0) return fr.level;
    GerstnerSample s = EvalGerstner(w, n, x, z, t, b.flow.x, b.flow.y);
    s = EvalGerstner(w, n, x - s.dx, z - s.dz, t, b.flow.x, b.flow.y);
    return fr.level + s.dy;
}

// Application 側で使う: 決定論的なフレーム時刻（秒）。timeOffset と timeScale を反映する。
inline float BodyTime(const WaterBody& b, float totalTime) { return totalTime * b.timeScale + b.timeOffset; }

}   // namespace dx12e::water
