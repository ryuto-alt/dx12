// TAA.hlsl - Temporal Anti-Aliasing の解決パス。トーンマップ前のリニア HDR に対して行う。
//
//   参考: Karis "High Quality Temporal Supersampling" (UE4)
//         Playdead "Temporal Reprojection AA in INSIDE" (clip_aabb / YCoCg / feedback)
//         Salvi "Temporal Supersampling" (variance clipping)
//         Pettineo (最適化 Catmull-Rom 9tap) / https://alextardif.com/TAA.html
//
// 座標規約: i.uv は「ビューポートローカル 0..1」。フル RT の UV は uv * rectP.zw + rectP.xy。
//           速度バッファも同じビューポートローカル UV 単位で書かれている。
//           velocity = 現UV - 前UV なので、履歴は uv - velocity から読む。
//
// エディタは中央のサブ矩形ビューポート、ゲームは全画面。両方でこの規約に統一してあるので
// 片方だけ壊れることはない。

#include "FullscreenTri.hlsli"

// YCoCg 空間でクリップすると AABB が実際の近傍色分布に密着し RGB より締まる（Playdead/UE4 の標準）。
// 0 にすれば RGB クリップになる。比較用に残してある。
#define TAA_USE_YCOCG 1

cbuffer TaaCB : register(b0)
{
    float4x4 invViewProj;    // 現(非ジッタ) viewProj の逆行列（転置済み）
    float4x4 prevViewProj;   // 前(非ジッタ) viewProj（転置済み）
    float4   rectP;          // xy=UVオフセット, zw=UVスケール（ビューポート矩形）
    float4   texel;          // xy=1/RTW,1/RTH   （zw は 16B 境界のための詰め物）
    float4   rtSize;         // xy=フルRTのpxサイズ（zw は詰め物）
    float4   params;         // x=historyValid y=feedbackMin z=feedbackMax w=varianceGamma
};

Texture2D         gScene    : register(t0);   // 現フレーム HDR（ジッタ込みで描かれている）
Texture2D         gHistory  : register(t1);   // 前フレームの TAA 出力
Texture2D<float2> gVelocity : register(t2);
Texture2D<float>  gDepth    : register(t3);
SamplerState gLinear : register(s0);          // LINEAR CLAMP
SamplerState gPoint  : register(s1);          // POINT  CLAMP

static const float FLT_EPS = 1e-5;

