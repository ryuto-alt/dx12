// FoliageDepth.hlsl - 植生 F1: 深度専用（カメラの深度プリパス / CSM の影）の VS + PS。
//   VSMain                : 風で変位した位置 → クリップ座標。PS へ UV とディザを渡す
//   PSMain                : ディザだけ（不透明の幹 / 岩）
//   PSMain + ALPHA_TEST=1 : ディザ + アルファクリップ（葉。ShadowMask.hlsl と同じ t0 / b2 / cutoff）
// ★本体（ForwardFoliage_*）と同じ FoliageTransformVertex / 同じディザ式。ずれると穴が開く。
// ★影パスは fade = (2, 2)（ディザ無し）で描く。カメラの深度プリパスは本体と同じ fade を持つ。
// 風のフレーム共通値は専用 CBV（b1）。C++ 側が SetGraphicsRootConstantBufferView(kSlotPerFrame, ...) で貼る。
#include "FoliageVS.hlsli"

cbuffer PerObjectConstants : register(b0)
{
    float4x4 gViewProjT;
    float4   gLayerWind;
};

cbuffer FoliageFrame : register(b1)
{
    float4 gWindA;
    float4 gWindB;
};

Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);

cbuffer PBRMaterial : register(b2)
{
    float defaultMetallic;
    float defaultRoughness;
    uint  pbrFlags;        // bit8..15 = alphaCutoff
    uint  packedTint;
    float4 uvScaleOffset;
};

struct VSOut
{
    float4 positionSV : SV_POSITION;
    float2 texCoord   : TEXCOORD0;
    nointerpolation float2 fade : TEXCOORD1;
};

VSOut VSMain(FoliageVSIn input)
{
    FoliageWindFrame wf = FoliageMakeWindFrame(gWindA, gWindB);
    FoliageWindLayer wl;
    wl.bend = gLayerWind.x; wl.flutter = gLayerWind.y; wl.bendExp = gLayerWind.z; wl.heightLocal = gLayerWind.w;
    const FoliageVertex v = FoliageTransformVertex(input, wf, wl, float3(1, 1, 1), 0.0);
    VSOut o;
    o.positionSV = mul(float4(v.wp, 1.0), gViewProjT);
    o.texCoord = input.texCoord;
    o.fade = v.fade;
    return o;
}

void PSMain(VSOut i)
{
    const float n = FoliageDitherNoise(i.positionSV.xy);
    if (n >= i.fade.x && n <= i.fade.y) discard;
#ifdef ALPHA_TEST
    const float2 uv = i.texCoord * uvScaleOffset.xy + uvScaleOffset.zw;
    const float cutoff = float((pbrFlags >> 8) & 0xFF) / 255.0;
    const float lod = g_albedo.CalculateLevelOfDetail(g_sampler, uv);
    clip(FoliageMaskAlpha(g_albedo.Sample(g_sampler, uv).a, lod) - cutoff);
#endif
}
