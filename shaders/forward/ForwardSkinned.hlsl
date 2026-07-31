// ForwardSkinned.hlsl - PBR Forward Rendering with Skeletal Animation
#include "Lighting.hlsli"

// Textures
Texture2D    g_albedo         : register(t0);
Texture2D    g_normalMap      : register(t1);
Texture2D    g_metalRoughness : register(t2);
SamplerState g_sampler        : register(s0);

// Bones (t3 - moved from t1)
StructuredBuffer<float4x4> g_bones : register(t3);

// Shadow (CSM: Texture2DArray, 1スライス=1カスケード)。g_shadowSampler(s1)は Lighting.hlsli で共有宣言。
Texture2DArray         g_shadowMap     : register(t4);
// PCSS / 3x3 PCF の共有実装（g_shadowMap と Lighting.hlsli の後で include すること）
#include "ShadowPcss.hlsli"

// IBL (t5,t6,t7 / s2=linear-clamp(mip有), s3=linear-clamp(mipなし))
TextureCube  g_irradianceMap  : register(t5);
TextureCube  g_prefilteredMap : register(t6);
Texture2D    g_brdfLUT        : register(t7);
SamplerState g_iblSampler     : register(s2);  // LINEAR CLAMP（mip有, irradiance/prefiltered 用）
SamplerState g_brdfSampler    : register(s3);  // LINEAR CLAMP（mipなし, LUT 用）

// SSAO（スクリーン空間 AO。フル解像度・同一ビューポート前提でピクセル直読み）
Texture2D<float> g_ssao        : register(t8);
SamplerState     g_ssaoSampler : register(s4);  // POINT CLAMP（未使用だが RootSig 整合のため宣言）

// コンタクトシャドウ（深度バッファのスクリーン空間レイマーチ。1=遮蔽なし）。太陽の寄与へ乗算する。
Texture2D<float> g_contactShadow : register(t11);

// SSR / SSGI（Forward.hlsl と同じ規約。無効時は 1x1 黒ダミー → Load が範囲外で 0 = 寄与ゼロ）
Texture2D<float4> g_ssr  : register(t16);
Texture2D<float4> g_ssgi : register(t17);

// デカール（t18..t21）。★g_sampler(s0) と Lighting.hlsli より後に include すること。
#include "DecalApply.hlsli"

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
    // ★エンティティ毎の一律色ティント（RGB888。0xFFFFFF = 白 = 影響なし）。
    //   共有 Mesh の頂点バッファを塗り替える旧実装だと、同じモデルを使う他のエンティティまで
    //   同じ色になり、シーン読み込みでは最後に読まれた 1 体の色が全員に残っていた。
    //   ★インスタンス描画では白が入り、色は per-instance の input.color 側で掛かる。
    uint  packedTint;
    // UV 変換 (xy=スケール, zw=オフセット)。MeshRenderer の UV スクロール/連番アニメ用。
    float4 uvScaleOffset;
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

float SampleCascade(int cascade, float3 worldPos, float2 svPos)
{
    // ★実体は ShadowPcss.hlsli（PCSS / 3x3 PCF の切替込み。4 つの PS で共有）
    return SampleShadowCascadeCommon(g_shadowMap, cascade, worldPos, svPos, shadowParams.y);
}

