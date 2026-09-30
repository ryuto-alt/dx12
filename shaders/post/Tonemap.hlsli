// Tonemap.hlsli — 新しいトーンマップ（Q2 校正）: UE 5 の Filmic / 線形(クリップのみ) / Khronos PBR Neutral。
// ★式は src/renderer/PhotometricMath.h の写し（tests/photometric_test.cpp が numpy 実装との突き合わせで検証している）。
//   片方を直したら両方直すこと。
// ★従来の ACES / AgX / なし（PostProcess.hlsl の ToneMapGamma）はここに移していない（既定経路のコードを 1 命令も
//   動かさないため）。ここは masks.w >= 3 のときだけ呼ばれる。
// 出力は表示(ガンマ)空間の値。新モードは sRGB の OETF で符号化する（従来モードは pow(1/2.2)）。

#ifndef TONEMAP_HLSLI
#define TONEMAP_HLSLI

static float UnoLinearToSrgb1(float x)
{
    x = saturate(x);
    return (x <= 0.0031308) ? 12.92 * x : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
}
static float3 UnoLinearToSrgb(float3 x)
{
    return float3(UnoLinearToSrgb1(x.r), UnoLinearToSrgb1(x.g), UnoLinearToSrgb1(x.b));
}

// 1 チャンネルぶんの S 字（対数空間）。★ベクトルの ?: は HLSL 2021 で無効なのでチャンネルごとに呼ぶ。
static float UnoFilmCurve(float lc, float slope, float toeScale, float shScale, float blackClip, float whiteClip,
                          float toeMatch, float straightMatch, float shoulderMatch)
{
    const float straight = slope * (lc + straightMatch);
    float toeC = -blackClip + (2.0 * toeScale) / (1.0 + exp((-2.0 * slope / toeScale) * (lc - toeMatch)));
    float shC  = (1.0 + whiteClip) - (2.0 * shScale) / (1.0 + exp((2.0 * slope / shScale) * (lc - shoulderMatch)));
    toeC = (lc < toeMatch) ? toeC : straight;
    shC  = (lc > shoulderMatch) ? shC : straight;
    float t = saturate((lc - toeMatch) / (shoulderMatch - toeMatch));
    if (shoulderMatch < toeMatch) t = 1.0 - t;
    t = (3.0 - 2.0 * t) * t * t;
    return lerp(toeC, shC, t);
}

