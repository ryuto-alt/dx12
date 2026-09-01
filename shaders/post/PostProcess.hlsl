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
#define E_AUTOEXP    (1u<<25)
#define E_LUT        (1u<<26)
#define E_DEBAND     (1u<<27)
#define E_GODRAYS    (1u<<28)
#define E_LENSFLARE  (1u<<29)
#define E_DISTORT    (1u<<30)

cbuffer PostCB : register(b0)
{
    float4 uvOffsetScale;   // xy=UVオフセット, zw=UVスケール（ビューポート矩形対応）
    float4 texelTime;       // xy=テクセル(1/W,1/H), z=time
    int4   masks;           // x=enableMask, y=posterizeLevels, z=ditherLevels, w=tonemapper(0=ACES,1=AgX,2=なし)
    float4 cg0;             // exposure, contrast, brightness, saturation
    float4 cg1;             // warmth, hueShift(度), bloom, bloomThreshold
    float4 tintVig;         // tint.rgb, vignette
    float4 stylize0;        // chromatic, pixelSize, scanline, sharpen
    float4 stylize1;        // grain, invert, sepia, grayscale
    float4 dist0;           // lens, radial, glitch, _
    float4 wave;            // waveAmp, waveFreq, waveSpeed, _
    float4 outlineP;        // outlineColor.rgb, outlineStrength
    float4 extra0;          // lutSize, lutAmount, _, _
    // ── ここから下は「1 エフェクト 1 スライダー」を卒業するために足した詳細パラメータ ──
    float4 vig2;            // vignetteRadius, vignetteSoftness, vignetteRoundness, chromaMode
    float4 vigCol;          // vignetteColor.rgb, scanCount
    float4 lens2;           // lensMode, lensK2, lensZoom, lensFlags(bit0=円形補正, bit1..2=はみ出し処理)
    float4 lens3;           // lensChroma, scanCurve, glitchBlocks, glitchSpeed
    float4 misc0;           // glitchColor, grainSize, grainColored, radialSamples
    float4 misc1;           // radialCenterX, radialCenterY, outlineThickness, outlineThreshold
    float4 outl2;           // outlineBg.rgb, outlineOnly
};

Texture2D    gScene : register(t0);
Texture2D    gBloom : register(t1);   // BloomPass の結果（ビューポートローカル 0..1）
Texture2D    gLut   : register(t2);   // 3D LUT ストリップ（N*N x N）。無効時は白ダミー
StructuredBuffer<float> gExposureBuf : register(t3);  // [0]=自動露出の倍率
Texture2D    gGodrays : register(t4); // ゴッドレイ光条（シーンと同じ正規化UVレイアウト）
Texture2D    gFlare   : register(t5); // レンズフレア（ビューポートローカル 0..1）
Texture2D    gDistort : register(t6); // パーティクル歪みバッファ（RG=UVオフセット、シーンと同レイアウト）
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

// 鏡映ラップ（0..1 の外へ出た UV を折り返す）。lensEdge=2 用。
static float2 MirrorUV(float2 uv)
{
    float2 t = frac(abs(uv) * 0.5) * 2.0;
    return 1.0 - abs(1.0 - t);
}

