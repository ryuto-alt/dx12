// ForwardSkinned.hlsl - Forward rendering with skeletal animation + Shadow

Texture2D    g_albedo    : register(t0);
SamplerState g_sampler   : register(s0);
StructuredBuffer<float4x4> g_bones : register(t1);

// Shadow map
Texture2D              g_shadowMap      : register(t2);
SamplerComparisonState g_shadowSampler  : register(s1);

cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
};

cbuffer PerFrameConstants : register(b1)
{
    float4x4 view;
    float4x4 proj;
    float3   lightDir;
    float    time;
    float3   lightColor;
    float    ambientStrength;
    float4x4 lightViewProj;
};

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 positionSV   : SV_POSITION;
    float3 worldNormal  : NORMAL;
    float4 color        : COLOR;
    float2 texCoord     : TEXCOORD0;
    float4 shadowCoord  : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    // Linear Blend Skinning
    float4 pos = float4(input.position, 1.0f);
    float3 norm = input.normal;

    float4 skinnedPos = (float4)0;
    float3 skinnedNorm = (float3)0;

    float totalWeight = input.boneWeights.x + input.boneWeights.y
                      + input.boneWeights.z + input.boneWeights.w;

    if (totalWeight > 0.0f)
    {
        skinnedPos  += input.boneWeights.x * mul(pos,  g_bones[input.boneIndices.x]);
        skinnedPos  += input.boneWeights.y * mul(pos,  g_bones[input.boneIndices.y]);
        skinnedPos  += input.boneWeights.z * mul(pos,  g_bones[input.boneIndices.z]);
        skinnedPos  += input.boneWeights.w * mul(pos,  g_bones[input.boneIndices.w]);

        skinnedNorm += input.boneWeights.x * mul(norm, (float3x3)g_bones[input.boneIndices.x]);
        skinnedNorm += input.boneWeights.y * mul(norm, (float3x3)g_bones[input.boneIndices.y]);
        skinnedNorm += input.boneWeights.z * mul(norm, (float3x3)g_bones[input.boneIndices.z]);
        skinnedNorm += input.boneWeights.w * mul(norm, (float3x3)g_bones[input.boneIndices.w]);
    }
    else
    {
        skinnedPos = pos;
        skinnedNorm = norm;
    }

    PSInput output;
    output.positionSV  = mul(skinnedPos, mvp);
    output.worldNormal = normalize(mul(skinnedNorm, (float3x3)model));
    output.color       = input.color;
    output.texCoord    = input.texCoord;

    // ライト空間座標
    float4 worldPos = mul(skinnedPos, model);
    output.shadowCoord = mul(worldPos, lightViewProj);

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
            shadow += g_shadowMap.SampleCmpLevelZero(g_shadowSampler, shadowUV + float2(x, y) * texelSize, currentDepth);
        }
    }
    return shadow / 25.0f;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 texColor = g_albedo.Sample(g_sampler, input.texCoord);
    float4 baseColor = texColor * input.color;

    float3 N = normalize(input.worldNormal);
    float3 L = normalize(-lightDir);
    float  NdotL = max(dot(N, L), 0.0f);

    float shadow = CalcShadow(input.shadowCoord);

    float3 ambient = ambientStrength * lightColor;
    float3 diffuse = NdotL * lightColor * shadow;
    float3 finalColor = baseColor.rgb * (ambient + diffuse);

    return float4(finalColor, baseColor.a);
}
