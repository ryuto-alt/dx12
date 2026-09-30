#pragma once
// ===========================================================================
// PhotometricMath — 光の物理単位・露出(EV100)・トーンマップの「純関数」(Q2 校正)
// ---------------------------------------------------------------------------
// ヘッダオンリー・標準ライブラリのみ(GPU も DirectXMath も要らない)。単体テスト tests/photometric_test.cpp が
// この式を独立に検証し、HLSL 側(shaders/post/Tonemap.hlsli / shaders/forward/Lighting.hlsli の
// UNO_PHYSICAL_LIGHTS 経路 / shaders/post/AutoExposure.hlsl)は**この式の写し**。片方を直したら両方直すこと。
// MCP の CPU 側スクショ(screenshot)もここのトーンマップを共有する(GPU の絵と食い違わせない)。
//
// 単位系(物理モード)
//   ・シーンの線形 RGB は輝度 [nit = cd/m^2] そのもの(エンジン単位 1.0 = 1 nit)。
//   ・太陽 DirectionalLight.intensity = 法線に垂直な面の照度 [lux]。拡散面の放射輝度 = albedo/π · E · cosθ。
//   ・点/スポット intensity = 光度 [cd]。面の照度 = I · cosθ / d^2(逆二乗)。
//   ・空/IBL/ambient(iblIntensity・skyboxIntensity・DirectionalLight.ambient)= 環境の輝度 [nit]。
//   ・自己発光 emissiveIntensity = 輝度 [nit]。
//   ・表示への変換 = 露出係数 F(EV100) = 1 / (1.2 · 2^EV100)(ISO 12232 の飽和ベース Lmax = 1.2·2^EV100)。
//     F を掛けた値 1.0 = 表示の白。既定 EV100 = 15(晴天 Sunny 16。太陽 10 万 lux の下の 18% グレーが約 0.14)。
// ===========================================================================
#include <algorithm>
#include <cmath>

