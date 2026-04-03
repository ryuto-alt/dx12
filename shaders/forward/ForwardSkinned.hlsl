// ForwardSkinned.hlsl - PBR Forward Rendering with Skeletal Animation
#include "PBR.hlsli"

// Textures
Texture2D    g_albedo         : register(t0);
Texture2D    g_normalMap      : register(t1);
Texture2D    g_metalRoughness : register(t2);
SamplerState g_sampler        : register(s0);

// Bones (t3 - moved from t1)
StructuredBuffer<float4x4> g_bones : register(t3);

// Shadow
Texture2D              g_shadowMap     : register(t4);
SamplerComparisonState g_shadowSampler : register(s1);

// PerObject constants (b0)
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
};

// PerFrame constants (b1)
cbuffer PerFrameConstants : register(b1)
{
    float4x4 view;
    float4x4 proj;
    float3   lightDir;
    float    time;
    float3   lightColor;
    float    ambientStrength;
    float4x4 lightViewProj;
    float3   cameraPos;
    float    _pad;
};

// PBR Material constants (b2)
cbuffer PBRMaterial : register(b2)
{
    float defaultMetallic;
    float defaultRoughness;
    uint  pbrFlags;
    float _pbrPad;
};

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
    float4 tangent     : TANGENT;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 positionSV   : SV_POSITION;
    float3 worldPos     : TEXCOORD2;
    float3 worldNormal  : NORMAL;
    float3 worldTangent : TANGENT;
    float  tangentW     : TEXCOORD3;
    float4 color        : COLOR;
    float2 texCoord     : TEXCOORD0;
    float4 shadowCoord  : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    // GPU Skinning (Linear Blend Skinning)
    float4x4 skinMatrix =
        input.boneWeights.x * g_bones[input.boneIndices.x] +
        input.boneWeights.y * g_bones[input.boneIndices.y] +
        input.boneWeights.z * g_bones[input.boneIndices.z] +
        input.boneWeights.w * g_bones[input.boneIndices.w];

    float4 skinnedPos = mul(float4(input.position, 1.0f), skinMatrix);
    float3 skinnedNormal = normalize(mul(input.normal, (float3x3)skinMatrix));
    float3 skinnedTangent = normalize(mul(input.tangent.xyz, (float3x3)skinMatrix));

    output.positionSV   = mul(skinnedPos, mvp);
    float4 worldPos4    = mul(skinnedPos, model);
    output.worldPos     = worldPos4.xyz;
    output.worldNormal  = normalize(mul(skinnedNormal, (float3x3)model));
    output.worldTangent = normalize(mul(skinnedTangent, (float3x3)model));
    output.tangentW     = input.tangent.w;
    output.color        = input.color;
    output.texCoord     = input.texCoord;
    output.shadowCoord  = mul(worldPos4, lightViewProj);

    return output;
}

float CalcShadow(float4 shadowCoord)
{
    float3 projCoords = shadowCoord.xyz / shadowCoord.w;
    float2 shadowUV = projCoords.xy * 0.5f + 0.5f;
    shadowUV.y = 1.0f - shadowUV.y;

    if (shadowUV.x < 0 || shadowUV.x > 1 || shadowUV.y < 0 || shadowUV.y > 1)
        return 1.0f;

    float currentDepth = projCoords.z;
    float shadow = 0.0f;
    float texelSize = 1.0f / 4096.0f;

    [unroll]
    for (int y = -2; y <= 2; y++)
    {
        [unroll]
        for (int x = -2; x <= 2; x++)
        {
            shadow += g_shadowMap.SampleCmpLevelZero(g_shadowSampler,
                shadowUV + float2(x, y) * texelSize, currentDepth);
        }
    }
    return shadow / 25.0f;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 albedo4 = g_albedo.Sample(g_sampler, input.texCoord) * input.color;
    float3 albedo = albedo4.rgb;

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
        roughness = mr.g;
        metallic  = mr.b;
    }
    else
    {
        metallic  = defaultMetallic;
        roughness = defaultRoughness;
    }
    roughness = max(roughness, 0.04);

    float3 V = normalize(cameraPos - input.worldPos);
    float3 L = normalize(-lightDir);
    float3 H = normalize(V + L);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float  NDF = DistributionGGX(N, H, roughness);
    float  G   = GeometrySmith(N, V, L, roughness);
    float3 F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);

    float3 specular = (NDF * G * F) / (4.0 * NdotV * NdotL + 0.0001);

    float shadow = CalcShadow(input.shadowCoord);

    float3 Lo = (kD * albedo / PI + specular) * lightColor * NdotL * shadow;
    float3 ambient = ambientStrength * albedo;
    float3 color = ambient + Lo;

    color = ACESFilm(color);
    color = pow(color, 1.0 / 2.2);

    return float4(color, albedo4.a);
}
