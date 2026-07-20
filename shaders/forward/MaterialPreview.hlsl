// MaterialPreview.hlsl - マテリアルエディタの3Dプレビュー専用シェーダー。
// メインの Forward.hlsl(9スロットRootSignature、CSM/IBL/SSAO込み)とは完全に独立した
// 最小限の固定2灯スタジオライティング(Cook-Torrance)。専用の小さいRootSignatureを使う
// (src/editor/panels/MaterialPreviewRenderer.cpp 参照)。LDR(R8G8B8A8)RTへ直接出力する。
#include "PBR.hlsli"

Texture2D    g_albedo         : register(t0);
Texture2D    g_normalMap      : register(t1);
Texture2D    g_metalRoughness : register(t2);
SamplerState g_sampler        : register(s0);

cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
};

cbuffer PreviewLight : register(b1)
{
    float3 camPos;       float _pad0;
    float3 keyDir;       float keyIntensity;
    float3 keyColor;     float _pad1;
    float3 fillDir;      float fillIntensity;
    float3 fillColor;    float _pad2;
    float  ambient;      float3 _pad3;
};

cbuffer PBRMaterial : register(b2)
{
    float defaultMetallic;
    float defaultRoughness;
    uint  pbrFlags;   // bit0=hasNormalMap, bit1=hasMetalRoughness
    float _pbrPad;
};

// Lighting.hlsli の ShadePunctual と同一実装(PerFrame/CSM/IBL cbuffer への依存を切るため複製)。
float3 ShadePunctualLite(float3 N, float3 V, float3 L, float3 radiance,
                         float3 albedo, float3 F0, float metallic, float roughness)
{
    float3 H = normalize(V + L);
    float  NdotV = max(dot(N, V), 0.001);
    float  NdotL = max(dot(N, L), 0.0);

    float  NDF = DistributionGGX(N, H, roughness);
    float  G   = GeometrySmith(N, V, L, roughness);
    float3 F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

    float3 kD   = (1.0 - F) * (1.0 - metallic);
    float3 spec = (NDF * G * F) / (4.0 * NdotV * NdotL + 0.0001);

    return (kD * albedo / PI + spec) * radiance * NdotL;
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 texCoord : TEXCOORD0;
    float4 tangent  : TANGENT;
};

struct PSInput
{
    float4 positionSV   : SV_POSITION;
    float3 worldPos     : TEXCOORD1;
    float3 worldNormal  : NORMAL;
    float3 worldTangent : TANGENT;
    float  tangentW     : TEXCOORD2;
    float2 texCoord     : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV   = mul(float4(input.position, 1.0f), mvp);
    float4 worldPos4    = mul(float4(input.position, 1.0f), model);
    output.worldPos     = worldPos4.xyz;
    output.worldNormal  = normalize(mul(input.normal, (float3x3)model));
    output.worldTangent = normalize(mul(input.tangent.xyz, (float3x3)model));
    output.tangentW     = input.tangent.w;
    output.texCoord     = input.texCoord;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 albedo = g_albedo.Sample(g_sampler, input.texCoord).rgb;

    float3 N;
    if (pbrFlags & 1u)
    {
        float3 normalSample = g_normalMap.Sample(g_sampler, input.texCoord).rgb;
        N = PerturbNormal(input.worldNormal, input.worldTangent, input.tangentW, normalSample);
    }
    else
    {
        N = normalize(input.worldNormal);
    }

    float metallic, roughness;
    if (pbrFlags & 2u)
    {
        float4 mr = g_metalRoughness.Sample(g_sampler, input.texCoord);
        roughness = mr.g * defaultRoughness;
        metallic  = mr.b * defaultMetallic;
    }
    else
    {
        metallic  = defaultMetallic;
        roughness = defaultRoughness;
    }
    roughness = max(roughness, 0.04);

    float3 V  = normalize(camPos - input.worldPos);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float3 Lo = ShadePunctualLite(N, V, normalize(-keyDir),  keyColor  * keyIntensity,  albedo, F0, metallic, roughness);
    Lo       += ShadePunctualLite(N, V, normalize(-fillDir), fillColor * fillIntensity, albedo, F0, metallic, roughness);

    float3 ambientDiffuse  = albedo * (1.0 - metallic);
    float3 ambientSpecular = F0;
    float3 color = Lo + ambient * (ambientDiffuse + ambientSpecular);

    color = ACESFilm(color);
    color = pow(color, 1.0 / 2.2);
    return float4(color, 1.0f);
}
