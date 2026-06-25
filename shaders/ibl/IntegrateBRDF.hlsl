// IntegrateBRDF.hlsl - split-sum approximation の scale/bias LUT を数値積分（cs_6_0）
// 出力 512x512 RG16F。x=NdotV, y=roughness。

#include "IBLCommon.hlsli"

RWTexture2D<float2> g_lut : register(u0);

cbuffer IBLConstants : register(b0)
{
    uint  faceSize;   // =512（LUT の一辺）
    float roughness;  // 未使用
    uint  _pad0;
    uint  _pad1;
};

// Smith Geometry（IBL 用 k）
float GeometrySchlickGGX_IBL(float NdotV, float roughness)
{
    float a = roughness;
    float k = (a * a) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith_IBL(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX_IBL(NdotV, roughness) * GeometrySchlickGGX_IBL(NdotL, roughness);
}

float2 IntegrateBRDFValue(float NdotV, float roughness)
{
    float3 V;
    V.x = sqrt(1.0 - NdotV * NdotV);
    V.y = 0.0;
    V.z = NdotV;

    float A = 0.0;
    float B = 0.0;

    float3 N = float3(0.0, 0.0, 1.0);

    const uint NUM_SAMPLES = 1024u;
    for (uint i = 0u; i < NUM_SAMPLES; ++i)
    {
        float2 xi = Hammersley(i, NUM_SAMPLES);
        float3 H  = ImportanceSampleGGX(xi, N, roughness);
        float3 L  = normalize(2.0 * dot(V, H) * H - V);

        float NoL = max(L.z, 0.0);
        float NoH = max(H.z, 0.0);
        float VoH = max(dot(V, H), 0.0);

        if (NoL > 0.0)
        {
            float G     = GeometrySmith_IBL(N, V, L, roughness);
            float G_Vis = (G * VoH) / max(NoH * NdotV, 1e-5);
            float Fc    = pow(1.0 - VoH, 5.0);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    return float2(A, B) / float(NUM_SAMPLES);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= faceSize || dtid.y >= faceSize)
        return;

    float NdotV     = (float(dtid.x) + 0.5) / float(faceSize);
    float roughVal  = (float(dtid.y) + 0.5) / float(faceSize);

    g_lut[dtid.xy] = IntegrateBRDFValue(max(NdotV, 1e-3), roughVal);
}
