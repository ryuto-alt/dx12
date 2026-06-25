// Forward.hlsl - PBR Forward Rendering (Cook-Torrance BRDF)
#include "Lighting.hlsli"

// Textures
Texture2D    g_albedo         : register(t0);
Texture2D    g_normalMap      : register(t1);
Texture2D    g_metalRoughness : register(t2);
SamplerState g_sampler        : register(s0);

// Shadow (CSM: Texture2DArray, 1スライス=1カスケード)
Texture2DArray         g_shadowMap     : register(t4);
SamplerComparisonState g_shadowSampler : register(s1);

// IBL (t5,t6,t7 / s2=linear-clamp(mip有), s3=linear-clamp(mipなし))
TextureCube  g_irradianceMap  : register(t5);
TextureCube  g_prefilteredMap : register(t6);
Texture2D    g_brdfLUT        : register(t7);
SamplerState g_iblSampler     : register(s2);  // LINEAR CLAMP（mip有, irradiance/prefiltered 用）
SamplerState g_brdfSampler    : register(s3);  // LINEAR CLAMP（mipなし, LUT 用）

// SSAO（スクリーン空間 AO。フル解像度・同一ビューポート前提でピクセル直読み）
Texture2D<float> g_ssao        : register(t8);
SamplerState     g_ssaoSampler : register(s4);  // POINT CLAMP（未使用だが RootSig 整合のため宣言）

// PerObject constants (b0)
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
};

// PBR Material constants (b2)
cbuffer PBRMaterial : register(b2)
{
    float defaultMetallic;
    float defaultRoughness;
    uint  pbrFlags;       // bit0=hasNormalMap, bit1=hasMetalRoughness
    float _pbrPad;
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
    float4 positionSV   : SV_POSITION;
    float3 worldPos     : TEXCOORD2;
    float3 worldNormal  : NORMAL;
    float3 worldTangent : TANGENT;
    float  tangentW     : TEXCOORD3;
    float4 color        : COLOR;
    float2 texCoord     : TEXCOORD0;
    float  viewDepth    : TEXCOORD4;  // view 空間深度(正値) → カスケード選択用
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV   = mul(float4(input.position, 1.0f), mvp);

    float4 worldPos4    = mul(float4(input.position, 1.0f), model);
    output.worldPos     = worldPos4.xyz;
    output.worldNormal  = normalize(mul(input.normal, (float3x3)model));
    output.worldTangent = normalize(mul(input.tangent.xyz, (float3x3)model));
    output.tangentW     = input.tangent.w;
    output.color        = input.color;
    output.texCoord     = input.texCoord;

    float4 viewPos4     = mul(worldPos4, view);  // LH: 前方 +z
    output.viewDepth    = viewPos4.z;

    return output;
}

// view 空間深度から該当カスケードを選ぶ（cascadeSplitsView.x<y<z<w に遠端深度）
int SelectCascade(float viewDepth)
{
    int c = NUM_CASCADES - 1;
    [unroll]
    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        if (viewDepth <= cascadeSplitsView[i]) { c = i; break; }
    }
    return c;
}

float SampleCascade(int cascade, float3 worldPos)
{
    float4 lc = mul(float4(worldPos, 1.0f), cascadeViewProj[cascade]);
    float3 proj = lc.xyz / lc.w;
    float2 uv = proj.xy * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) return 1.0f;

    // 受光面のシャドウアクネ/ピーターパン調整用の深度バイアス（shadowParams.y）。
    // 比較深度を手前へずらして自己遮蔽を抑える。
    float current = proj.z - shadowParams.y;
    float texel = shadowParams.x;  // 1/shadowMapSize
    float s = 0.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    [unroll]
    for (int x = -2; x <= 2; ++x)
        s += g_shadowMap.SampleCmpLevelZero(g_shadowSampler,
                 float3(uv + float2(x, y) * texel, (float)cascade), current);
    return s / 25.0f;
}

