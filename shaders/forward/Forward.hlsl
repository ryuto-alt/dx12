// Forward.hlsl - PBR Forward Rendering (Cook-Torrance BRDF)
#include "PBR.hlsli"

// Textures
Texture2D    g_albedo         : register(t0);
Texture2D    g_normalMap      : register(t1);
Texture2D    g_metalRoughness : register(t2);
SamplerState g_sampler        : register(s0);

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
    uint  pbrFlags;       // bit0=hasNormalMap, bit1=hasMetalRoughness
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
    output.positionSV   = mul(float4(input.position, 1.0f), mvp);

    float4 worldPos4    = mul(float4(input.position, 1.0f), model);
    output.worldPos     = worldPos4.xyz;
    output.worldNormal  = normalize(mul(input.normal, (float3x3)model));
    output.worldTangent = normalize(mul(input.tangent.xyz, (float3x3)model));
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
    // Albedo
    float4 albedo4 = g_albedo.Sample(g_sampler, input.texCoord) * input.color;
    float3 albedo = albedo4.rgb;

    // Normal
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

    // Metallic / Roughness
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

    // View & Light
    float3 V = normalize(cameraPos - input.worldPos);
    float3 L = normalize(-lightDir);
    float3 H = normalize(V + L);

    // PBR Cook-Torrance BRDF
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float  NDF = DistributionGGX(N, H, roughness);
    float  G   = GeometrySmith(N, V, L, roughness);
    float3 F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);

    float3 specular = (NDF * G * F) / (4.0 * NdotV * NdotL + 0.0001);

    // Shadow
    float shadow = CalcShadow(input.shadowCoord);

    // Final
    float3 Lo = (kD * albedo / PI + specular) * lightColor * NdotL * shadow;
    float3 ambient = ambientStrength * albedo;
    float3 color = ambient + Lo;

    // Tone mapping + gamma
    color = ACESFilm(color);
    color = pow(color, 1.0 / 2.2);

    return float4(color, albedo4.a);
}
