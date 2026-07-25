// ForwardInstanced.hlsl - Forward.hlsl の VS を GPU インスタンシング化した版。
// PS は Forward_PS.cso をそのまま共用するので、PSInput の並び/セマンティクスは
// Forward.hlsl と完全に一致させること（ズレると PSO 作成が落ちる）。
//
// world 行列は cbuffer ではなく slot1 の per-instance 頂点属性から復元する。
// これで「同じメッシュ・同じマテリアルの N 体」を 1 ドローに畳める。
#include "Lighting.hlsli"

// PerObject(b0): インスタンシング時は per-object の mvp/model が要らないので、
// 代わりにパス共通の transpose(viewProj) を入れる（Emissive.hlsl と同じ流儀）。
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
    // slot1（per-instance）: transpose(world) の先頭3行 + 色
    float4 ir0         : TEXCOORD8;
    float4 ir1         : TEXCOORD9;
    float4 ir2         : TEXCOORD10;
    float4 icolor      : TEXCOORD11;
};

struct PSInput
{
    float4 positionSV   : SV_POSITION;
    float3 worldPos     : TEXCOORD2;
    float3 worldNormal  : NORMAL;
    float3 worldTangent : TANGENT;
    float  tangentW     : TEXCOORD3;
    float4 color        : COLOR;
    float2 texCoord     : TEXCOORD0;
    float  viewDepth    : TEXCOORD4;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    // modelT[r][c] = world[c][r]（= 数学的な transpose(world)）。
    // mul(modelT, col) は行ベクトル規約の pos·world と等価。
    float4x4 modelT = float4x4(input.ir0, input.ir1, input.ir2, float4(0, 0, 0, 1));

    float4 worldPos4    = mul(modelT, float4(input.position, 1.0f));
    output.positionSV   = mul(worldPos4, gViewProjT);
    output.worldPos     = worldPos4.xyz;
    output.worldNormal  = normalize(mul((float3x3)modelT, input.normal));
    output.worldTangent = normalize(mul((float3x3)modelT, input.tangent.xyz));
    output.tangentW     = input.tangent.w;
    output.color        = input.color * input.icolor;
    output.texCoord     = input.texCoord;

    float4 viewPos4     = mul(worldPos4, view);   // LH: 前方 +z
    output.viewDepth    = viewPos4.z;

    return output;
}
