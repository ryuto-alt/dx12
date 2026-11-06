// SSAOBlur.hlsl - 深度を見る 5x5 バイラテラルブラー（空間ガウス × 線形深度差の指数減衰）。
// 縁をまたいで AO がにじまないようにする。
// VS は SSAO_VS.cso（FSTriVS）を共有するため、ここでは PS のみコンパイルする。
#include "../post/FullscreenTri.hlsli"   // FSQuadVSOut(uv)

Texture2D<float> g_ao         : register(t0);
Texture2D<float> g_depth      : register(t1);
SamplerState     g_pointClamp : register(s0);

cbuffer BlurCB : register(b0)
{
    float4x4 invProj;   // クリップ → ビュー（転置済み: mul(row, invProj)）。線形深度の復元用
    float2   invRTSize; // 1/width, 1/height
    float2   _pad;
};

// 深度バッファ値 → ビュー空間 z（透視/正射とも invProj の z,w だけで決まる）。
float LinearZ(float d)
{
    float4 v = mul(float4(0.0, 0.0, d, 1.0), invProj);
    return v.z / v.w;
}

float PSMain(FSQuadVSOut i) : SV_TARGET
{
    float dC = g_depth.SampleLevel(g_pointClamp, i.uv, 0);
    float aoC = g_ao.SampleLevel(g_pointClamp, i.uv, 0);
    if (dC >= 1.0)
        return aoC;   // 背景はそのまま

    const float zC = LinearZ(dC);
    const float kRelThreshold = 0.05;           // 深度の 5% 差で重み e^-1
    const float kInvTwoSigma2 = 1.0 / (2.0 * 1.5 * 1.5);   // 空間ガウス σ=1.5px

    float sum = 0.0, wsum = 0.0;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    [unroll]
    for (int x = -2; x <= 2; ++x)
    {
        float2 uv = i.uv + float2(x, y) * invRTSize;
        float  d  = g_depth.SampleLevel(g_pointClamp, uv, 0);
        float  a  = g_ao.SampleLevel(g_pointClamp, uv, 0);
        float  wS = exp(-(float)(x * x + y * y) * kInvTwoSigma2);
        float  wD = (d >= 1.0) ? 0.0 : exp(-abs(LinearZ(d) - zC) / (kRelThreshold * max(abs(zC), 1e-4)));
        float  w  = wS * wD;
        sum  += a * w;
        wsum += w;
    }
    return (wsum > 1e-5) ? sum / wsum : aoC;
}
