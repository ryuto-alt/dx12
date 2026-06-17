// シーンをサンプルしてポストエフェクトを適用し、バックバッファへ出力する単一パス。
// 各エフェクトは enableMask のビットで個別に ON/OFF される。
// 多くは godotshaders.com のスクリーンシェーダーを解析して移植したもの。

#include "FullscreenTri.hlsli"

static const float PI = 3.14159265;

// 有効ビット（PostProcess.cpp の PostEffectBit と一致）
#define E_EXPOSURE   (1u<<0)
#define E_CONTRAST   (1u<<1)
#define E_BRIGHTNESS (1u<<2)
#define E_SATURATION (1u<<3)
#define E_WARMTH     (1u<<4)
#define E_TINT       (1u<<5)
#define E_HUE        (1u<<6)
#define E_BLOOM      (1u<<7)
#define E_VIGNETTE   (1u<<8)
#define E_CHROMATIC  (1u<<9)
#define E_PIXELIZE   (1u<<10)
#define E_POSTERIZE  (1u<<11)
#define E_DITHER     (1u<<12)
#define E_SCANLINE   (1u<<13)
#define E_SHARPEN    (1u<<14)
#define E_GRAIN      (1u<<15)
#define E_INVERT     (1u<<16)
#define E_SEPIA      (1u<<17)
#define E_GRAYSCALE  (1u<<18)
#define E_LENS       (1u<<19)
#define E_WAVE       (1u<<20)
#define E_RADIAL     (1u<<21)
#define E_GLITCH     (1u<<22)
#define E_OUTLINE    (1u<<23)
#define E_FXAA       (1u<<24)

cbuffer PostCB : register(b0)
{
    float4 uvOffsetScale;   // xy=UVオフセット, zw=UVスケール（ビューポート矩形対応）
    float4 texelTime;       // xy=テクセル(1/W,1/H), z=time
    int4   masks;           // x=enableMask, y=posterizeLevels, z=ditherLevels
    float4 cg0;             // exposure, contrast, brightness, saturation
    float4 cg1;             // warmth, hueShift(度), bloom, bloomThreshold
    float4 tintVig;         // tint.rgb, vignette
    float4 stylize0;        // chromatic, pixelSize, scanline, sharpen
    float4 stylize1;        // grain, invert, sepia, grayscale
    float4 dist0;           // lens, radial, glitch, _
    float4 wave;            // waveAmp, waveFreq, waveSpeed, _
    float4 outlineP;        // outlineColor.rgb, outlineStrength
};

Texture2D    gScene : register(t0);
SamplerState gSamp  : register(s0);

static float3 SampleScene(float2 uv) { return gScene.Sample(gSamp, uv).rgb; }
static float  Luma(float3 c)         { return dot(c, float3(0.299, 0.587, 0.114)); }

// 色相回転（角度ラジアン）。YIQ 風の回転行列で近似。
static float3 HueRotate(float3 col, float angle)
{
    float c = cos(angle), s = sin(angle);
    float3x3 m = float3x3(
        0.299 + 0.701*c + 0.168*s, 0.587 - 0.587*c + 0.330*s, 0.114 - 0.114*c - 0.497*s,
        0.299 - 0.299*c - 0.328*s, 0.587 + 0.413*c + 0.035*s, 0.114 - 0.114*c + 0.292*s,
        0.299 - 0.300*c + 1.250*s, 0.587 - 0.588*c - 1.050*s, 0.114 + 0.886*c - 0.203*s);
    return mul(m, col);
}

static float Rand(float2 p) { return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453); }

