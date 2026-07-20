// 物理ベースブルーム（CoD:Advanced Warfare / Jimenez 2014 方式）。
// ダウンサンプル: 13タップ（重なり合う 2x2 ボックス 5 個の加重平均）。
//   初段のみ Karis average（輝度重み）でファイアフライを抑え、ソフトニーしきい値で抽出。
// アップサンプル: 9タップ テントフィルタ。合成は PSO のブレンドファクタ
//   （dest = src*radius + dest*(1-radius)）で行う。
// 参考: Froyok "Custom Bloom Post-Process" / LearnOpenGL "Phys. Based Bloom"

#include "FullscreenTri.hlsli"

cbuffer BloomCB : register(b0)
{
    float4 srcRect;  // xy=UVオフセット, zw=UVスケール（初段のシーンRTサブ矩形。以降は 0,0,1,1）
    float4 texelP;   // xy=ソースのテクセルサイズ, z=初段フラグ(1/0), w=未使用
    float4 filt;     // x=しきい値, y=ニー, z=未使用, w=未使用
};

Texture2D    gSrc  : register(t0);
SamplerState gSamp : register(s0);

static float3 SampleSrc(float2 uv)
{
    // サブ矩形の外（クリア色など）を拾わないよう 0..1 でクランプしてから写像
    uv = clamp(uv, 0.0, 1.0);
    return gSrc.Sample(gSamp, srcRect.xy + uv * srcRect.zw).rgb;
}

static float Luma(float3 c) { return dot(c, float3(0.2127, 0.7152, 0.0722)); }

// ソフトニーしきい値（Unity PostProcessing 方式）
static float3 Prefilter(float3 c)
{
    float threshold = filt.x;
    float knee      = filt.y;
    float br   = max(c.r, max(c.g, c.b));
    float soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float contrib = max(soft, br - threshold) / max(br, 1e-4);
    return c * contrib;
}

// ---- ダウンサンプル（13タップ）----
float4 BloomDownPS(FSQuadVSOut i) : SV_TARGET
{
    float2 t = texelP.xy;
    float2 uv = i.uv;

    // a b c
    //  j k
    // d e f
    //  l m
    // g h i
    float3 a = SampleSrc(uv + t * float2(-2, -2));
    float3 b = SampleSrc(uv + t * float2( 0, -2));
    float3 c = SampleSrc(uv + t * float2( 2, -2));
    float3 d = SampleSrc(uv + t * float2(-2,  0));
    float3 e = SampleSrc(uv);
    float3 f = SampleSrc(uv + t * float2( 2,  0));
    float3 g = SampleSrc(uv + t * float2(-2,  2));
    float3 h = SampleSrc(uv + t * float2( 0,  2));
    float3 i2 = SampleSrc(uv + t * float2( 2,  2));
    float3 j = SampleSrc(uv + t * float2(-1, -1));
    float3 k = SampleSrc(uv + t * float2( 1, -1));
    float3 l = SampleSrc(uv + t * float2(-1,  1));
    float3 m = SampleSrc(uv + t * float2( 1,  1));

    float3 col;
    if (texelP.z > 0.5)
    {
        // 初段: 2x2 ボックス 5 個を Karis average（1/(1+luma) 重み）で混ぜて
        // 高輝度の孤立ピクセル（ファイアフライ）のちらつきを抑える。
        float3 g0 = (a + b + d + e) * 0.25;
        float3 g1 = (b + c + e + f) * 0.25;
        float3 g2 = (d + e + g + h) * 0.25;
        float3 g3 = (e + f + h + i2) * 0.25;
        float3 g4 = (j + k + l + m) * 0.25;
        float w0 = 0.125 / (1.0 + Luma(g0));
        float w1 = 0.125 / (1.0 + Luma(g1));
        float w2 = 0.125 / (1.0 + Luma(g2));
        float w3 = 0.125 / (1.0 + Luma(g3));
        float w4 = 0.5   / (1.0 + Luma(g4));
        col = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
        col = Prefilter(col);
    }
    else
    {
        col = e * 0.125
            + (a + c + g + i2) * 0.03125
            + (b + d + f + h)  * 0.0625
            + (j + k + l + m)  * 0.125;
    }
    return float4(col, 1.0);
}

// ---- アップサンプル（9タップ テント）----
// 出力先ミップへの合成は PSO のブレンド（BLEND_FACTOR=radius）で行うので、ここはフィルタのみ。
float4 BloomUpPS(FSQuadVSOut i) : SV_TARGET
{
    float2 t = texelP.xy;
    float2 uv = i.uv;

    float3 col = SampleSrc(uv) * 4.0;
    col += (SampleSrc(uv + t * float2(-1,  0)) +
            SampleSrc(uv + t * float2( 1,  0)) +
            SampleSrc(uv + t * float2( 0, -1)) +
            SampleSrc(uv + t * float2( 0,  1))) * 2.0;
    col += (SampleSrc(uv + t * float2(-1, -1)) +
            SampleSrc(uv + t * float2( 1, -1)) +
            SampleSrc(uv + t * float2(-1,  1)) +
            SampleSrc(uv + t * float2( 1,  1)));
    return float4(col / 16.0, 1.0);
}
