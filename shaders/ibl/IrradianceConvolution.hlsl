// IrradianceConvolution.hlsl - 環境キューブを半球積分して irradiance cube を生成（cs_6_0）
// 出力は 6 スライスの Texture2DArray（SRV 側で TextureCube として張る）。

#include "IBLCommon.hlsli"

TextureCube              g_env : register(t0);
RWTexture2DArray<float4> g_out : register(u0);
SamplerState             g_samp : register(s0);  // LINEAR CLAMP

cbuffer IBLConstants : register(b0)
{
    uint  faceSize;   // =32
    float roughness;  // 未使用（共通 cbuffer レイアウト維持）
    uint  _pad0;
    uint  _pad1;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= faceSize || dtid.y >= faceSize)
        return;

    uint  face = dtid.z;
    float2 uv  = (float2(dtid.xy) + 0.5) / float(faceSize);
    float3 N   = DirectionFromFaceUV(face, uv);

    // 半球積分（N を up とした接空間で φ/θ を走査）
    float3 up    = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 right = normalize(cross(up, N));
    up           = normalize(cross(N, right));

    float3 irradiance = float3(0.0, 0.0, 0.0);
    float  sampleCount = 0.0;
    const float dPhi   = 0.05;
    const float dTheta = 0.05;

    for (float phi = 0.0; phi < 2.0 * PI_IBL; phi += dPhi)
    {
        for (float theta = 0.0; theta < 0.5 * PI_IBL; theta += dTheta)
        {
            // 接空間 → world
            float3 tangentSample = float3(sin(theta) * cos(phi),
                                          sin(theta) * sin(phi),
                                          cos(theta));
            float3 sampleVec = tangentSample.x * right +
                               tangentSample.y * up +
                               tangentSample.z * N;

            irradiance += g_env.SampleLevel(g_samp, sampleVec, 0).rgb
                          * cos(theta) * sin(theta);
            sampleCount += 1.0;
        }
    }

    irradiance = PI_IBL * irradiance * (1.0 / max(sampleCount, 1.0));
    g_out[dtid] = float4(irradiance, 1.0);
}