namespace dx12e::photo
{

inline constexpr float kPi = 3.14159265358979323846f;

// ---- 露出 ------------------------------------------------------------------
inline constexpr float kExposureLumScale = 1.2f;    // Lmax(EV100) = 1.2 · 2^EV100 [nit]
inline constexpr float kMidGray          = 0.18f;   // 自動露出の目標(平均輝度がこの値へ写る)
inline constexpr float kDefaultEv100     = 15.0f;   // 物理モードの既定 EV100(= 露出 0 の基準)

// 露出係数(線形の乗数)。F = 1 / (1.2 · 2^EV100)
inline float ExposureScaleFromEv100(float ev100)
{
    return 1.0f / (kExposureLumScale * std::exp2(ev100));
}
inline float Ev100FromExposureScale(float scale)
{
    return -std::log2((std::max)(scale, 1e-30f) * kExposureLumScale);
}
// 手動露出: EV100 と補正(EV。+ で明るく)。補正 +1 = 係数 2 倍。
inline float ManualExposureScale(float ev100, float evCompensation)
{
    return ExposureScaleFromEv100(ev100 - evCompensation);
}
// 自動露出: 平均輝度 avgNit [nit] を 18% グレー(×2^補正)へ写す EV100。
inline float AutoEv100FromLuminance(float avgNit, float evCompensation)
{
    return std::log2((std::max)(avgNit, 1e-6f) / (kExposureLumScale * kMidGray)) - evCompensation;
}
// EV100 → 「その EV100 で 18% グレーに写る輝度」の log2。自動露出のヒストグラム範囲(log2 輝度)へ変換する。
inline float Log2LuminanceFromEv100(float ev100)
{
    return ev100 + std::log2(kExposureLumScale * kMidGray);
}
inline float Ev100FromLog2Luminance(float log2Lum)
{
    return log2Lum - std::log2(kExposureLumScale * kMidGray);
}

// ---- 光の単位 ---------------------------------------------------------------
inline float LumensToCandelaPoint(float lumens) { return lumens / (4.0f * kPi); }
inline float CandelaToLumensPoint(float cd)     { return cd * 4.0f * kPi; }
// スポット: 半頂角 outerHalfDeg の円錐に一様に出す近似(立体角 Ω = 2π(1 − cosθ))。
inline float SpotSolidAngle(float outerHalfDeg)
{
    const float th = (std::min)((std::max)(outerHalfDeg, 0.01f), 89.99f) * kPi / 180.0f;
    const float s = std::sin(0.5f * th);
    return 4.0f * kPi * s * s;   // = 2π(1 − cosθ)。細い円錐で桁落ちしない形
}
inline float LumensToCandelaSpot(float lumens, float outerHalfDeg) { return lumens / SpotSolidAngle(outerHalfDeg); }
inline float CandelaToLumensSpot(float cd, float outerHalfDeg)     { return cd * SpotSolidAngle(outerHalfDeg); }

// 点光源(光度 I [cd])が距離 d で法線方向 cosTheta に作る照度 [lux]。
inline float PointIlluminance(float candela, float dist, float cosTheta)
{
    return candela * (std::max)(cosTheta, 0.0f) / (std::max)(dist * dist, 1e-8f);
}
// ランバート面(反射率 albedo)の放射輝度 [nit] = albedo/π · 照度。
inline float LambertRadiance(float albedo, float illuminance) { return albedo / kPi * illuminance; }

// 物理モードの点/スポットの減衰(HLSL の AccumulatePunctualLights の UNO_PHYSICAL_LIGHTS 経路と同じ式)。
//   逆二乗 1/max(d², r²) に、影響半径 range の滑らかな窓 saturate(1 − (d/range)^4)^2 を掛ける。
//   r = 光源の半径(球光源。d < r では最大 1/r² で頭打ち)。range が d に比べて十分大きければ窓は 1。
inline float PhysicalFalloff(float dist, float range, float sourceRadius)
{
    const float r  = (std::max)(sourceRadius, 0.01f);
    const float d2 = (std::max)(dist * dist, r * r);
    const float x  = dist / (std::max)(range, 1e-4f);
    const float w  = (std::min)((std::max)(1.0f - x * x * x * x, 0.0f), 1.0f);
    return w * w / d2;
}
// 従来モードの減衰(Lighting.hlsli): saturate(1 − d/range)^2
inline float LegacyFalloff(float dist, float range)
{
    const float a = (std::min)((std::max)(1.0f - dist / (std::max)(range, 1e-4f), 0.0f), 1.0f);
    return a * a;
}

// ---- 色空間 / トーンマップ --------------------------------------------------
struct Rgb { float r, g, b; };

inline float Saturate(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// sRGB の OETF(リニア → エンコード)。
inline float LinearToSrgb(float x)
{
    x = Saturate(x);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

// 従来の ACES(Narkowicz のフィット)。PostProcess.hlsl の ACESFilm と同じ。
inline float AcesNarkowicz(float x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return Saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// ---- UE 5 の Filmic(既定の ACES 系トーンマッパ)の再現 ------------------------
// ★根拠と不確実性(docs/PARITY_HARNESS.md の Q2 節にも書く):
//   ・公開文書(Color Grading and the Filmic Tonemapper)が示すのは既定値だけ:
//     Slope 0.88 / Toe 0.55 / Shoulder 0.26 / Black Clip 0 / White Clip 0.04。
//   ・式は ACES 1.0 の参照実装(RRT のグロー・赤変調・彩度係数 0.96)と、UE が使う「対数空間の S 字
//     (下端 = ロジスティックの toe、中央 = 直線、上端 = ロジスティックの shoulder を smoothstep で継ぐ)」を
//     **記憶から独自に書き起こした**もの。UE のシェーダのコードは 1 行もコピーしていないし、UE 本体でも
//     照合できていない(UE は再インストールしない方針)。
//   ・確かな性質(構成から): 0.18 の灰 → 0.18 のまま、0 → 0、大きな入力 → 1 + WhiteClip に漸近、単調増加。
//   ・不確実: グロー・赤変調・彩度係数の細部(UE 5.x の版ごとの差)、AP1 → sRGB の色域処理(UE は既定で
//     色域拡張を掛けない)、出力を sRGB OETF にする点(UE の既定表示は sRGB)。
struct FilmParams
{
    float slope = 0.88f, toe = 0.55f, shoulder = 0.26f, blackClip = 0.0f, whiteClip = 0.04f;
};

inline Rgb UeFilmicLinear(Rgb in, const FilmParams& p = FilmParams())
{
    // 行列(chromaticity から数値計算したもの。sRGB(D65) → AP0(D60。Bradford 順応)/ AP0 → AP1 / AP1 → sRGB(D60 → D65))
    static const float kSrgbToAp0[9] = {
        0.4396329819f, 0.3829886982f, 0.1773783199f,
        0.0897764430f, 0.8134394287f, 0.0967841283f,
        0.0175411704f, 0.1115465533f, 0.8709122763f };
    static const float kAp0ToAp1[9] = {
         1.4514393161f, -0.2365107469f, -0.2149285693f,
        -0.0765537734f,  1.1762296998f, -0.0996759264f,
         0.0083161484f, -0.0060324498f,  0.9977163014f };
    static const float kAp1ToSrgb[9] = {
         1.7050509927f, -0.6217921207f, -0.0832588720f,
        -0.1302564175f,  1.1408047366f, -0.0105483191f,
        -0.0240033568f, -0.1289689761f,  1.1529723329f };
    const float ap1Y[3] = { 0.2722287168f, 0.6740817658f, 0.0536895174f };
    auto mul3 = [](const float m[9], const float v[3], float o[3])
    {
        o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
        o[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
        o[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
    };

    const float src[3] = { in.r, in.g, in.b };
    float aces[3];
    mul3(kSrgbToAp0, src, aces);

    // ---- RRT: グロー(低彩度・暗部を持ち上げる)と赤の変調 ----
    const float mi = (std::min)(aces[0], (std::min)(aces[1], aces[2]));
    const float ma = (std::max)(aces[0], (std::max)(aces[1], aces[2]));
    const float sat = ((std::max)(ma, 1e-10f) - (std::max)(mi, 1e-10f)) / (std::max)(ma, 1e-2f);
    const float chroma = std::sqrt((std::max)(aces[2] * (aces[2] - aces[1]) + aces[1] * (aces[1] - aces[0])
                                            + aces[0] * (aces[0] - aces[2]), 0.0f));
    const float yc = (aces[0] + aces[1] + aces[2] + 1.75f * chroma) / 3.0f;
    const float sx = (sat - 0.4f) / 0.2f;
    const float st = (std::max)(1.0f - std::fabs(sx / 2.0f), 0.0f);
    const float sShaper = (1.0f + (sx < 0.0f ? -1.0f : (sx > 0.0f ? 1.0f : 0.0f)) * (1.0f - st * st)) / 2.0f;
    const float glowGain = 0.05f * sShaper, glowMid = 0.08f;
    float glow;
    if (yc <= (2.0f / 3.0f) * glowMid)   glow = glowGain;
    else if (yc >= 2.0f * glowMid)       glow = 0.0f;
    else                                 glow = glowGain * (glowMid / yc - 0.5f);
    const float addedGlow = 1.0f + glow;
    for (float& c : aces) c *= addedGlow;

    float hue = 0.0f;
    if (!(aces[0] == aces[1] && aces[1] == aces[2]))
    {
        hue = std::atan2(std::sqrt(3.0f) * (aces[1] - aces[2]), 2.0f * aces[0] - aces[1] - aces[2]) * (180.0f / kPi);
        if (hue < 0.0f) hue += 360.0f;
    }
    float centered = hue;
    if (centered < -180.0f) centered += 360.0f; else if (centered > 180.0f) centered -= 360.0f;
    const float ru = Saturate(1.0f - std::fabs(2.0f * centered / 135.0f));
    const float hueW = (ru * ru * (3.0f - 2.0f * ru)) * (ru * ru * (3.0f - 2.0f * ru));
    aces[0] += hueW * sat * (0.03f - aces[0]) * (1.0f - 0.82f);

    float pre[3];
    mul3(kAp0ToAp1, aces, pre);
    for (float& c : pre) c = (std::max)(c, 0.0f);
    const float py = pre[0] * ap1Y[0] + pre[1] * ap1Y[1] + pre[2] * ap1Y[2];
    for (float& c : pre) c = py + (c - py) * 0.96f;

    // ---- S 字(対数空間) ----
    const float toeScale = 1.0f + p.blackClip - p.toe;
    const float shScale  = 1.0f + p.whiteClip - p.shoulder;
    const float inMatch = 0.18f, outMatch = 0.18f;
    float toeMatch;
    if (p.toe > 0.8f)
        toeMatch = (1.0f - p.toe - outMatch) / p.slope + std::log10(inMatch);
    else
    {
        const float bt = (outMatch + p.blackClip) / toeScale - 1.0f;
        toeMatch = std::log10(inMatch) - 0.5f * std::log((1.0f + bt) / (1.0f - bt)) * (toeScale / p.slope);
    }
    const float straightMatch = (1.0f - p.toe) / p.slope - toeMatch;
    const float shoulderMatch = p.shoulder / p.slope - straightMatch;

    float tone[3];
    for (int i = 0; i < 3; ++i)
    {
        const float lc = std::log10((std::max)(pre[i], 1e-30f));
        const float straight = p.slope * (lc + straightMatch);
        float toeC = -p.blackClip + (2.0f * toeScale) / (1.0f + std::exp((-2.0f * p.slope / toeScale) * (lc - toeMatch)));
        float shC  = (1.0f + p.whiteClip) - (2.0f * shScale) / (1.0f + std::exp((2.0f * p.slope / shScale) * (lc - shoulderMatch)));
        toeC = lc < toeMatch ? toeC : straight;
        shC  = lc > shoulderMatch ? shC : straight;
        float t = Saturate((lc - toeMatch) / (shoulderMatch - toeMatch));
        if (shoulderMatch < toeMatch) t = 1.0f - t;
        t = (3.0f - 2.0f * t) * t * t;
        tone[i] = toeC + (shC - toeC) * t;
    }
    const float ty = tone[0] * ap1Y[0] + tone[1] * ap1Y[1] + tone[2] * ap1Y[2];
    for (float& c : tone) c = (std::max)(ty + (c - ty) * 0.93f, 0.0f);

    float out[3];
    mul3(kAp1ToSrgb, tone, out);
    return { out[0], out[1], out[2] };
}

// ---- Khronos PBR Neutral(glTF の KHR_materials 系の標準ビュー用トーンマップ。Apache-2.0 の公開アルゴリズム) ----
inline Rgb PbrNeutralLinear(Rgb c)
{
    const float startCompression = 0.8f - 0.04f;
    const float desaturation = 0.15f;
    const float x = (std::min)(c.r, (std::min)(c.g, c.b));
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    c.r -= offset; c.g -= offset; c.b -= offset;
    const float peak = (std::max)(c.r, (std::max)(c.g, c.b));
    if (peak < startCompression) return c;
    const float d = 1.0f - startCompression;
    const float newPeak = 1.0f - d * d / (peak + d - startCompression);
    const float k = newPeak / peak;
    c.r *= k; c.g *= k; c.b *= k;
    const float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return { c.r + (newPeak - c.r) * g, c.g + (newPeak - c.g) * g, c.b + (newPeak - c.b) * g };
}

// ---- トーンマップの選択(PostProcessSettings::tonemapper の値) ---------------
enum ToneMapper : int
{
    kTmAces          = 0,   // 従来の既定(Narkowicz ACES + pow(1/2.2))
    kTmAgx           = 1,   // AgX(Wrensch のフィット。ガンマ空間の値を直接返す)
    kTmNone          = 2,   // なし(ガンマのみ: pow(x,1/2.2))。従来のまま
    kTmUeFilmic      = 3,   // UE 5 の Filmic(ACES 系。出力は sRGB OETF)
    kTmLinearClip    = 4,   // 線形(トーンマップ無し。1 でクリップ + sRGB OETF)
    kTmPbrNeutral    = 5,   // Khronos PBR Neutral(出力は sRGB OETF)
    kTmCount         = 6,
};

// UE Filmic をディスプレイ値(sRGB エンコード済み 0..1)へ。
inline Rgb UeFilmicDisplay(Rgb in, const FilmParams& p = FilmParams())
{
    const Rgb l = UeFilmicLinear(in, p);
    return { LinearToSrgb(l.r), LinearToSrgb(l.g), LinearToSrgb(l.b) };
}
inline Rgb PbrNeutralDisplay(Rgb in)
{
    const Rgb l = PbrNeutralLinear({ (std::max)(in.r, 0.0f), (std::max)(in.g, 0.0f), (std::max)(in.b, 0.0f) });
    return { LinearToSrgb(l.r), LinearToSrgb(l.g), LinearToSrgb(l.b) };
}
inline Rgb LinearClipDisplay(Rgb in)
{
    return { LinearToSrgb(in.r), LinearToSrgb(in.g), LinearToSrgb(in.b) };
}

// 新しいトーンマップ(3..5)だけをここで面倒を見る。0..2(従来)は呼び出し側の既存の式を使う
// (PostProcess.hlsl の ToneMapGamma / MCP の CPU 側ミラー)。戻り値 = 表示(ガンマ空間)値。
inline bool IsNewToneMapper(int tm) { return tm >= kTmUeFilmic && tm < kTmCount; }
inline Rgb ToneMapNewDisplay(int tm, Rgb in, const FilmParams& film = FilmParams())
{
    switch (tm)
    {
    case kTmUeFilmic:   return UeFilmicDisplay(in, film);
    case kTmLinearClip: return LinearClipDisplay(in);
    case kTmPbrNeutral: return PbrNeutralDisplay(in);
    default:            return in;
    }
}

} // namespace dx12e::photo
