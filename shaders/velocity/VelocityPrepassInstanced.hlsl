// VelocityPrepassInstanced.hlsl - 自動インスタンシング版の速度 + G-Buffer 生成。
// slot1 = MeshInstanceData(現world, 64B) / slot2 = MeshInstancePrevData(前world, 48B)
// ★ ShadowPassInstanced.hlsl と同じ復元式（mul(modelT, pos) → mul(wp, gViewProjT)）にすること。
//
// b0 レイアウト(36 DWORD): gViewProjJT(16) + gPrevViewProjT(16) + gJitterNdc(2) + gMatPacked(1) + pad(1)
// ★静的/スキンド版とは b0 のレイアウトが違う。roughness/metallic は VS で展開して
//   インターポレータへ流すので、PS は b0 を一切読まない（読むとレイアウト差で壊れる）。
#include "VelocityCommon.hlsli"

cbuffer PerObjectConstants : register(b0)
{
    float4x4 gViewProjJT;      // transpose(viewProjJittered)
    float4x4 gPrevViewProjT;   // transpose(prevViewProjNoJitter)
    float2   gJitterNdc;
    float    gMatPacked;
    float    _vpad;
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
    float4 ir0         : TEXCOORD8;    // slot1: transpose(world) 3行
    float4 ir1         : TEXCOORD9;
    float4 ir2         : TEXCOORD10;
    float4 icolor      : TEXCOORD11;
    float4 ip0         : TEXCOORD12;   // slot2: transpose(prevWorld) 3行
    float4 ip1         : TEXCOORD13;
    float4 ip2         : TEXCOORD14;
};

VelocityVSOut VSMain(VSInput input)
{
    VelocityVSOut o;
    float4 p = float4(input.position, 1.0f);

    float4x4 modelT     = float4x4(input.ir0, input.ir1, input.ir2, float4(0, 0, 0, 1));
    float4x4 prevModelT = float4x4(input.ip0, input.ip1, input.ip2, float4(0, 0, 0, 1));

    float4 wp     = mul(modelT,     p);
    float4 prevWp = mul(prevModelT, p);

    o.posSV       = mul(wp, gViewProjJT);
    o.curClip     = o.posSV;
    o.curClip.xy -= gJitterNdc * o.posSV.w;
    o.prevClip    = mul(prevWp, gPrevViewProjT);

    // modelT は transpose(world) なので mul(modelT, n) = n * world3x3
    // ＝ ForwardInstanced / Forward.hlsl の mul(normal, (float3x3)model) と同値。
    o.worldNormal = mul(modelT, float4(input.normal, 0.0f)).xyz;

    float rough, metal;
    SS_UnpackMaterial(gMatPacked, rough, metal);
    o.material = float2(rough, metal);
#ifdef ALPHA_TEST
    o.uv = input.texCoord;   // MASK バリアントだけ UV を流す（PS で clip する）
#endif
    return o;
}