// シーンRT にはリニア HDR が入っている（forward/skybox はトーンマップせず出力）。
// HDR 空間で行うべき処理（露出・ブルーム合成）の後、ここで表示変換（トーンマップ+ガンマ）を
// 一括適用し、以降の色補正・スタイライズ系は表示基準(LDR)の色に対して行う。
static float3 ACESFilm(float3 x)
{
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// AgX（Troy Sobotka / Blender 4.0 採用。高輝度・高彩度光源の色相スキューがなく白へ滑らかに転がる）
// Benjamin Wrensch "Minimal AgX Implementation" のフィット版。
// 行列は GLSL(列優先) から転置済み。最終の pow(2.2)（EOTF）と本エンジンの pow(1/2.2) は
// 相殺されるため両方省略し、ガンマ空間の値を直接返す。
static float3 AgXContrast(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4
         - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
static float3 TonemapAgX(float3 val)
{
    const float3x3 agxMat = float3x3(
        0.842479062253094,  0.0784335999999992, 0.0792237451477643,
        0.0423282422610123, 0.878468636469772,  0.0791661274605434,
        0.0423756549057051, 0.0784336,          0.879142973793104);
    const float3x3 agxMatInv = float3x3(
         1.19687900512017,   -0.0980208811401368, -0.0990297440797205,
        -0.0528968517574562,  1.15190312990417,   -0.0989611768448433,
        -0.0529716355144438, -0.0980434501171241,  1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;

    val = mul(agxMat, max(val, 0.0));
    val = clamp(log2(val), minEv, maxEv);
    val = (val - minEv) / (maxEv - minEv);
    val = AgXContrast(val);
    val = mul(agxMatInv, val);
    return saturate(val);
}

// 表示変換（リニアHDR → ガンマ空間 LDR）。masks.w で切替。
static float3 ToneMapGamma(float3 c)
{
    int tm = masks.w;
    if (tm == 1) return TonemapAgX(c);
    if (tm == 2) return pow(max(c, 0.0), 1.0 / 2.2);  // トーンマップなし（ガンマのみ）
    return pow(ACESFilm(c), 1.0 / 2.2);
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

// ─────────────────────────────────────────────────────────────────────────────
// レンズ歪み / 魚眼の逆写像。
//   出力ピクセルの正規化半径 r（画面中心=0 / 短辺の端=1）から、元のパース画像を
//   どの半径でサンプルするか rIn を返す。r で割った「倍率」を返す（r=0 で 1 に収束）。
//
//   ★旧実装は d*(1+k*r^2*1.5) を【アスペクトを無視した UV 空間】で掛けていたので、
//     16:9 では横だけ強く歪んで円が楕円に潰れた（＝「魚眼がおかしい」の正体）。
//     さらに範囲外は端の 1px が引き伸ばされるだけで、魚眼の丸い像にならなかった。
// ─────────────────────────────────────────────────────────────────────────────
static float LensRadiusScale(float r, int mode, float k, float k2)
{
    if (mode == 0)
    {
        // 多項式（Brown-Conrady 風）。k>0 で樽（画角が広がる）/ k<0 で糸巻き。
        float r2 = r * r;
        return 1.0 + k * r2 + k2 * r2 * r2;
    }

    // 魚眼。k を「最大半画角」へ写す（k=1 で 85 度近く＝ど魚眼）。
    // 出力半径 r に対応する画角 theta を作り、パース画像上の半径 tan(theta)/tan(thetaMax) を返す。
    float thetaMax = clamp(abs(k), 0.001, 1.0) * (PI * 0.5 - 0.05);
    float theta = (mode == 1)
        ? r * thetaMax                                   // 等距離射影（f*theta）
        : 2.0 * asin(saturate(r * sin(thetaMax * 0.5))); // 等立体角射影（2f*sin(theta/2)）
    float denom = max(tan(thetaMax), 1e-5);
    float rIn = tan(theta) / denom;
    // k<0 の魚眼は「逆魚眼」として扱う（半径の写像を反転）。
    if (k < 0.0) rIn = (r > 1e-5) ? (r * r / max(rIn, 1e-5)) : r;
    return (r > 1e-5) ? (rIn / r) : 1.0;
}

float4 PostPS(FSQuadVSOut i) : SV_TARGET
{
    float2 scale = uvOffsetScale.zw;
    float2 ofs   = uvOffsetScale.xy;
    float2 texel = texelTime.xy;
    float  time  = texelTime.z;
    uint   mask  = (uint)masks.x;
    // 画面のアスペクト比（W/H）。テクセルサイズから出せるので CB を増やさない。
    float  aspect = texel.y / max(texel.x, 1e-8);

    // マスター無効（全ビット 0）= エフェクト無し。ただしシーンRTはリニアHDRなので
    // トーンマップ+ガンマだけは必ず適用して表示用カラーにする。
    if (mask == 0u)
        return float4(ToneMapGamma(SampleScene(i.uv * scale + ofs)), 1.0);

    float2 luv    = i.uv;          // ビューポートローカル 0..1
    bool   crtOut = false;
    bool   lensOut = false;
    // 3 タップの色収差オフセット（レンズの倍率色収差と Chromatic をここに集約する。
    // ★旧実装は Chromatic と FXAA が else-if で排他になっていて、
    //   色収差を ON にすると FXAA が【無言で消えていた】）。
    float2 caOfs = float2(0.0, 0.0);

    // ===== UV ステージ（サンプル前に UV を歪ませる） =====

    // --- パーティクル歪み（熱ゆらぎ/衝撃波。歪みバッファの RG=オフセット）---
    if (mask & E_DISTORT)
    {
        float2 dofs = gDistort.Sample(gSamp, i.uv * scale + ofs).rg;
        luv += dofs;
    }

    // --- レンズ歪み / 魚眼 ---
    if (mask & E_LENS)
    {
        int   lmode  = (int)lens2.x;
        float k      = dist0.x;
        float k2     = lens2.y;
        float zoom   = max(lens2.z, 0.01);
        uint  lflags = (uint)lens2.w;
        bool  circular = (lflags & 1u) != 0u;
        int   edge     = (int)((lflags >> 1) & 3u);

        // 中心からのベクトルを「短辺基準の正方座標」へ持ち上げる＝歪みが円形になる。
        float2 d = (luv - 0.5) * 2.0;
        if (circular) d.x *= aspect;

        float r  = length(d);
        float sc = LensRadiusScale(r, lmode, k, k2) / zoom;

        float2 dOut = d * sc;
        if (circular) dOut.x /= aspect;
        luv = dOut * 0.5 + 0.5;

        // 倍率色収差: 歪みの強さに比例して R/B の倍率をずらす（本物のレンズ味）。
        if (lens3.x > 0.0001)
        {
            float2 dCa = d * (sc * lens3.x * 0.02 * r);
            if (circular) dCa.x /= aspect;
            caOfs += dCa * 0.5;
        }

        // はみ出しの扱い。0=端を引き伸ばす(サンプラの clamp 任せ) / 1=黒 / 2=鏡映
        if (edge == 1)
        {
            if (luv.x < 0.0 || luv.x > 1.0 || luv.y < 0.0 || luv.y > 1.0) lensOut = true;
        }
        else if (edge == 2)
        {
            luv = MirrorUV(luv);
        }
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
        luv += d * dot(d, d) * lens3.y;
        if (luv.x < 0.0 || luv.x > 1.0 || luv.y < 0.0 || luv.y > 1.0)
            crtOut = true;
    }

    // --- デジタルグリッチ（行ブロックを横ずらし） ---
    if (mask & E_GLITCH)
    {
        float blocks = max(lens3.z, 1.0);
        float speed  = max(lens3.w, 0.0);
        float blockY = floor(luv.y * blocks);
        float n = Rand(float2(blockY, floor(time * speed)));
        if (n > 0.8)
        {
            float shift = (Rand(float2(blockY, floor(time * speed) + 7.0)) - 0.5) * dist0.z * 0.3;
            luv.x += shift;
            // RGB 分離（グリッチらしい色ズレ）。3 タップの共通オフセットに足す。
            caOfs.x += shift * misc0.x;
        }
    }

    float2 uv = luv * scale + ofs;

    // --- ピクセル化 ---
    if (mask & E_PIXELIZE)
    {
        float2 block = max(stylize0.y, 1.0) * texel;
        uv = (floor(uv / block) + 0.5) * block;
    }

    // ===== サンプル ステージ =====

    // --- 色収差（放射 / 水平 / 垂直）---
    if (mask & E_CHROMATIC)
    {
        int cmode = (int)vig2.w;
        float amt = stylize0.x * 0.03;
        if (cmode == 1)      caOfs += float2(amt, 0.0);
        else if (cmode == 2) caOfs += float2(0.0, amt);
        else                 caOfs += (luv - 0.5) * amt;
    }

    float3 col;
    if (any(abs(caOfs) > 1e-6))
    {
        col.r = SampleScene(uv + caOfs).r;
        col.g = SampleScene(uv).g;
        col.b = SampleScene(uv - caOfs).b;
    }
    else
    {
        col = SampleScene(uv);
    }

    // --- FXAA（色収差とは独立に効く。旧実装は排他だった）---
    if (mask & E_FXAA)
    {
        float3 n = SampleScene(uv + float2(0, -texel.y));
        float3 s = SampleScene(uv + float2(0,  texel.y));
        float3 e = SampleScene(uv + float2( texel.x, 0));
        float3 w = SampleScene(uv + float2(-texel.x, 0));
        float edge  = abs(Luma(n) - Luma(s)) + abs(Luma(e) - Luma(w));
        col = lerp(col, (col + n + s + e + w) * 0.2, saturate(edge * 2.0));
    }

    // --- 放射ブラー（任意の中心へズーム） ---
    if (mask & E_RADIAL)
    {
        int   taps = clamp((int)misc0.w, 2, 32);
        float2 ctr = float2(misc1.x, misc1.y);
        float2 dir = (luv - ctr);
        float3 acc = col;
        for (int k = 1; k <= taps; ++k)
        {
            float t = (float)k / (float)taps;
            acc += SampleScene(uv - dir * t * dist0.y * 0.15);
        }
        col = acc / (float)(taps + 1);
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

    // ===== HDR ステージ（トーンマップ前・リニア輝度に対して行う） =====

    if (mask & E_AUTOEXP)    col *= gExposureBuf[0];             // 自動露出（GPU内で完結）
    if (mask & E_EXPOSURE)   col *= cg0.x;                       // 露出（手動・自動の上乗せ可）

    // --- ブルーム（BloomPass のダウン/アップサンプルチェーン結果を合成）---
    // トーンマップ前のリニアHDRで合成するので、光源・発光体の輝度エネルギー(>1)が正しく咲く。
    // gBloom はビューポートローカル 0..1（UVエフェクト後の luv でサンプルして歪みと整合させる）
    if (mask & E_BLOOM)
        col += gBloom.Sample(gSamp, saturate(luv)).rgb * cg1.z;

    // --- ゴッドレイ（強度焼き込み済み・シーンと同レイアウトなので uv でサンプル）---
    if (mask & E_GODRAYS)
        col += gGodrays.Sample(gSamp, uv).rgb;

    // --- レンズフレア（強度焼き込み済み・ローカル 0..1）---
    if (mask & E_LENSFLARE)
        col += gFlare.Sample(gSamp, saturate(luv)).rgb;

    // ===== トーンマップ（ACES）+ ガンマ =====
    // ここから先は表示基準(LDR, 0..1)の色として扱う
    col = ToneMapGamma(col);

    // ===== カラー ステージ（LDR） =====

    if (mask & E_BRIGHTNESS) col += cg0.z;                       // 明るさ（加算）
    if (mask & E_CONTRAST)   col = (col - 0.5) * cg0.y + 0.5;    // コントラスト
    if (mask & E_SATURATION) col = lerp(Luma(col).xxx, col, cg0.w); // 彩度

    // --- 色温度 ---
    // ★旧実装のゲインは ±0.08 しかなく、スライダーを端まで振っても
    //   ほとんど見た目が変わらなかった（「効かない」と言われていた項目）。
    if (mask & E_WARMTH)
    {
        col.r += cg1.x * 0.16;
        col.g += cg1.x * 0.03;
        col.b -= cg1.x * 0.16;
    }

    if (mask & E_HUE)  col = HueRotate(col, radians(cg1.y));     // 色相回転
    if (mask & E_TINT) col *= tintVig.rgb;                       // 色味

    col = max(col, 0.0);

    // --- 3D LUT カラーグレーディング（ストリップ形式 N*N x N）---
    // トーンマップ後の LDR に適用。青チャンネルでスライスを選び、2 スライスを補間。
    if (mask & E_LUT)
    {
        float  sz    = max(extra0.x, 2.0);
        float3 base  = saturate(col);
        float  lscale = (sz - 1.0) / sz;
        float  ofsL  = 0.5 / sz;
        float  b      = base.b * (sz - 1.0);
        float  slice0 = floor(b);
        float  f      = b - slice0;
        float  slice1 = min(slice0 + 1.0, sz - 1.0);
        float  inX    = base.r * lscale + ofsL;     // スライス内 0..1
        float  y      = base.g * lscale + ofsL;
        float3 l0 = gLut.Sample(gSamp, float2((inX + slice0) / sz, y)).rgb;
        float3 l1 = gLut.Sample(gSamp, float2((inX + slice1) / sz, y)).rgb;
        col = lerp(col, lerp(l0, l1, f), extra0.y);
    }

    // --- ポスタライズ ---
    // ★旧実装は floor(col*N)/(N-1) で、白(1.0)が N/(N-1) = 1.2 に【増光】していた。
    //   N 段の量子化は floor(col*(N-1)+0.5)/(N-1) が正しい（0 と 1 が保存される）。
    if (mask & E_POSTERIZE)
    {
        float fl = (float)max(masks.y, 2) - 1.0;
        col = floor(saturate(col) * fl + 0.5) / fl;
    }

    // --- 順序ディザ ---
    if (mask & E_DITHER)
    {
        float fl = (float)max(masks.z, 2) - 1.0;
        float th = Bayer4(i.uv / texel);
        col = floor(saturate(col) * fl + th) / fl;
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

    // --- 輪郭線（Sobel エッジ検出。太さ / しきい値 / 線画モード）---
    if (mask & E_OUTLINE)
    {
        float2 t = texel * max(misc1.z, 0.1);
        float l00 = Luma(SampleScene(uv + float2(-t.x, -t.y)));
        float l10 = Luma(SampleScene(uv + float2(0,    -t.y)));
        float l20 = Luma(SampleScene(uv + float2( t.x, -t.y)));
        float l01 = Luma(SampleScene(uv + float2(-t.x, 0)));
        float l21 = Luma(SampleScene(uv + float2( t.x, 0)));
        float l02 = Luma(SampleScene(uv + float2(-t.x,  t.y)));
        float l12 = Luma(SampleScene(uv + float2(0,     t.y)));
        float l22 = Luma(SampleScene(uv + float2( t.x,  t.y)));
        float gx = -l00 - 2.0*l01 - l02 + l20 + 2.0*l21 + l22;
        float gy = -l00 - 2.0*l10 - l20 + l02 + 2.0*l12 + l22;
        // ルマ勾配はリニアHDRのシーンから取る（暗部の勾配が小さいのでゲインで明瞭化）
        float g = sqrt(gx*gx + gy*gy);
        float edge = saturate(max(g - misc1.w, 0.0) * outlineP.w * 4.0);
        float3 baseCol = (outl2.w > 0.5) ? outl2.rgb : col;   // 線画モードは下地を塗り潰す
        col = lerp(baseCol, outlineP.rgb, edge);
    }

    // --- CRT 走査線 ---
    if (mask & E_SCANLINE)
    {
        float scan = sin(luv.y * max(vigCol.w, 1.0) * PI) * 0.5 + 0.5;
        col *= lerp(1.0, scan, stylize0.z);
        if (crtOut) col = 0.0;
    }

    // --- フィルムグレイン ---
    if (mask & E_GRAIN)
    {
        float2 gp = i.uv / (texel * max(misc0.y, 0.25));   // 粒サイズ(px)
        gp = floor(gp);
        if (misc0.z > 0.5)
        {
            float3 n3 = float3(Rand(gp + time * 1.1),
                               Rand(gp + time * 1.7 + 13.0),
                               Rand(gp + time * 2.3 + 71.0));
            col += (n3 - 0.5) * stylize1.x * 0.3;
        }
        else
        {
            float n = Rand(gp + frac(time));
            col += (n - 0.5) * stylize1.x * 0.3;
        }
    }

    // --- ビネット（半径 / 柔らかさ / 真円度 / 色）---
    // ★旧実装は 1 - dot(d,d)*2*strength の一発で、半径も境界も固定だった
    //   （濃くすると画面中央まで一様に暗くなるだけで「四隅だけ落とす」が作れない）。
    if (mask & E_VIGNETTE)
    {
        float2 d = (i.uv - 0.5) * 2.0;
        float2 e = float2(lerp(1.0, aspect, saturate(vig2.z)), 1.0);
        float  r = length(d * e) / max(length(e), 1e-5);   // 中心 0 / 四隅 1
        float  soft = max(vig2.y, 0.001);
        float  v = 1.0 - smoothstep(vig2.x - soft, vig2.x + soft, r);
        col = lerp(vigCol.rgb, col, lerp(1.0, v, saturate(tintVig.w)));
    }

    // --- レンズ歪みのはみ出しを黒で塗る（lensEdge=1）---
    if (lensOut) col = 0.0;

    // --- デバンディング（TPDF ディザ）---
    // 三角分布ノイズ ±0.5/255 を最終 8bit 量子化の直前に加え、空・ビネットの縞を消す。
    if (mask & E_DEBAND)
    {
        float r1 = Rand(i.uv + frac(time * 0.1301));
        float r2 = Rand(i.uv * 1.3719 + frac(time * 0.7177));
        col += (r1 + r2 - 1.0) * (1.0 / 255.0);
    }

    // トーンマップは上のカラーステージ冒頭で適用済み。
    // R8G8B8A8_UNORM へ書くので、効果でレンジ外へ出た値は saturate で [0,1] にクランプ。
    return float4(saturate(col), 1.0);
}
