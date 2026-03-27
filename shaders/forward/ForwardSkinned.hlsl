// ForwardSkinned.hlsl - Forward rendering with skeletal animation

Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);
StructuredBuffer<float4x4> g_bones : register(t1);

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
    float4 positionSV  : SV_POSITION;
    float3 worldNormal : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
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
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 texColor = g_albedo.Sample(g_sampler, input.texCoord);
    float4 baseColor = texColor * input.color;

    float3 N = normalize(input.worldNormal);
    float3 L = normalize(-lightDir);
    float  NdotL = max(dot(N, L), 0.0f);

    float3 ambient = ambientStrength * lightColor;
    float3 diffuse = NdotL * lightColor;
    float3 finalColor = baseColor.rgb * (ambient + diffuse);

    return float4(finalColor, baseColor.a);
}
