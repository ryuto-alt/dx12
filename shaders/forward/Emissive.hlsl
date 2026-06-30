// Emissive.hlsl - 加算発光（Pfx 弾など）。GPU インスタンシング版。
// slot1 の per-instance データ（transpose(world) の先頭3行 + 色）から world を復元して描く。
// 加算ブレンド前提なので順序非依存＝インスタンシングで安全にまとめられる。テクスチャ不要。

cbuffer PerPass : register(b0)
{
    float4x4 gViewProjT;   // = transpose(camViewProj)（CPU で転置して渡す）
};

struct VSInput
{
    // slot0（頂点）: 既存頂点レイアウトと一致。position 以外は未使用だがレイアウト整合のため宣言。
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
    float4 tangent     : TANGENT;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
    // slot1（per-instance）: transpose(world) の3行 + 色
    float4 ir0         : TEXCOORD8;
    float4 ir1         : TEXCOORD9;
    float4 ir2         : TEXCOORD10;
    float4 icolor      : TEXCOORD11;
};

struct PSInput
{
    float4 positionSV : SV_POSITION;
    float4 color      : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    // model = float4x4(rows = ir0,ir1,ir2,(0,0,0,1)) = transpose(world)。
    // mul(model, pos) = pos·world（列ベクトル規約。CPU 側は ir = transpose(world).r[0..2]）。
    float4x4 model = float4x4(input.ir0, input.ir1, input.ir2, float4(0, 0, 0, 1));
    float4 wp = mul(model, float4(input.position, 1.0f));   // = pos·world（行列×列ベクトル）
    // gViewProjT は cbuffer の列優先再解釈で hlsl 上は VP 本体になる（CPUで transpose(VP) を渡す）。
    output.positionSV = mul(wp, gViewProjT);                // wp·VP = クリップ座標（行ベクトル×行列）
    output.color = input.color * input.icolor;              // 頂点色(共有メッシュは白) × per-instance 色
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // 加算ブレンド前提：色を増幅して出力（HDR で 1 を超え bloom が拾って発光する）
    return float4(input.color.rgb * 2.4f, 1.0f);
}
