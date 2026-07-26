// VelocityPrepassSkinned.hlsl - スキンドメッシュの速度 + G-Buffer 生成。
//
// ★ DepthPrepassSkinned.hlsl / ForwardSkinned.hlsl と完全に同じスキニング式にすること
//   （skinMatrix = sum(w*B) を作ってから mul(pos, skinMatrix)。totalWeight==0 のフォールバックは無し）。
//   ShadowPassSkinned.hlsl は演算順が違い、深度がビット一致しない。
//
// 前フレームのボーンは SkinningBuffer の「前フレームのスロット」の SRV を t12 にバインドしたもの。
//   SkinningBuffer は frameCount(=3) 枚を多重化していて、毎フレーム frameIndex のスロットだけを
//   書く＝前フレームのスロットには前フレームの行列がそのまま残っている。追加のGPUメモリ不要。
//
// b0 レイアウトは VelocityPrepass.hlsl と同一（40 DWORD）。ヘッダのコメント参照。
#include "VelocityCommon.hlsli"

StructuredBuffer<float4x4> g_bones     : register(t3);
StructuredBuffer<float4x4> g_prevBones : register(t12);

cbuffer PerObjectConstants : register(b0)
{
    float4x4 gMvpJ;
    float4   gPrevC0;
    float4   gPrevC1;
    float4   gPrevC3;
    float3   gNrm0;
    float    gJitterX;
    float3   gNrm1;
    float    gJitterY;
    float3   gNrm2;
    float    gMatPacked;
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

VelocityVSOut VSMain(VSInput input)
{
    VelocityVSOut o;
    float4 p = float4(input.position, 1.0f);

    float4x4 skinMatrix =
        input.boneWeights.x * g_bones[input.boneIndices.x] +
        input.boneWeights.y * g_bones[input.boneIndices.y] +
        input.boneWeights.z * g_bones[input.boneIndices.z] +
        input.boneWeights.w * g_bones[input.boneIndices.w];

    float4x4 prevSkinMatrix =
        input.boneWeights.x * g_prevBones[input.boneIndices.x] +
        input.boneWeights.y * g_prevBones[input.boneIndices.y] +
        input.boneWeights.z * g_prevBones[input.boneIndices.z] +
        input.boneWeights.w * g_prevBones[input.boneIndices.w];

    float4 sp     = mul(p, skinMatrix);
    float4 prevSp = mul(p, prevSkinMatrix);

    o.posSV       = mul(sp, gMvpJ);
    o.curClip     = o.posSV;
    o.curClip.xy -= float2(gJitterX, gJitterY) * o.posSV.w;
    o.prevClip    = float4(dot(prevSp, gPrevC0), dot(prevSp, gPrevC1), 0.0f, dot(prevSp, gPrevC3));

    // 法線もスキニング行列で回してからワールドへ（ForwardSkinned.hlsl:83,89 と同じ順序）。
    float3 sn = normalize(mul(input.normal, (float3x3)skinMatrix));
    o.worldNormal = float3(dot(sn, gNrm0), dot(sn, gNrm1), dot(sn, gNrm2));

    float rough, metal;
    SS_UnpackMaterial(gMatPacked, rough, metal);
    o.material = float2(rough, metal);
    return o;
}
