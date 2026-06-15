// シーンをサンプルしてポストエフェクトを適用し、バックバッファへ出力する単一パス。
// 露出 / コントラスト / 彩度 / 色味 / ビネット / 簡易ブルーム / グレースケール / 簡易FXAA。

#include "FullscreenTri.hlsli"

cbuffer PostCB : register(b0)
{
    float4 uvOffsetScale;   // xy = UV オフセット, zw = UV スケール（エディタのビューポート矩形対応）
    float4 texelExposure;   // xy = テクセルサイズ(1/W,1/H), z = exposure, w = contrast
    float4 satTint;         // x = saturation, yzw = tint
    float4 vigBloom;        // x = vignette, y = bloom, z = bloomThreshold, w = 未使用
    int4   flags;           // x = fxaa, y = grayscale, z = enabled, w = 未使用
};

Texture2D    gScene : register(t0);
SamplerState gSamp  : register(s0);

static float3 SampleScene(float2 uv) { return gScene.Sample(gSamp, uv).rgb; }
static float  Luma(float3 c)         { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 PostPS(FSQuadVSOut i) : SV_TARGET
{
    float2 uv = i.uv * uvOffsetScale.zw + uvOffsetScale.xy;

    // 無効時は素通し
    if (flags.z == 0)
        return float4(SampleScene(uv), 1.0);

    float2 texel    = texelExposure.xy;
    float  exposure = texelExposure.z;
    float  contrast = texelExposure.w;
    float  sat      = satTint.x;
    float3 tint     = satTint.yzw;

    float3 col;

    // --- 簡易FXAA（近傍4タップの輝度差でブラー） ---
    if (flags.x != 0)
    {
        float3 c  = SampleScene(uv);
        float3 n  = SampleScene(uv + float2(0, -texel.y));
        float3 s  = SampleScene(uv + float2(0,  texel.y));
        float3 e  = SampleScene(uv + float2( texel.x, 0));
        float3 w  = SampleScene(uv + float2(-texel.x, 0));
        float edge = abs(Luma(n) - Luma(s)) + abs(Luma(e) - Luma(w));
        float blend = saturate(edge * 2.0);
        col = lerp(c, (c + n + s + e + w) * 0.2, blend);
    }
    else
    {
        col = SampleScene(uv);
    }

    // --- 露出 ---
    col *= exposure;

    // --- 簡易ブルーム（明部を近傍ブラーして加算） ---
    if (vigBloom.y > 0.0)
    {
        float th = vigBloom.z;
        float3 b = 0.0;
        [unroll] for (int x = -2; x <= 2; ++x)
        [unroll] for (int y = -2; y <= 2; ++y)
        {
            float3 s = SampleScene(uv + float2(x, y) * texel * 1.5);
            b += max(s - th, 0.0);
        }
        b /= 25.0;
        col += b * vigBloom.y * 3.0;
    }

    // --- コントラスト ---
    col = (col - 0.5) * contrast + 0.5;

    // --- 彩度 ---
    col = lerp(Luma(col).xxx, col, sat);

    // --- 色味 ---
    col *= tint;

    // --- グレースケール ---
    if (flags.y != 0)
        col = Luma(col).xxx;

    // --- ビネット（ビューポート中心からの距離で減光） ---
    if (vigBloom.x > 0.0)
    {
        float2 d = i.uv - 0.5;
        float r = saturate(1.0 - dot(d, d) * 2.0 * vigBloom.x);
        col *= r;
    }

    return float4(saturate(col), 1.0);
}
