// forward_shade_probe.hlsl - マテリアルグラフ G2b の ForwardGraph.hlsl を模した「予行」（テスト専用）。
//
//   G2b が作るもの（バインドレスでパラメータプール / テクスチャを引き、UnoShadeForward でライティングする PS）と
//   同じ形を、G2a の時点でコンパイル・PSO 作成できることを確かめるために置いてある。
//     ・ps_6_6 + ResourceDescriptorHeap[]（メイン RS の CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED が要る）
//     ・b2 の 9 DWORD を「recordBase / poolSrvIndex / …」へ読み替える（地形が TerrainMaterial として読み替える前例と同じ）
//     ・法線は UnoNormalFromTangentSpace（PerturbNormal と同じ z >= 0.35 ガード）→ UnoShadeForward
//   使うのは tests/forward_shade_test.cpp（DXC コンパイル）と tests/main_rs_bindless_test.cpp（PSO 作成）。
//   エンジンの本番シェーダーではない（assets/shaders には置かない）。

#include "forward/Lighting.hlsli"

SamplerState g_sampler : register(s0);

Texture2DArray g_shadowMap : register(t4);
#include "forward/ShadowPcss.hlsli"

TextureCube  g_irradianceMap  : register(t5);
TextureCube  g_prefilteredMap : register(t6);
Texture2D    g_brdfLUT        : register(t7);
SamplerState g_iblSampler     : register(s2);
SamplerState g_brdfSampler    : register(s3);

Texture2D<float>  g_ssao         : register(t8);
Texture2D<float>  g_contactShadow: register(t11);
Texture2D<float4> g_ssr          : register(t16);
Texture2D<float4> g_ssgi         : register(t17);

#include "forward/DecalApply.hlsli"
#include "forward/ForwardShade.hlsli"

// b2（9 DWORD）の読み替え（G2b の設計: 設計書 §3.6）
cbuffer GraphMaterial : register(b2)
{
    uint   recordBase;      // パラメータプール内のレコード先頭（float4 単位）
    uint   poolSrvIndex;    // プール（StructuredBuffer<float4>）の SRV 添字
    uint   graphFlags;
    uint   packedTint;
    float4 uvScaleOffset;
    uint   packedEmissive;
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

float4 PSMain(PSInput input) : SV_TARGET
{
    // ★フォワードではドロー内で一様なので NonUniformResourceIndex は付けない（RtBindless.hlsli と同じ方針）
    StructuredBuffer<float4> pool = ResourceDescriptorHeap[poolSrvIndex];
    const float4 rec = pool[recordBase];
    Texture2D<float4> tex = ResourceDescriptorHeap[asuint(rec.x)];
    const float4 c = tex.Sample(g_sampler, input.texCoord * uvScaleOffset.xy + uvScaleOffset.zw);

    UnoSurface s = UnoSurfaceDefault();
    s.baseColor = c.rgb * input.color.rgb;
    s.opacity   = c.a;
    s.roughness = pool[recordBase + 1u].x;
    s.hasNormal = true;
    s.normalTS  = float3(0.1, 0.1, 0.99);

    const float3 N = s.hasNormal
        ? UnoNormalFromTangentSpace(input.worldNormal, input.worldTangent, input.tangentW, s.normalTS)
        : normalize(input.worldNormal);

    return UnoShadeForward(s, N,
        UnoMakeShadeInput(input.worldPos, input.worldNormal, input.positionSV, input.viewDepth));
}
