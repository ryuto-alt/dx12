// VelocityPrepass.hlsl - 静的メッシュの速度 + G-Buffer 生成（深度も同時に書く）。
//
// b0 レイアウト(40 DWORD = ルート定数の上限ぴったり):
//   [ 0..15] gMvpJ        transpose(world * viewProjJittered)
//   [16..19] gPrevC0      transpose(prevWorld * prevViewProjNoJitter) の row0（= prevMvp の col0）
//   [20..23] gPrevC1      同 row1
//   [24..27] gPrevC3      同 row3   ★row2(z) は使わないので送らない。ここで浮いた 4 DWORD を法線行列に回した
//   [28..30] gNrm0.xyz    transpose(world) の row0.xyz（= world 3x3 の col0）
//   [31]     gJitterX
//   [32..34] gNrm1.xyz
//   [35]     gJitterY
//   [36..38] gNrm2.xyz
//   [39]     gMatPacked   round(rough*255) + round(metal*255)*256
//
// ★ ShadowPass.hlsl / Forward.hlsl と同じ演算順 mul(pos, mvp) にすること。
//   演算順が違うとクリップ Z がビット一致せず、forward の LESS_EQUAL で面が欠落する
//   （DepthPrepassSkinned.hlsl のヘッダコメントが実際にこの事故の記録）。
// ★ 法線も Forward.hlsl の VS と同じ式にすること（mul(normal, (float3x3)model)）。
//   float3x3 の列ベクトルとの内積 = gNrm0/1/2 との内積。
#include "VelocityCommon.hlsli"

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

    o.posSV = mul(p, gMvpJ);

    // 現フレームのジッタ除去: clip.xy -= jitter * clip.w （NDC で jitter ぶん戻すのと等価）
    o.curClip     = o.posSV;
    o.curClip.xy -= float2(gJitterX, gJitterY) * o.posSV.w;

    // 前フレームは最初から非ジッタ。x/y/w だけあれば NDC が出るので z 列は送っていない。
    o.prevClip = float4(dot(p, gPrevC0), dot(p, gPrevC1), 0.0f, dot(p, gPrevC3));

    o.worldNormal = float3(dot(input.normal, gNrm0),
                          dot(input.normal, gNrm1),
                          dot(input.normal, gNrm2));

    float rough, metal;
    SS_UnpackMaterial(gMatPacked, rough, metal);
    o.material = float2(rough, metal);
#ifdef ALPHA_TEST
    o.uv = input.texCoord;   // MASK バリアントだけ UV を流す（PS で clip する）
#endif
    return o;
}
