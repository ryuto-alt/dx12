// ShadowPassInstanced.hlsl - 深度onlyパス（シャドウ/深度プリパス）のインスタンシング版。
// world は slot1 の per-instance 属性から復元し、b0 には transpose(viewProj) を入れる。
// 深度しか書かないので PS は無し。

cbuffer PerObjectConstants : register(b0)
{
    float4x4 gViewProjT;
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
    float4 ir0         : TEXCOORD8;
    float4 ir1         : TEXCOORD9;
    float4 ir2         : TEXCOORD10;
    float4 icolor      : TEXCOORD11;
};

float4 VSMain(VSInput input) : SV_POSITION
{
    float4x4 modelT = float4x4(input.ir0, input.ir1, input.ir2, float4(0, 0, 0, 1));
    float4 wp = mul(modelT, float4(input.position, 1.0f));
    return mul(wp, gViewProjT);
}
