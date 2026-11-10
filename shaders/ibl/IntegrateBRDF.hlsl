// IntegrateBRDF.hlsl - split-sum approximation の scale/bias LUT を数値積分（cs_6_0）
// 出力 512x512 RGBA16F。x=NdotV, y=roughness。
//   .r = A = ∫(1-Fc)·G_vis  / .g = B = ∫Fc·G_vis（split-sum。A+B = F0=1 の方向アルベド E → マルチスキャッタ補償）
//   .b = シーン（Charlie 分布 + Charlie 可視項）の方向アルベド E_sheen(N·V, sheenRoughness)（y をシーンの粗さとして読む）
//   .a = 直接光の遮蔽（Schlick-GGX k = (r+1)²/8。Lighting.hlsli の GeometrySmith）で積分した F0=1 の方向アルベド
//        （直接光とパストレーサーのマルチスキャッタ補償はこれで割る。IBL は A+B で割る。遮蔽モデルを揃えないと補償が合わない）
// ★環境マップに依らないので、IBLBaker は環境が無くても起動時に 1 回焼く（マルチスキャッタは直接光にも使う）。

#include "IBLCommon.hlsli"
#include "../forward/BrdfExt.hlsli"

RWTexture2D<float4> g_lut : register(u0);
static float g_lastEd;   // IntegrateBRDFValue が最後に積分した直接光の E（.a）

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
    float Ed = 0.0;

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
            const float rk = roughness + 1.0;
            const float kd = rk * rk / 8.0;
            const float Gd = (NdotV / (NdotV * (1.0 - kd) + kd)) * (NoL / (NoL * (1.0 - kd) + kd));
            Ed += Gd * VoH / max(NoH * NdotV, 1e-5);
        }
    }

    g_lastEd = Ed / float(NUM_SAMPLES);
    return float2(A, B) / float(NUM_SAMPLES);
}

// シーンの方向アルベド: ∫ D_charlie · V_charlie · (N·L) dω_L。L は一様半球サンプル（pdf = 1/2π）。
// Charlie は粗さが小さいほどグレージングの尖った葉になるので、サンプル数を多めに取る（LUT は起動時に 1 回だけ）。
float IntegrateSheenValue(float NdotV, float sheenRoughness)
{
    const float3 V = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    const uint NUM = 4096u;
    float sum = 0.0;
    for (uint i = 0u; i < NUM; ++i)
    {
        const float2 xi = Hammersley(i, NUM);
        const float cosT = xi.x;                       // 一様半球: cosθ は [0,1) 一様
        const float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
        const float phi  = 2.0 * PI_IBL * xi.y;
        const float3 L = float3(sinT * cos(phi), sinT * sin(phi), cosT);
        const float NoL = L.z;
        if (NoL <= 0.0) continue;
        const float3 H = normalize(V + L);
        const float D  = UnoBxD_Charlie(sheenRoughness, saturate(H.z));
        const float Vs = UnoBxV_Charlie(NoL, NdotV, sheenRoughness);
        sum += D * Vs * NoL;
    }
    return saturate(sum * (2.0 * PI_IBL) / float(NUM));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= faceSize || dtid.y >= faceSize)
        return;

    float NdotV     = (float(dtid.x) + 0.5) / float(faceSize);
    float roughVal  = (float(dtid.y) + 0.5) / float(faceSize);

    const float2 ab = IntegrateBRDFValue(max(NdotV, 1e-3), roughVal);
    const float  es = IntegrateSheenValue(max(NdotV, 1e-3), max(roughVal, 0.07));
    g_lut[dtid.xy] = float4(ab, es, saturate(g_lastEd));
}
