// ShadowPassSkinned.hlsl - Depth-only pass for shadow map (skeletal meshes)

StructuredBuffer<float4x4> g_bones : register(t3);

cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;    // lightViewProj * model
    float4x4 model;  // unused in shadow pass
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

float4 VSMain(VSInput input) : SV_POSITION
{
    float4 pos = float4(input.position, 1.0f);
    float4 skinnedPos = (float4)0;

    float totalWeight = input.boneWeights.x + input.boneWeights.y
                      + input.boneWeights.z + input.boneWeights.w;

    if (totalWeight > 0.0f)
    {
        skinnedPos += input.boneWeights.x * mul(pos, g_bones[input.boneIndices.x]);
        skinnedPos += input.boneWeights.y * mul(pos, g_bones[input.boneIndices.y]);
        skinnedPos += input.boneWeights.z * mul(pos, g_bones[input.boneIndices.z]);
        skinnedPos += input.boneWeights.w * mul(pos, g_bones[input.boneIndices.w]);
    }
    else
    {
        skinnedPos = pos;
    }

    return mul(skinnedPos, mvp);
}
