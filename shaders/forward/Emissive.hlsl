// Emissive.hlsl - 加算発光（パーティクル用）
// ライティングを無視し、頂点カラーをそのまま明るく出力する。
// 加算ブレンド＋bloom と組み合わせて「光る粒子」になる。テクスチャ不要。

cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
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

struct PSInput
{
    float4 positionSV : SV_POSITION;
    float4 color      : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV = mul(float4(input.position, 1.0f), mvp);
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // 加算ブレンド前提：色を増幅して出力（HDR で 1 を超え bloom が拾って発光する）
    return float4(input.color.rgb * 2.4f, 1.0f);
}
