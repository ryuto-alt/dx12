// VelocityPrepass.hlsl - 静的メッシュの速度バッファ生成（深度も同時に書く）。
// b0 レイアウト(36 DWORD): gMvpJ(16) + gPrevMvp(16) + gJitterNdc(2) + pad(2)
//
// ★ ShadowPass.hlsl / Forward.hlsl と同じ演算順 mul(pos, mvp) にすること。
//   演算順が違うとクリップ Z がビット一致せず、forward の LESS_EQUAL で面が欠落する
//   （DepthPrepassSkinned.hlsl のヘッダコメントが実際にこの事故の記録）。
#include "VelocityCommon.hlsli"

cbuffer PerObjectConstants : register(b0)
{
    float4x4 gMvpJ;        // transpose(world * viewProjJittered)
    float4x4 gPrevMvp;     // transpose(prevWorld * prevViewProjNoJitter)
    float2   gJitterNdc;   // 現フレームの NDC ジッタ
    float2   _vpad;
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

    o.posSV = mul(p, gMvpJ);

    // 現フレームのジッタ除去: clip.xy -= jitter * clip.w （NDC で jitter ぶん戻すのと等価）
    o.curClip     = o.posSV;
    o.curClip.xy -= gJitterNdc * o.posSV.w;

    o.prevClip = mul(p, gPrevMvp);   // 前フレームは最初から非ジッタ
    return o;
}
