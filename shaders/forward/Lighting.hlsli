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
#define MAX_SHADOW_SPOT  4   // 影を落とせるスポットライトの同時上限（castShadows かつカメラ近い順）
#define MAX_SHADOW_POINT 2   // 影を落とせるポイントライトの同時上限（6面/灯）

struct PointLightData
{
    float3 position;  float range;
    float3 color;     float shadowIndex;  // color はあらかじめ intensity 乗算済み。shadowIndex: -1=影なし、それ以外はキューブ配列のインデックス
};

struct SpotLightData
{
    float3 position;  float range;
    float3 direction; float cosInner;    // cos(内側コーン半角)
    float3 color;     float cosOuter;    // cos(外側コーン半角)。color は intensity 乗算済み
    float  shadowIndex; float3 _spad;    // shadowIndex: -1=影なし、それ以外は spotShadowMatrix[] のインデックス
};

// スポット/ポイント影サンプリング用（Forward.hlsl / ForwardSkinned.hlsl の両方で s1 を共有するため
// ここに一元化。CSM の g_shadowMap も同サンプラーを使う。
SamplerComparisonState g_shadowSampler       : register(s1);
Texture2DArray         g_spotShadowMap       : register(t9);   // ArraySize=MAX_SHADOW_SPOT
TextureCubeArray       g_pointShadowMap      : register(t10);  // NumCubes=MAX_SHADOW_POINT

// PerFrame constants (b1)
// CSM 対応で lightViewProj(64B) を cascadeViewProj[4](256B) + cascadeSplitsView(16B) + shadowParams(16B) へ拡張。
// IBL 対応で iblParams(16B) を追加。スポット/ポイント影対応で shadowIndex・spotShadowMatrix[]・
// spotShadowTexel/pointShadowNear を追加。コンタクトシャドウ対応で末尾に 16B を追加。
// C++ Application.cpp の FrameConstants(1536B) とバイト単位で一致させること。
cbuffer PerFrameConstants : register(b1)
{
    float4x4 view;                               // 64B  (offset   0)
    float4x4 proj;                               // 64B  (offset  64)
    float3   lightDir;        float time;        // 16B  (offset 128)
    float3   lightColor;      float ambientStrength; // 16B (offset 144)
    float4x4 cascadeViewProj[NUM_CASCADES];      // 256B (offset 160)
    float4   cascadeSplitsView;                  // 16B  (offset 416)  各カスケード遠端の view 空間深度(正値) .x..w
    float4   shadowParams;                       // 16B  (offset 432)  .x=1/shadowMapSize .y=depthBias .z=blendBand .w=showCascadeDebug
    float3   cameraPos;       float aoEnabled;   // 16B  (offset 448)  aoEnabled: 1=実AOを読む, 0=AO読まず ao=1
    uint     numPointLights;  uint  numSpotLights;
    float    spotShadowTexel; float pointShadowNear;          // 16B (offset 464)
    PointLightData pointLights[MAX_POINT_LIGHTS]; // 256B (offset 480)
    SpotLightData  spotLights[MAX_SPOT_LIGHTS];   // 512B (offset 736)
    float4x4 spotShadowMatrix[MAX_SHADOW_SPOT];   // 256B (offset 1248)
    // ▼ IBL 制御 16B (offset 1504)
    float  iblIntensity;     // IBL 拡散/反射の全体スケール
    float  maxPrefilterMip;  // prefiltered cube の最大 mip index（=4.0）
    uint   hasIBL;           // 1=IBL テクスチャ有効, 0=従来 ambient フォールバック
    float  skyboxIntensity;  // skybox 描画/反射の明るさ
    // ▼ コンタクトシャドウ制御 16B (offset 1520)
    float  contactShadowEnabled;  // 1=実テクスチャ(t11)を読む, 0=読まず 1.0（白ダミー 1x1 の範囲外 Load 対策）
    float3 _csPad;
};                                               // total = 1536B

// スポットライト影: spotShadowMatrix[idx] で射影し 3x3 PCF（CSM の SampleCascade と同じ流儀）。
float SampleSpotShadow(int idx, float3 worldPos)
{
    float4 clip = mul(float4(worldPos, 1.0), spotShadowMatrix[idx]);
    if (clip.w <= 0.0) return 1.0;
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;

    float current = ndc.z - shadowParams.y;
    float texel = spotShadowTexel;
    float s = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            s += g_spotShadowMap.SampleCmpLevelZero(g_shadowSampler,
                float3(uv + float2(x, y) * texel, (float)idx), current);
    return s / 9.0;
}

// ポイントライト影: major-axis 距離から比較深度を再構成して SampleCmpLevelZero 1発。
// fromLight = worldPos - lightPos（ライトからピクセルへの方向、正規化不要）。range = ライトのrange(=far)。
float SamplePointShadow(int idx, float3 fromLight, float range)
{
    float dist = max(max(abs(fromLight.x), abs(fromLight.y)), abs(fromLight.z));
    float n = pointShadowNear;
    float f = max(range, n + 0.01);
    // 透視深度の再構成: z = f/(f-n) - (f*n)/(f-n)/dist  (D3D の 0..1 深度規約, LH)
    float current = (f / (f - n)) - (f * n) / (f - n) / max(dist, 0.0001) - shadowParams.y;
    return g_pointShadowMap.SampleCmpLevelZero(g_shadowSampler,
        float4(fromLight, (float)idx), current);
}

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

        float shadow = 1.0;
        if (pointLights[i].shadowIndex >= 0.0)
            shadow = SamplePointShadow((int)pointLights[i].shadowIndex, -d, pointLights[i].range);

        Lo += ShadePunctual(N, V, L, pointLights[i].color * att * shadow,
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

        float shadow = 1.0;
        if (spotLights[j].shadowIndex >= 0.0)
            shadow = SampleSpotShadow((int)spotLights[j].shadowIndex, worldPos);

        Lo += ShadePunctual(N, V, L, spotLights[j].color * att * cone * shadow,
                            albedo, F0, metallic, roughness);
    }

    return Lo;
}

#endif // LIGHTING_HLSLI
