// PrefilterEnv.hlsl - roughness 別 GGX importance sampling で prefiltered cube の各 mip を生成（cs_6_0）
// mip ごとに Dispatch（faceSize/roughness を cbuffer で差し替え）。出力は 6 スライスの Texture2DArray。

#include "IBLCommon.hlsli"

TextureCube              g_env  : register(t0);
RWTexture2DArray<float4> g_out  : register(u0);
SamplerState             g_samp : register(s0);  // LINEAR CLAMP

cbuffer IBLConstants : register(b0)
{
    uint  faceSize;   // 現 mip の 1 面のサイズ
    float roughness;  // 現 mip の roughness（mip/maxMip）
    uint  _pad0;
    uint  _pad1;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= faceSize || dtid.y >= faceSize)
        return;

    uint   face = dtid.z;
    float2 uv   = (float2(dtid.xy) + 0.5) / float(faceSize);
    float3 N    = DirectionFromFaceUV(face, uv);

    // 視線 = 法線 = 反射方向 と近似（split-sum の前提）
    float3 R = N;
    float3 V = N;

    const uint NUM_SAMPLES = 128u;
    float3 prefiltered = float3(0.0, 0.0, 0.0);
    float  totalWeight = 0.0;

    for (uint i = 0u; i < NUM_SAMPLES; ++i)
    {
        float2 xi = Hammersley(i, NUM_SAMPLES);
        float3 H  = ImportanceSampleGGX(xi, N, roughness);
        float3 L  = normalize(2.0 * dot(V, H) * H - V);

        float NoL = max(dot(N, L), 0.0);
        if (NoL > 0.0)
        {
            prefiltered += g_env.SampleLevel(g_samp, L, 0).rgb * NoL;
            totalWeight += NoL;
        }
    }

    prefiltered = prefiltered / max(totalWeight, 0.001);
    g_out[dtid] = float4(prefiltered, 1.0);
}
