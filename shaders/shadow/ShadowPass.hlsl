// ShadowPass.hlsl - Depth-only pass for shadow map generation (static meshes)

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
    return mul(float4(input.position, 1.0f), mvp);
}