// ---- UE 5 の Filmic（ACES 系。根拠と不確実性は PhotometricMath.h の注記）----
// film = (slope, toe, shoulder, blackClip)、filmWhite = whiteClip
static float3 UnoUeFilmicLinear(float3 lin, float4 film, float filmWhite)
{
    // chromaticity から数値計算した行列（行ベクトル × 転置ではなく mul(M, v) の列ベクトル規約で書いてある）
    static const float3x3 kSrgbToAp0 = float3x3(
        0.4396329819, 0.3829886982, 0.1773783199,
        0.0897764430, 0.8134394287, 0.0967841283,
        0.0175411704, 0.1115465533, 0.8709122763);
    static const float3x3 kAp0ToAp1 = float3x3(
         1.4514393161, -0.2365107469, -0.2149285693,
        -0.0765537734,  1.1762296998, -0.0996759264,
         0.0083161484, -0.0060324498,  0.9977163014);
    static const float3x3 kAp1ToSrgb = float3x3(
         1.7050509927, -0.6217921207, -0.0832588720,
        -0.1302564175,  1.1408047366, -0.0105483191,
        -0.0240033568, -0.1289689761,  1.1529723329);
    const float3 ap1Y = float3(0.2722287168, 0.6740817658, 0.0536895174);

    const float slope = film.x, toe = film.y, shoulder = film.z, blackClip = film.w, whiteClip = filmWhite;

    float3 aces = mul(kSrgbToAp0, lin);

    // ---- RRT: グロー（低彩度・暗部を持ち上げる）と赤の変調 ----
    const float mi = min(aces.r, min(aces.g, aces.b));
    const float ma = max(aces.r, max(aces.g, aces.b));
    const float sat = (max(ma, 1e-10) - max(mi, 1e-10)) / max(ma, 1e-2);
    const float chroma = sqrt(max(aces.b * (aces.b - aces.g) + aces.g * (aces.g - aces.r) + aces.r * (aces.r - aces.b), 0.0));
    const float yc = (aces.r + aces.g + aces.b + 1.75 * chroma) / 3.0;
    const float sx = (sat - 0.4) / 0.2;
    const float st = max(1.0 - abs(sx / 2.0), 0.0);
    const float sShaper = (1.0 + (sx < 0.0 ? -1.0 : (sx > 0.0 ? 1.0 : 0.0)) * (1.0 - st * st)) / 2.0;
    const float glowGain = 0.05 * sShaper;
    const float glowMid = 0.08;
    float glow;
    if (yc <= (2.0 / 3.0) * glowMid)  glow = glowGain;
    else if (yc >= 2.0 * glowMid)     glow = 0.0;
    else                              glow = glowGain * (glowMid / yc - 0.5);
    aces *= 1.0 + glow;

    float hue = 0.0;
    if (!(aces.r == aces.g && aces.g == aces.b))
    {
        hue = atan2(sqrt(3.0) * (aces.g - aces.b), 2.0 * aces.r - aces.g - aces.b) * (180.0 / 3.14159265358979);
        if (hue < 0.0) hue += 360.0;
    }
    float centered = hue;
    if (centered < -180.0) centered += 360.0; else if (centered > 180.0) centered -= 360.0;
    const float ru = saturate(1.0 - abs(2.0 * centered / 135.0));
    const float hueSm = ru * ru * (3.0 - 2.0 * ru);
    const float hueW = hueSm * hueSm;
    aces.r += hueW * sat * (0.03 - aces.r) * (1.0 - 0.82);

    float3 pre = max(mul(kAp0ToAp1, aces), 0.0);
    const float py = dot(pre, ap1Y);
    pre = py + (pre - py) * 0.96;

    // ---- S 字（対数空間: toe = ロジスティック / 中央 = 直線 / shoulder = ロジスティック）----
    const float toeScale = 1.0 + blackClip - toe;
    const float shScale  = 1.0 + whiteClip - shoulder;
    const float inMatch = 0.18, outMatch = 0.18;
    float toeMatch;
    if (toe > 0.8)
        toeMatch = (1.0 - toe - outMatch) / slope + log10(inMatch);
    else
    {
        const float bt = (outMatch + blackClip) / toeScale - 1.0;
        toeMatch = log10(inMatch) - 0.5 * log((1.0 + bt) / (1.0 - bt)) * (toeScale / slope);
    }
    const float straightMatch = (1.0 - toe) / slope - toeMatch;
    const float shoulderMatch = shoulder / slope - straightMatch;

    const float3 lc = log10(max(pre, 1e-30));
    float3 tone;
    tone.r = UnoFilmCurve(lc.r, slope, toeScale, shScale, blackClip, whiteClip, toeMatch, straightMatch, shoulderMatch);
    tone.g = UnoFilmCurve(lc.g, slope, toeScale, shScale, blackClip, whiteClip, toeMatch, straightMatch, shoulderMatch);
    tone.b = UnoFilmCurve(lc.b, slope, toeScale, shScale, blackClip, whiteClip, toeMatch, straightMatch, shoulderMatch);

    const float ty = dot(tone, ap1Y);
    tone = max(ty + (tone - ty) * 0.93, 0.0);
    return mul(kAp1ToSrgb, tone);
}

// ---- Khronos PBR Neutral ----
static float3 UnoPbrNeutralLinear(float3 c)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    const float x = min(c.r, min(c.g, c.b));
    const float offset = (x < 0.08) ? x - 6.25 * x * x : 0.04;
    c -= offset;
    const float peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) return c;
    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(c, newPeak.xxx, g);
}

// masks.w >= 3 の表示変換（戻り値 = 表示(ガンマ)空間 0..1）。
// tm: 3 = UE Filmic / 4 = 線形(クリップのみ) / 5 = Khronos PBR Neutral
static float3 UnoToneMapNew(int tm, float3 c, float4 film, float filmWhite)
{
    c = max(c, 0.0);
    if (tm == 3) return UnoLinearToSrgb(UnoUeFilmicLinear(c, film, filmWhite));
    if (tm == 5) return UnoLinearToSrgb(UnoPbrNeutralLinear(c));
    return UnoLinearToSrgb(c);   // 4: 線形（トーンマップ無し。1 でクリップ）
}

#endif // TONEMAP_HLSLI
