// ForwardFoliage_VS.hlsl - 植生 F1: 本体（Forward+）の頂点シェーダ。
// PS は ForwardFoliage_PS.hlsl（Forward.hlsl の PSMain をディザ付きで包んだもの）。PSInput の並びは Forward.hlsl と一致させること。
//
// ★風のフレーム共通値は PerFrame(b1) の予約領域 _clusterReserved[0..1]（DWORD 増分ゼロ）。レイヤー別は b0 の末尾。
#include "../forward/Lighting.hlsli"
#include "FoliageVS.hlsli"

cbuffer PerObjectConstants : register(b0)
{
    float4x4 gViewProjT;   // transpose(viewProj)（ジッタあり）
    float4   gLayerWind;   // x=曲げ y=はばたき z=曲げの指数 w=モデル高さ(m)
    float4   gLayerTint;   // rgb=レイヤー色 / a=AO の効き
};

struct VSOut
{
    float4 positionSV   : SV_POSITION;
    float3 worldPos     : TEXCOORD2;
    float3 worldNormal  : NORMAL;
    float3 worldTangent : TANGENT;
    float  tangentW     : TEXCOORD3;
    float4 color        : COLOR;
    float2 texCoord     : TEXCOORD0;
    float  viewDepth    : TEXCOORD4;
    nointerpolation float2 fade : TEXCOORD5;
};

VSOut VSMain(FoliageVSIn input)
{
    FoliageWindFrame wf = FoliageMakeWindFrame(_clusterReserved[0], _clusterReserved[1]);
    FoliageWindLayer wl;
    wl.bend = gLayerWind.x; wl.flutter = gLayerWind.y; wl.bendExp = gLayerWind.z; wl.heightLocal = gLayerWind.w;
    const FoliageVertex v = FoliageTransformVertex(input, wf, wl, gLayerTint.rgb, gLayerTint.a);

    VSOut o;
    const float4 wp4 = float4(v.wp, 1.0);
    o.positionSV   = mul(wp4, gViewProjT);
    o.worldPos     = v.wp;
    o.worldNormal  = v.nrm;
    o.worldTangent = v.tan;
    o.tangentW     = input.tangent.w;
    o.color        = v.color;
    o.texCoord     = input.texCoord;
    o.viewDepth    = mul(wp4, view).z;   // LH: 前方 +z
    o.fade         = v.fade;
    return o;
}