float3 RGB_YCoCg(float3 c)
{
    return float3( c.x * 0.25 + c.y * 0.5 + c.z * 0.25,
                   c.x * 0.5              - c.z * 0.5,
                  -c.x * 0.25 + c.y * 0.5 - c.z * 0.25);
}
float3 YCoCg_RGB(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Playdead INSIDE の clip_aabb（最適化版）。履歴を近傍 AABB へ「クリップ」する。
// clamp と違い色相を保ったまま境界へ寄せるので、ゴーストの色ズレが出にくい。
float3 ClipAABB(float3 aabbMin, float3 aabbMax, float3 q)
{
    float3 pClip = 0.5 * (aabbMax + aabbMin);
    float3 eClip = 0.5 * (aabbMax - aabbMin) + FLT_EPS;
    float3 vClip = q - pClip;
    float3 aUnit = abs(vClip / eClip);
    float  maUnit = max(aUnit.x, max(aUnit.y, aUnit.z));
    return (maUnit > 1.0) ? (pClip + vClip / maUnit) : q;
}

// Pettineo の最適化 Catmull-Rom（9タップ・バイリニア併用）。
// 履歴をバイリニアで読むとフレームごとにボケが累積して画面全体が甘くなるため必須。
// uvMin/uvMax でビューポート矩形の外を読まないようにクランプする
// （履歴 RT はフル RT サイズで、矩形外には前のフレームの残骸が居る）。
float3 SampleHistoryCatmullRom(float2 uvFull, float2 uvMin, float2 uvMax)
{
    const float2 size    = rtSize.xy;
    const float2 invSize = texel.xy;

    float2 samplePos = uvFull * size;
    float2 texPos1   = floor(samplePos - 0.5) + 0.5;
    float2 f  = samplePos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12      = w1 + w2;
    float2 offset12 = w2 / max(w12, 1e-6);

    float2 texPos0  = (texPos1 - 1.0) * invSize;
    float2 texPos3  = (texPos1 + 2.0) * invSize;
    float2 texPos12 = (texPos1 + offset12) * invSize;

    texPos0  = clamp(texPos0,  uvMin, uvMax);
    texPos3  = clamp(texPos3,  uvMin, uvMax);
    texPos12 = clamp(texPos12, uvMin, uvMax);

    float3 r = 0;
    r += gHistory.SampleLevel(gLinear, float2(texPos0.x,  texPos0.y),  0).rgb * w0.x  * w0.y;
    r += gHistory.SampleLevel(gLinear, float2(texPos12.x, texPos0.y),  0).rgb * w12.x * w0.y;
    r += gHistory.SampleLevel(gLinear, float2(texPos3.x,  texPos0.y),  0).rgb * w3.x  * w0.y;
    r += gHistory.SampleLevel(gLinear, float2(texPos0.x,  texPos12.y), 0).rgb * w0.x  * w12.y;
    r += gHistory.SampleLevel(gLinear, float2(texPos12.x, texPos12.y), 0).rgb * w12.x * w12.y;
    r += gHistory.SampleLevel(gLinear, float2(texPos3.x,  texPos12.y), 0).rgb * w3.x  * w12.y;
    r += gHistory.SampleLevel(gLinear, float2(texPos0.x,  texPos3.y),  0).rgb * w0.x  * w3.y;
    r += gHistory.SampleLevel(gLinear, float2(texPos12.x, texPos3.y),  0).rgb * w12.x * w3.y;
    r += gHistory.SampleLevel(gLinear, float2(texPos3.x,  texPos3.y),  0).rgb * w3.x  * w3.y;
    return max(r, 0.0);   // Catmull-Rom は負に振れるので HDR では必ずクランプ（黒点対策）
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 TaaResolvePS(FSQuadVSOut i) : SV_TARGET
{
    const float2 luv    = i.uv;                       // ビューポートローカル
    const float2 uvFull = luv * rectP.zw + rectP.xy;  // フル RT
    const float2 texelF = texel.xy;                   // フル RT のテクセル

    // 履歴/近傍サンプルがビューポート矩形の外へ出ないためのクランプ範囲（半テクセル内側）。
    const float2 uvMin = rectP.xy + texelF * 0.5;
    const float2 uvMax = rectP.xy + rectP.zw - texelF * 0.5;

    // ---- 速度の取得 ----
    float depth = gDepth.SampleLevel(gPoint, clamp(uvFull, uvMin, uvMax), 0);
    float2 vel;
    if (depth >= 1.0 - 1e-6)
    {
        // スカイボックス/未描画領域: 速度パスで描かれていないのでカメラ運動から復元する
        // （MotionBlur.hlsl と同じ深度再投影。空も TAA でアンチエイリアスされる）。
        float2 ndc   = float2(luv.x * 2.0 - 1.0, 1.0 - luv.y * 2.0);
        float4 world = mul(invViewProj, float4(ndc, depth, 1.0));
        world /= (abs(world.w) > 1e-6 ? world.w : 1e-6);
        float4 pc     = mul(prevViewProj, world);
        float2 prevNd = pc.xy / max(abs(pc.w), 1e-6);
        float2 prevUv = float2(prevNd.x * 0.5 + 0.5, 0.5 - prevNd.y * 0.5);
        vel = luv - prevUv;
    }
    else
    {
        // closest-depth velocity dilation: 3x3 のうち最も手前(深度最小)の速度を採る。
        // 輪郭のディスオクルージョンに強くなる定石。
        float2 bestOfs = 0; float bestDepth = depth;
        [unroll] for (int y = -1; y <= 1; ++y)
        {
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                float2 o = float2(x, y) * texelF;
                float  d = gDepth.SampleLevel(gPoint, clamp(uvFull + o, uvMin, uvMax), 0);
                if (d < bestDepth) { bestDepth = d; bestOfs = o; }
            }
        }
        vel = gVelocity.SampleLevel(gPoint, clamp(uvFull + bestOfs, uvMin, uvMax), 0);
    }

    // ---- 現フレームの色と 3x3 近傍統計 ----
    float3 m1 = 0, m2 = 0;
    float3 nmin =  1e30, nmax = -1e30;
    float3 center = 0;
    [unroll] for (int yy = -1; yy <= 1; ++yy)
    {
        [unroll] for (int xx = -1; xx <= 1; ++xx)
        {
            float3 c = gScene.SampleLevel(gPoint, clamp(uvFull + float2(xx, yy) * texelF, uvMin, uvMax), 0).rgb;
            c = max(c, 0.0);
#if TAA_USE_YCOCG
            c = RGB_YCoCg(c);
#endif
            if (xx == 0 && yy == 0) center = c;
            m1 += c; m2 += c * c;
            nmin = min(nmin, c); nmax = max(nmax, c);
        }
    }
    const float3 mu    = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mu * mu, 0.0));
    const float  gamma = params.w;
    // variance clipping (Salvi) と 3x3 min/max の交差を採る。
    // min/max だけだとゴーストが残り、variance だけだと薄いエッジで暴れる。
    float3 cmin = max(mu - gamma * sigma, nmin);
    float3 cmax = min(mu + gamma * sigma, nmax);

    // ---- 履歴のサンプルとクリップ ----
    float2 histLuv = luv - vel;
    // ビューポート外へ出たら履歴を捨てる（ディスオクルージョン/画面端）
    bool histOk = params.x > 0.5 && all(histLuv > 0.0) && all(histLuv < 1.0);
    float2 histUvFull = histLuv * rectP.zw + rectP.xy;

    float3 hist = SampleHistoryCatmullRom(histUvFull, uvMin, uvMax);
#if TAA_USE_YCOCG
    hist = RGB_YCoCg(hist);
#endif
    hist = ClipAABB(cmin, cmax, hist);

    // ---- ブレンド ----
    // ① 輝度差ベースの feedback 可変（Playdead）: 履歴と現在が食い違うほど履歴の比率を下げる
#if TAA_USE_YCOCG
    float lumC = center.x, lumH = hist.x;
#else
    float lumC = Luma(center), lumH = Luma(hist);
#endif
    float diff = abs(lumC - lumH) / max(lumC, max(lumH, 0.2));
    float w    = 1.0 - diff;
    float feedback = lerp(params.y, params.z, saturate(w * w));
    if (!histOk) feedback = 0.0;

    // ② firefly 対策: 1/(1+luma) の重み付き平均（可逆トーンマップ相当）。
    //    「トーンマップしてから平均して逆トーンマップする」のと等価で、
    //    高輝度サンプルの寄与を下げてから合成する（Karis / AMD GPUOpen）。
    float wc = (1.0 - feedback) / (1.0 + max(lumC, 0.0));
    float wh = feedback         / (1.0 + max(lumH, 0.0));
    float3 outc = (center * wc + hist * wh) / max(wc + wh, 1e-5);

#if TAA_USE_YCOCG
    outc = YCoCg_RGB(outc);
#endif
    return float4(max(outc, 0.0), 1.0);
}
