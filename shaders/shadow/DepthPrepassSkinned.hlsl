// DepthPrepassSkinned.hlsl - SSAO 用カメラ深度プリパス（スケルタルメッシュ）。
// クリップ Z を forward(ForwardSkinned.hlsl) とビット一致させるため、スキニング式を
// forward と完全一致させる: skinMatrix=sum(w*B) を作ってから mul(pos, skinMatrix)。
// （ShadowPassSkinned.hlsl は sum(w*mul(pos,B)) 順かつ totalWeight==0 で identity に
//  フォールバックするため演算順が違い、SSAO の深度再利用で z-fight/欠落を招く。よって専用化する。）
// totalWeight==0 のフォールバックも forward に合わせて「無し」（skinMatrix=0 → 原点）にする。

StructuredBuffer<float4x4> g_bones : register(t3);

cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;    // world * cameraViewProj（転置済み）
    float4x4 model;  // depth pass では未使用
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
    // ForwardSkinned.hlsl と同一: skinMatrix を作ってから mul(pos, skinMatrix)。
    float4x4 skinMatrix =
        input.boneWeights.x * g_bones[input.boneIndices.x] +
        input.boneWeights.y * g_bones[input.boneIndices.y] +
        input.boneWeights.z * g_bones[input.boneIndices.z] +
        input.boneWeights.w * g_bones[input.boneIndices.w];

    float4 skinnedPos = mul(float4(input.position, 1.0f), skinMatrix);
    return mul(skinnedPos, mvp);
}