float CalcShadow(float3 worldPos, float viewDepth)
{
    int c = SelectCascade(viewDepth);
    float shadow = SampleCascade(c, worldPos);
    // カスケード境界ブレンド(任意): 次カスケードと線形混合
    float band = shadowParams.z;
    if (band > 0.0f && c < NUM_CASCADES - 1)
    {
        float edge = cascadeSplitsView[c];
        float t = saturate((edge - viewDepth) / max(band, 1e-4));
        if (t < 1.0f)
            shadow = lerp(SampleCascade(c + 1, worldPos), shadow, t);
    }
    return shadow;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // Albedo
    float4 albedo4 = g_albedo.Sample(g_sampler, input.texCoord) * input.color;
    float3 albedo = albedo4.rgb;

    // Normal
    float3 N;
    if (pbrFlags & 1u)
    {
        float3 normalSample = g_normalMap.Sample(g_sampler, input.texCoord).rgb;
        N = PerturbNormal(input.worldNormal, input.worldTangent, input.tangentW, normalSample);
    }
    else
    {
        N = normalize(input.worldNormal);
    }

    // Metallic / Roughness（テクスチャ × スライダー値でスケーリング）
    float metallic, roughness;
    if (pbrFlags & 2u)
    {
        float4 mr = g_metalRoughness.Sample(g_sampler, input.texCoord);
        roughness = mr.g * defaultRoughness;
        metallic  = mr.b * defaultMetallic;
    }
    else
    {
        metallic  = defaultMetallic;
        roughness = defaultRoughness;
    }
    roughness = max(roughness, 0.04);

    // View & Light
    float3 V = normalize(cameraPos - input.worldPos);
    float3 L = normalize(-lightDir);

    // PBR Cook-Torrance BRDF
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    // Shadow (CSM カスケード選択 PCF)
    float shadow = CalcShadow(input.worldPos, input.viewDepth);

    // Directional Light（影付き）
    float3 Lo = ShadePunctual(N, V, L, lightColor * shadow, albedo, F0, metallic, roughness);

    // Point Lights + Spot Lights
    Lo += AccumulatePunctualLights(N, V, input.worldPos, albedo, F0, metallic, roughness);

    // SSAO（スクリーン空間 AO）: フル解像度・同一ビューポートなのでピクセル直読み。
    // SSAO 無効/編集ビューでは白(1.0)がバインドされ素通し。
    float ao = g_ssao.Load(int3(input.positionSV.xy, 0));

    // ===== Ambient / IBL =====
    float3 ambient;
    if (hasIBL != 0u)
    {
        float3 R   = reflect(-V, N);
        float  NoV = max(dot(N, V), 0.0);
        float3 F   = FresnelSchlickRoughness(NoV, F0, roughness);
        float3 kD  = (1.0 - F) * (1.0 - metallic);

        // 拡散 IBL（irradiance）
        float3 irradiance = g_irradianceMap.SampleLevel(g_iblSampler, N, 0).rgb;
        float3 diffuseIBL = irradiance * albedo;

        // 鏡面 IBL（split-sum: prefiltered * (F*scale + bias)）
        float  mip = roughness * maxPrefilterMip;
        float3 prefiltered = g_prefilteredMap.SampleLevel(g_iblSampler, R, mip).rgb;
        float2 envBRDF = g_brdfLUT.SampleLevel(g_brdfSampler, float2(NoV, roughness), 0).rg;
        float3 specularIBL = prefiltered * (F * envBRDF.x + envBRDF.y);

        ambient = (kD * diffuseIBL + specularIBL) * iblIntensity;
    }
    else
    {
        // 従来フォールバック（ライト ambient のみ）
        // metallic は拡散反射を持たない → (1-metallic) でスケール。F0 項は環境反射の簡易近似。
        float3 ambientDiffuse  = albedo * (1.0 - metallic);
        float3 ambientSpecular = F0;
        ambient = ambientStrength * (ambientDiffuse + ambientSpecular);
    }

    // 環境光(IBL/ambient)へ AO を乗算（直接光は遮蔽しない）。
    ambient *= ao;

    float3 color = ambient + Lo;

    // カスケード可視化デバッグ（shadowParams.w>0.5）
    if (shadowParams.w > 0.5f)
    {
        float3 tint[4] = { float3(1,0.4,0.4), float3(0.4,1,0.4), float3(0.4,0.4,1), float3(1,1,0.4) };
        color *= tint[SelectCascade(input.viewDepth)];
    }

    // Tone mapping + gamma
    color = ACESFilm(color);
    color = pow(color, 1.0 / 2.2);

    return float4(color, albedo4.a);
}
