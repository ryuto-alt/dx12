// SSAOBlur.hlsl - 4x4 ボックスブラーで SSAO のバンディングを除去する。
// VS は SSAO_VS.cso（FSTriVS）を共有するため、ここでは PS のみコンパイルする。
#include "../post/FullscreenTri.hlsli"   // FSQuadVSOut(uv)

Texture2D<float> g_ao         : register(t0);
SamplerState     g_pointClamp : register(s0);

cbuffer BlurCB : register(b0)
{
    float2 invRTSize;   // 1/width, 1/height
    float2 _pad;
};

float PSMain(FSQuadVSOut i) : SV_TARGET
{
    float sum = 0.0;
    [unroll]
    for (int y = -2; y < 2; ++y)
    [unroll]
    for (int x = -2; x < 2; ++x)
        sum += g_ao.Sample(g_pointClamp, i.uv + float2(x, y) * invRTSize);
    return sum / 16.0;
}