// ACES フィルミックトーンマップ（Narkowicz 近似）。HDR(RGBA16F)→表示用にロールオフ。
static float3 ACESFilm(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// 4x4 Bayer 順序ディザ閾値（godotshaders の ordered dithering 由来）
static float Bayer4(float2 px)
{
    int x = (int)fmod(px.x, 4.0);
    int y = (int)fmod(px.y, 4.0);
    int idx = x + y * 4;
    float m[16] = {  0.0,  8.0,  2.0, 10.0,
                    12.0,  4.0, 14.0,  6.0,
                     3.0, 11.0,  1.0,  9.0,
                    15.0,  7.0, 13.0,  5.0 };
    return m[idx] / 16.0;
}

float4 PostPS(FSQuadVSOut i) : SV_TARGET
{
    float2 scale = uvOffsetScale.zw;
    float2 ofs   = uvOffsetScale.xy;
    float2 texel = texelTime.xy;
    float  time  = texelTime.z;
    uint   mask  = (uint)masks.x;

    // マスター無効（全ビット 0）でも HDR バッファなのでトーンマップは通す
    if (mask == 0u)
        return float4(ACESFilm(SampleScene(i.uv * scale + ofs)), 1.0);

    float2 luv    = i.uv;          // ビューポートローカル 0..1
    bool   crtOut = false;

    // ===== UV ステージ（サンプル前に UV を歪ませる） =====

    // --- レンズ歪み（バレル） ---
    if (mask & E_LENS)
    {
        float2 d = luv - 0.5;
        float r2 = dot(d, d);
        luv = 0.5 + d * (1.0 + dist0.x * r2 * 1.5);
    }

    // --- 波ゆらぎ（水中/陽炎） ---
    if (mask & E_WAVE)
    {
        luv.x += sin(luv.y * wave.y + time * wave.z) * wave.x;
        luv.y += cos(luv.x * wave.y + time * wave.z) * wave.x;
    }

    // --- CRT 画面湾曲 ---
    if (mask & E_SCANLINE)
    {
        float2 d = luv - 0.5;
        luv += d * dot(d, d) * (stylize0.z * 0.18);
        if (luv.x < 0.0 || luv.x > 1.0 || luv.y < 0.0 || luv.y > 1.0)
            crtOut = true;
    }

    // --- デジタルグリッチ（行ブロックを横ずらし） ---
    if (mask & E_GLITCH)
    {
        float blockY = floor(luv.y * 24.0);
        float n = Rand(float2(blockY, floor(time * 12.0)));
        if (n > 0.8)
            luv.x += (Rand(float2(blockY, time)) - 0.5) * dist0.z * 0.3;
    }

    float2 uv = luv * scale + ofs;

    // --- ピクセル化 ---
    if (mask & E_PIXELIZE)
    {
        float2 block = max(stylize0.y, 1.0) * texel;
        uv = (floor(uv / block) + 0.5) * block;
    }

    // ===== サンプル ステージ =====
    float3 col;
    if (mask & E_CHROMATIC)
    {
        float2 dir = (luv - 0.5) * stylize0.x * 0.03;
        col.r = SampleScene(uv + dir).r;
        col.g = SampleScene(uv).g;
        col.b = SampleScene(uv - dir).b;
    }
    else if (mask & E_FXAA)
    {
        float3 c = SampleScene(uv);
        float3 n = SampleScene(uv + float2(0, -texel.y));
        float3 s = SampleScene(uv + float2(0,  texel.y));
        float3 e = SampleScene(uv + float2( texel.x, 0));
        float3 w = SampleScene(uv + float2(-texel.x, 0));
        float edge  = abs(Luma(n) - Luma(s)) + abs(Luma(e) - Luma(w));
        col = lerp(c, (c + n + s + e + w) * 0.2, saturate(edge * 2.0));
    }
    else
    {
        col = SampleScene(uv);
    }

    // --- 放射ブラー（中心へズーム） ---
    if (mask & E_RADIAL)
    {
        float2 dir = (luv - 0.5);
        float3 acc = col;
        [unroll] for (int k = 1; k <= 6; ++k)
        {
            float t = (float)k / 6.0;
            acc += SampleScene(uv - dir * t * dist0.y * 0.15);
        }
        col = acc / 7.0;
    }

    // --- シャープ（アンシャープマスク） ---
    if (mask & E_SHARPEN)
    {
        float3 blur = (SampleScene(uv + float2( texel.x, 0)) +
                       SampleScene(uv + float2(-texel.x, 0)) +
                       SampleScene(uv + float2(0,  texel.y)) +
                       SampleScene(uv + float2(0, -texel.y))) * 0.25;
        col += (col - blur) * stylize0.w * 2.0;
    }

    // ===== カラー ステージ =====

    if (mask & E_EXPOSURE)   col *= cg0.x;                       // 露出
    if (mask & E_BRIGHTNESS) col += cg0.z;                       // 明るさ（加算）

    // --- ブルーム（HDR上で3スケールの広く柔らかいハロー）---
    if (mask & E_BLOOM)
    {
        float th = cg1.w;
        float3 b = 0.0;
        float wsum = 0.0;
        [unroll] for (int s = 0; s < 3; ++s)
        {
            float rad = (s + 1.0) * 2.5;     // 2.5 / 5 / 7.5 px
            float w   = 1.0 / (s + 1.0);     // 内側ほど強い
            [unroll] for (int k = 0; k < 8; ++k)
            {
                float ang = (k / 8.0) * 6.2831853;
                float2 o  = float2(cos(ang), sin(ang)) * rad * texel;
                b += max(SampleScene(uv + o) - th, 0.0) * w;
                wsum += w;
            }
            b += max(SampleScene(uv) - th, 0.0) * w;
            wsum += w;
        }
        col += (b / max(wsum, 1.0)) * cg1.z * 4.0;
    }

    if (mask & E_CONTRAST)   col = (col - 0.5) * cg0.y + 0.5;    // コントラスト
    if (mask & E_SATURATION) col = lerp(Luma(col).xxx, col, cg0.w); // 彩度

    // --- 色温度 ---
    if (mask & E_WARMTH)
    {
        col.r += cg1.x * 0.08;
        col.b -= cg1.x * 0.08;
    }

    if (mask & E_HUE)  col = HueRotate(col, radians(cg1.y));     // 色相回転
    if (mask & E_TINT) col *= tintVig.rgb;                       // 色味

    col = max(col, 0.0);

    // --- ポスタライズ ---
    if (mask & E_POSTERIZE)
    {
        float fl = (float)max(masks.y, 2);
        col = floor(col * fl) / max(fl - 1.0, 1.0);
    }

    // --- 順序ディザ ---
    if (mask & E_DITHER)
    {
        float fl = (float)max(masks.z, 2);
        float th = Bayer4(i.uv / texel) - 0.5;
        col = floor(col * fl + th) / max(fl - 1.0, 1.0);
    }

    // --- 色反転 ---
    if (mask & E_INVERT)
        col = lerp(col, 1.0 - col, stylize1.y);

    // --- セピア ---
    if (mask & E_SEPIA)
    {
        float3 sp = float3(
            dot(col, float3(0.393, 0.769, 0.189)),
            dot(col, float3(0.349, 0.686, 0.168)),
            dot(col, float3(0.272, 0.534, 0.131)));
        col = lerp(col, sp, stylize1.z);
    }

    // --- グレースケール ---
    if (mask & E_GRAYSCALE)
        col = lerp(col, Luma(col).xxx, stylize1.w);

    // --- 輪郭線（Sobel エッジ検出） ---
    if (mask & E_OUTLINE)
    {
        float l00 = Luma(SampleScene(uv + float2(-texel.x, -texel.y)));
        float l10 = Luma(SampleScene(uv + float2(0,        -texel.y)));
        float l20 = Luma(SampleScene(uv + float2( texel.x, -texel.y)));
        float l01 = Luma(SampleScene(uv + float2(-texel.x, 0)));
        float l21 = Luma(SampleScene(uv + float2( texel.x, 0)));
        float l02 = Luma(SampleScene(uv + float2(-texel.x,  texel.y)));
        float l12 = Luma(SampleScene(uv + float2(0,         texel.y)));
        float l22 = Luma(SampleScene(uv + float2( texel.x,  texel.y)));
        float gx = -l00 - 2.0*l01 - l02 + l20 + 2.0*l21 + l22;
        float gy = -l00 - 2.0*l10 - l20 + l02 + 2.0*l12 + l22;
        float edge = saturate(sqrt(gx*gx + gy*gy) * outlineP.w);
        col = lerp(col, outlineP.rgb, edge);
    }

    // --- CRT 走査線 ---
    if (mask & E_SCANLINE)
    {
        float scan = sin(luv.y * 240.0 * PI) * 0.5 + 0.5;
        col *= lerp(1.0, scan, stylize0.z * 0.5);
        if (crtOut) col = 0.0;
    }

    // --- フィルムグレイン ---
    if (mask & E_GRAIN)
    {
        float n = Rand(i.uv * (time + 1.0));
        col += (n - 0.5) * stylize1.x * 0.3;
    }

    // --- ビネット ---
    if (mask & E_VIGNETTE)
    {
        float2 d = i.uv - 0.5;
        col *= saturate(1.0 - dot(d, d) * 2.0 * tintVig.w);
    }

    return float4(ACESFilm(col), 1.0);
}