float CalcShadow(float3 worldPos, float viewDepth, float2 svPos)
{
    // 正射カメラでは CSM 無効（cascadeSplitsView=1e9 センチネル）。identity フォールバックが
    // 原点付近でゴミ影を落とすため、明示的に無影(1.0)を返す。
    if (cascadeSplitsView.x > 1.0e8) return 1.0f;
    int c = SelectCascade(viewDepth);
    float shadow = SampleCascade(c, worldPos, svPos);
    // カスケード境界ブレンド(任意): 次カスケードと線形混合
    float band = shadowParams.z;
    if (band > 0.0f && c < NUM_CASCADES - 1)
    {
        float edge = cascadeSplitsView[c];
        float t = saturate((edge - viewDepth) / max(band, 1e-4));
        if (t < 1.0f)
            shadow = lerp(SampleCascade(c + 1, worldPos, svPos), shadow, t);
    }
    return shadow;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // UV スクロール/連番アニメ (b2)。無効時は uvScaleOffset=(1,1,0,0) なので恒等変換
    float2 uv = input.texCoord * uvScaleOffset.xy + uvScaleOffset.zw;

    const float3 objectTint = float3((packedTint >> 16) & 0xFF,
                                     (packedTint >>  8) & 0xFF,
                                     (packedTint      ) & 0xFF) / 255.0;
    float4 albedo4 = g_albedo.Sample(g_sampler, uv) * input.color * float4(objectTint, 1.0);
    float3 albedo = albedo4.rgb;

    float3 N;
    if (pbrFlags & 1u)
    {
        float3 normalSample = g_normalMap.Sample(g_sampler, uv).rgb;
        N = PerturbNormal(input.worldNormal, input.worldTangent, input.tangentW, normalSample);
    }
    else
    {
        N = normalize(input.worldNormal);
    }

    float metallic, roughness;
    if (pbrFlags & 2u)
    {
        float4 mr = g_metalRoughness.Sample(g_sampler, uv);
        roughness = mr.g * defaultRoughness;
        metallic  = mr.b * defaultMetallic;
    }
    else
    {
        metallic  = defaultMetallic;
        roughness = defaultRoughness;
    }
    roughness = max(roughness, 0.04);

    // ===== デカール（Forward.hlsl と同一ブロック。片方を直したら必ず両方直す）=====
    float3 decalEmissive = 0.0;
    ApplyDecals(input.worldPos, input.positionSV.xy, input.viewDepth,
                albedo, N, metallic, roughness, decalEmissive);

    float3 V = normalize(cameraPos - input.worldPos);
    float3 L = normalize(-lightDir);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float shadow = CalcShadow(input.worldPos, input.viewDepth, input.positionSV.xy);

    // コンタクトシャドウ（Forward.hlsl と同じ規約。無効時は読まず 1.0）。
    if (contactShadowEnabled > 0.5)
        shadow = min(shadow, g_contactShadow.Load(int3(input.positionSV.xy, 0)));

    // Directional Light（影付き）
    float3 Lo = ShadePunctual(N, V, L, lightColor * shadow, albedo, F0, metallic, roughness);

    // Point Lights + Spot Lights（クラスタードライティング / Forward+。灯数上限なし）
    Lo += AccumulatePunctualLights(N, V, input.worldPos, albedo, F0, metallic, roughness,
                                   input.positionSV.xy);

    // SSAO（スクリーン空間 AO）: フル解像度・同一ビューポートなのでピクセル直読み。
    // aoEnabled=0（SSAO無効/正射カメラ/編集2Dビュー）のときは AO を読まず 1.0（白ダミー1x1の範囲外Load=0対策）。
    float ao = (aoEnabled > 0.5) ? g_ssao.Load(int3(input.positionSV.xy, 0)) : 1.0;

    // SSR / SSGI（無効時は 1x1 黒ダミー → Load が範囲外で 0 = 寄与ゼロ）
    //   ssr .rgb=反射放射輝度 / .a=confidence
    //   ssgi.rgb=半球の間接放射照度（IBL ミスフォールバック込み） / .a=有効度
    float4 ssr  = g_ssr.Load(int3(input.positionSV.xy, 0));
    float4 ssgi = g_ssgi.Load(int3(input.positionSV.xy, 0));
    float  ssrConf  = saturate(ssr.a);
    float  ssgiConf = saturate(ssgi.a);

    // ===== Ambient / IBL =====
    float3 ambient;
    if (hasIBL != 0u)
    {
        float3 R   = reflect(-V, N);
        float  NoV = max(dot(N, V), 0.0);
        float3 F   = FresnelSchlickRoughness(NoV, F0, roughness);
        float3 kD  = (1.0 - F) * (1.0 - metallic);

        // 拡散 IBL（irradiance）
        // ★SSGI は irradiance を「置き換える」（足さない）。SSGI はミスしたレイに
        //   IBL の irradiance を積んでいるので、足すと同じ光を二重に数えて全体が倍明るくなる。
        //   SSGI が無効/無効ピクセルでは ssgiConf=0 で完全に従来どおり。
        float3 irradiance = g_irradianceMap.SampleLevel(g_iblSampler, N, 0).rgb;
        irradiance = lerp(irradiance, ssgi.rgb, ssgiConf);
        float3 diffuseIBL = irradiance * albedo;

        // 鏡面 IBL（split-sum: prefiltered * (F*scale + bias)）。SSR がヒットしていれば置換。
        float  mip = roughness * maxPrefilterMip;
        float3 prefiltered = g_prefilteredMap.SampleLevel(g_iblSampler, R, mip).rgb;
        prefiltered = lerp(prefiltered, ssr.rgb, ssrConf);
        float2 envBRDF = g_brdfLUT.SampleLevel(g_brdfSampler, float2(NoV, roughness), 0).rg;
        float3 specularIBL = prefiltered * (F * envBRDF.x + envBRDF.y);

        // AO は SSGI/SSR が担当していない分にだけ掛ける（Forward.hlsl と同じ規則）。
        float aoDiff = lerp(ao, 1.0, ssgiConf);
        float aoSpec = lerp(ao, 1.0, ssrConf);
        ambient = (kD * diffuseIBL * aoDiff + specularIBL * aoSpec) * iblIntensity;
    }
    else
    {
        // 従来フォールバック（ライト ambient のみ）
        float3 ambientDiffuse  = albedo * (1.0 - metallic);
        float3 ambientSpecular = lerp(F0, ssr.rgb, ssrConf);
        ambient = ambientStrength * (ambientDiffuse + ambientSpecular) * ao;
        ambient = lerp(ambient,
                       ssgi.rgb * ambientDiffuse + ambientStrength * ambientSpecular * ao,
                       ssgiConf);
    }

    float3 color = ambient + Lo + decalEmissive;

    // カスケード可視化デバッグ（shadowParams.w>0.5）
    if (shadowParams.w > 0.5f)
    {
        float3 tint[4] = { float3(1,0.4,0.4), float3(0.4,1,0.4), float3(0.4,0.4,1), float3(1,1,0.4) };
        color *= tint[SelectCascade(input.viewDepth)];
    }

    // クラスタ可視化デバッグ（clusterExtra.z: 1=ライト複雑度ヒートマップ / 2=クラスタ境界）
    color = ApplyClusterDebug(color, input.positionSV.xy, input.worldPos);
    // デカール枚数ヒートマップ（clusterExtra.z == 3。dx12_render_debug decalCount）
    color = ApplyDecalDebug(color, input.positionSV.xy, input.worldPos);

    // リニア HDR のまま scene RT へ出力（トーンマップ+ガンマは PostProcess 最終段）
    return float4(color, albedo4.a);
}
