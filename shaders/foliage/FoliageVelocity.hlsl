// FoliageVelocity.hlsl - 植生 F1: 速度 + G-Buffer プリパス（PrepassMode::DepthVelocityGBuffer）の VS + PS。
// ★風でゆれる葉でもモーションベクタが正しい: 現在時刻と前フレーム時刻の 2 回、同じ風関数で位置を評価する
//   （インスタンス自体は静的なので前フレームのワールド行列は要らない）。
// b0 は 40 DWORD ぴったり: vpJ(16) + prevVp(16) + jitter(2) + matPacked(1) + pad(1) + layerWind(4)。
// PS は VelocityCommon.hlsli の VelocityPS を使わず、ディザ（本体と同じ断片を捨てる）を前置して同じ出力を作る。
#define FOLIAGE_WANT_PREV 1
#include "FoliageVS.hlsli"
#include "../velocity/VelocityCommon.hlsli"

cbuffer PerObjectConstants : register(b0)
{
    float4x4 gViewProjJT;      // transpose(viewProjJittered)
    float4x4 gPrevViewProjT;   // transpose(prevViewProjNoJitter)
    float2   gJitterNdc;
    float    gMatPacked;
    float    _vpad;
    float4   gLayerWind;
};

cbuffer FoliageFrame : register(b1)
{
    float4 gWindA;
    float4 gWindB;
};

struct FoliageVelOut
{
    float4 posSV       : SV_POSITION;
    float4 curClip     : TEXCOORD0;
    float4 prevClip    : TEXCOORD1;
    float3 worldNormal : NORMAL;
    float2 material    : TEXCOORD2;
    float2 uv          : TEXCOORD3;
    nointerpolation float2 fade : TEXCOORD4;
};

FoliageVelOut VSMain(FoliageVSIn input)
{
    FoliageWindFrame wf = FoliageMakeWindFrame(gWindA, gWindB);
    FoliageWindLayer wl;
    wl.bend = gLayerWind.x; wl.flutter = gLayerWind.y; wl.bendExp = gLayerWind.z; wl.heightLocal = gLayerWind.w;
    const FoliageVertex v = FoliageTransformVertex(input, wf, wl, float3(1, 1, 1), 0.0);

    FoliageVelOut o;
    o.posSV = mul(float4(v.wp, 1.0), gViewProjJT);
    o.curClip = o.posSV;
    o.curClip.xy -= gJitterNdc * o.posSV.w;
    o.prevClip = mul(float4(v.wpPrev, 1.0), gPrevViewProjT);
    o.worldNormal = v.nrm;
    float rough, metal;
    SS_UnpackMaterial(gMatPacked, rough, metal);
    o.material = float2(rough, metal);
    o.uv = input.texCoord;
    o.fade = v.fade;
    return o;
}

// ALPHA_TEST 版の g_albedo(t0) / g_sampler(s0) / PBRMaterial(b2: vpbrFlags, vpbrUvScaleOffset) は VelocityCommon.hlsli が宣言している。
PrepassOut PSMain(FoliageVelOut i, bool isFront : SV_IsFrontFace)
{
    const float n = FoliageDitherNoise(i.posSV.xy);
    if (n >= i.fade.x && n <= i.fade.y) discard;
#ifdef ALPHA_TEST
    {
        const float2 uv = i.uv * vpbrUvScaleOffset.xy + vpbrUvScaleOffset.zw;
        const float cutoff = float((vpbrFlags >> 8) & 0xFF) / 255.0;
        const float lod = g_albedo.CalculateLevelOfDetail(g_sampler, uv);
        clip(FoliageMaskAlpha(g_albedo.Sample(g_sampler, uv).a, lod) - cutoff);
    }
#endif
    PrepassOut o;
    const float2 curNdc  = i.curClip.xy  / max(abs(i.curClip.w),  1e-6);
    const float2 prevNdc = i.prevClip.xy / max(abs(i.prevClip.w), 1e-6);
    o.velocity = (curNdc - prevNdc) * float2(0.5, -0.5);
    float3 nn = normalize(i.worldNormal);
    if (!isFront) nn = -nn;
    o.gbuffer = SS_PackGBuffer(nn, i.material.x, i.material.y);
    return o;
}
