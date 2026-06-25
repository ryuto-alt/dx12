// Lighting.hlsli - 共有ライティング定義
// PerFrame 定数バッファ（b1）と、点光源 / スポットライトの Cook-Torrance 累積。
// Forward.hlsl と ForwardSkinned.hlsl の両方から include し、レイアウトを一致させる。
// C++ 側 Application.cpp の FrameConstants と完全一致させること。

#ifndef LIGHTING_HLSLI
#define LIGHTING_HLSLI

#include "PBR.hlsli"

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS  8
#define NUM_CASCADES     4

struct PointLightData
{
    float3 position;  float range;
    float3 color;     float _pad;     // color はあらかじめ intensity 乗算済み
};

struct SpotLightData
{
    float3 position;  float range;
    float3 direction; float cosInner; // cos(内側コーン半角)
    float3 color;     float cosOuter; // cos(外側コーン半角)。color は intensity 乗算済み
};

// PerFrame constants (b1)
// CSM 対応で lightViewProj(64B) を cascadeViewProj[4](256B) + cascadeSplitsView(16B) + shadowParams(16B) へ拡張。
// IBL 対応で末尾に iblParams(16B) を追加。
// C++ Application.cpp の FrameConstants(1136B) とバイト単位で一致させること。
cbuffer PerFrameConstants : register(b1)
{
    float4x4 view;                               // 64B  (offset   0)
    float4x4 proj;                               // 64B  (offset  64)
    float3   lightDir;        float time;        // 16B  (offset 128)
    float3   lightColor;      float ambientStrength; // 16B (offset 144)
    float4x4 cascadeViewProj[NUM_CASCADES];      // 256B (offset 160)
    float4   cascadeSplitsView;                  // 16B  (offset 416)  各カスケード遠端の view 空間深度(正値) .x..w
    float4   shadowParams;                       // 16B  (offset 432)  .x=1/shadowMapSize .y=depthBias .z=blendBand .w=showCascadeDebug
    float3   cameraPos;       float _pfpad0;     // 16B  (offset 448)
    uint     numPointLights;  uint  numSpotLights;  float2 _pfpad1; // 16B (offset 464)
    PointLightData pointLights[MAX_POINT_LIGHTS]; // 256B (offset 480)
    SpotLightData  spotLights[MAX_SPOT_LIGHTS];   // 384B (offset 736)
    // ▼ IBL 制御 16B (offset 1120)
    float  iblIntensity;     // IBL 拡散/反射の全体スケール
    float  maxPrefilterMip;  // prefiltered cube の最大 mip index（=4.0）
    uint   hasIBL;           // 1=IBL テクスチャ有効, 0=従来 ambient フォールバック
    float  skyboxIntensity;  // skybox 描画/反射の明るさ
};                                               // total = 1136B

// 1 灯ぶんの Cook-Torrance 寄与。L は正規化済みでライトへ向かうベクトル、
// radiance はライト色（減衰・コーン・影を乗じたもの）。
float3 ShadePunctual(float3 N, float3 V, float3 L, float3 radiance,
                     float3 albedo, float3 F0, float metallic, float roughness)
{
    float3 H = normalize(V + L);
    float  NdotV = max(dot(N, V), 0.001);
    float  NdotL = max(dot(N, L), 0.0);

    float  NDF = DistributionGGX(N, H, roughness);
    float  G   = GeometrySmith(N, V, L, roughness);
    float3 F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

    float3 kD   = (1.0 - F) * (1.0 - metallic);
    float3 spec = (NDF * G * F) / (4.0 * NdotV * NdotL + 0.0001);

    return (kD * albedo / PI + spec) * radiance * NdotL;
}

// 点光源 + スポットライトをまとめて累積する。
float3 AccumulatePunctualLights(float3 N, float3 V, float3 worldPos,
                                float3 albedo, float3 F0, float metallic, float roughness)
{
    float3 Lo = 0.0;

    // --- Point Lights ---
    [loop]
    for (uint i = 0; i < min(numPointLights, (uint)MAX_POINT_LIGHTS); ++i)
    {
        float3 d    = pointLights[i].position - worldPos;
        float  dist = length(d);
        float3 L    = d / max(dist, 0.0001);

        float att = saturate(1.0 - dist / pointLights[i].range);
        att *= att;

        Lo += ShadePunctual(N, V, L, pointLights[i].color * att,
                            albedo, F0, metallic, roughness);
    }

    // --- Spot Lights ---
    [loop]
    for (uint j = 0; j < min(numSpotLights, (uint)MAX_SPOT_LIGHTS); ++j)
    {
        float3 d    = spotLights[j].position - worldPos;
        float  dist = length(d);
        float3 L    = d / max(dist, 0.0001);

        float att = saturate(1.0 - dist / spotLights[j].range);
        att *= att;

        // コーン減衰: スポット軸と (-L) の角度を inner..outer でフェード
        float cd   = dot(normalize(spotLights[j].direction), -L);
        float cone = saturate((cd - spotLights[j].cosOuter) /
                              max(spotLights[j].cosInner - spotLights[j].cosOuter, 0.001));
        cone *= cone;

        Lo += ShadePunctual(N, V, L, spotLights[j].color * att * cone,
                            albedo, F0, metallic, roughness);
    }

    return Lo;
}

#endif // LIGHTING_HLSLI
