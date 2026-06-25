// ForwardSkinned.hlsl - PBR Forward Rendering with Skeletal Animation
#include "Lighting.hlsli"

// Textures
Texture2D    g_albedo         : register(t0);
Texture2D    g_normalMap      : register(t1);
Texture2D    g_metalRoughness : register(t2);
SamplerState g_sampler        : register(s0);

// Bones (t3 - moved from t1)
StructuredBuffer<float4x4> g_bones : register(t3);

// Shadow (CSM: Texture2DArray, 1スライス=1カスケード)
Texture2DArray         g_shadowMap     : register(t4);
SamplerComparisonState g_shadowSampler : register(s1);

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
    uint  pbrFlags;
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

    // GPU Skinning (Linear Blend Skinning)
    float4x4 skinMatrix =
        input.boneWeights.x * g_bones[input.boneIndices.x] +
        input.boneWeights.y * g_bones[input.boneIndices.y] +
        input.boneWeights.z * g_bones[input.boneIndices.z] +
        input.boneWeights.w * g_bones[input.boneIndices.w];

    float4 skinnedPos = mul(float4(input.position, 1.0f), skinMatrix);
    float3 skinnedNormal = normalize(mul(input.normal, (float3x3)skinMatrix));
    float3 skinnedTangent = normalize(mul(input.tangent.xyz, (float3x3)skinMatrix));

    output.positionSV   = mul(skinnedPos, mvp);
    float4 worldPos4    = mul(skinnedPos, model);
    output.worldPos     = worldPos4.xyz;
    output.worldNormal  = normalize(mul(skinnedNormal, (float3x3)model));
    output.worldTangent = normalize(mul(skinnedTangent, (float3x3)model));
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

    float current = proj.z;
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
    float4 albedo4 = g_albedo.Sample(g_sampler, input.texCoord) * input.color;
    float3 albedo = albedo4.rgb;

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

    float3 V = normalize(cameraPos - input.worldPos);
    float3 L = normalize(-lightDir);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float shadow = CalcShadow(input.worldPos, input.viewDepth);

    // Directional Light（影付き）
    float3 Lo = ShadePunctual(N, V, L, lightColor * shadow, albedo, F0, metallic, roughness);

    // Point Lights + Spot Lights
    Lo += AccumulatePunctualLights(N, V, input.worldPos, albedo, F0, metallic, roughness);

    // Ambient: metallic は拡散反射を持たない → (1-metallic) でスケール
    // F0 項は環境反射の簡易近似（IBL 未実装のため F0 をそのまま使用）
    float3 ambientDiffuse  = albedo * (1.0 - metallic);
    float3 ambientSpecular = F0;
    float3 ambient = ambientStrength * (ambientDiffuse + ambientSpecular);

    float3 color = ambient + Lo;

    // カスケード可視化デバッグ（shadowParams.w>0.5）
    if (shadowParams.w > 0.5f)
    {
        float3 tint[4] = { float3(1,0.4,0.4), float3(0.4,1,0.4), float3(0.4,0.4,1), float3(1,1,0.4) };
        color *= tint[SelectCascade(input.viewDepth)];
    }

    color = ACESFilm(color);
    color = pow(color, 1.0 / 2.2);

    return float4(color, albedo4.a);
}
