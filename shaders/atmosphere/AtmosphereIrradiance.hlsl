// AtmosphereIrradiance.hlsl — 大気の環境キューブ(mip 付き)から irradiance キューブを作る(cs_6_0)
// shaders/ibl/IrradianceConvolution.hlsl と同じ式(半球のコサイン重み積分)だが、環境キューブの mip(16x16 の面)を引いて
// サンプル数を 1/8 に減らす(GPU 0.7 ms → 0.1 ms 未満)。大気の空はなめらかなので誤差は無視できる。
// IBLBaker の増分再ベイク(RebakeStage の stage 0)が使う。既存の IrradianceConvolution は従来の環境マップ用のまま無変更。
// ルートシグネチャ・定数バッファ(IBLConstants)は IBLBaker の compute RS と同じ。

#include "../ibl/IBLCommon.hlsli"

TextureCube              g_env  : register(t0);
RWTexture2DArray<float4> g_out  : register(u0);
SamplerState             g_samp : register(s0);  // LINEAR CLAMP

cbuffer IBLConstants : register(b0)
{
    uint  faceSize;   // =32
    float roughness;  // 未使用
    uint  envSize;    // 環境キューブの mip0 の面サイズ
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

    float3 up    = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 right = normalize(cross(up, N));
    up           = normalize(cross(N, right));

    // 面が 16x16 になる mip(mip が無い環境でも SampleLevel は最大 mip へクランプする)
    const float mip = max(log2(float(max(envSize, 16u))) - 4.0, 0.0);

    float3 irradiance = float3(0.0, 0.0, 0.0);
    float  sampleCount = 0.0;
    const float dPhi   = 0.15;
    const float dTheta = 0.15;

    for (float phi = 0.0; phi < 2.0 * PI_IBL; phi += dPhi)
    {
        for (float theta = 0.0; theta < 0.5 * PI_IBL; theta += dTheta)
        {
            float3 tangentSample = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            irradiance += g_env.SampleLevel(g_samp, sampleVec, mip).rgb * cos(theta) * sin(theta);
            sampleCount += 1.0;
        }
    }

    irradiance = PI_IBL * irradiance * (1.0 / max(sampleCount, 1.0));
    g_out[dtid] = float4(irradiance, 1.0);
}
