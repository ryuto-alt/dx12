// ForwardSkinned.hlsl - PBR Forward Rendering with Skeletal Animation
#include "Lighting.hlsli"

// Textures
Texture2D    g_albedo         : register(t0);
Texture2D    g_normalMap      : register(t1);
Texture2D    g_metalRoughness : register(t2);
SamplerState g_sampler        : register(s0);

// 自己発光（emissive、t24）。★材質 SRV ブロックの 4 枚目。
//   レジスタは t0,t1,t2 から t24 へ飛んでいるが、ルートシグネチャの材質テーブルは
//   OFFSET_APPEND なので**ヒープ上は t2 の直後**（graphics/RootSignature.cpp 参照）。
//   t3..t23 は骨/影/IBL/SSAO/クラスタ/デカール/DDGI で埋まっていて、空きが t24 だけだった。
//   ★pbrFlags bit3 が立っていないときは絶対にサンプルしないこと（材質ブロックを持たない
//     フォールバック描画では 4 枚目が別物のディスクリプタになる）。
Texture2D    g_emissive       : register(t24);

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
    // bit0=hasNormalMap, bit1=hasMetalRoughness, bit2=アルファテスト有効, bit3=emissive テクスチャ有り,
    // bit4=MR の R をマテリアル AO として読む, bit16..23=AO 強度(8bit),
    // bit8..15=alphaCutoff(8bit 量子化)。詳細は renderer/Material.h。
    uint  pbrFlags;
    // ★エンティティ毎の一律色ティント（RGB888。0xFFFFFF = 白 = 影響なし）。
    //   共有 Mesh の頂点バッファを塗り替える旧実装だと、同じモデルを使う他のエンティティまで
    //   同じ色になり、シーン読み込みでは最後に読まれた 1 体の色が全員に残っていた。
    //   ★インスタンス描画では白が入り、色は per-instance の input.color 側で掛かる。
    // ★上位 8bit(bit24..31) は「不透明度(opacity)」を 8bit 量子化したもの。255 = 不透明。
    //   ルート定数の予算が 61/64 で埋まっていて float を足せないため、空きバイトへ詰めてある
    //   （b2 のバイトレイアウトは 1 バイトも変わっていない）。詳細は renderer/Material.h。
    uint  packedTint;
    // UV 変換 (xy=スケール, zw=オフセット)。MeshRenderer の UV スクロール/連番アニメ用。
    float4 uvScaleOffset;
    // 自己発光（9 本目のルート定数）。詰め方の正は renderer/Material.h の PackEmissive()。
    uint  packedEmissive;
};

// packedEmissive（b2）を放射輝度へ戻す。CPU 側 renderer/Material.h の PackEmissive と対。
// ★0 なら丸ごと 0 を返す＝ emissive を持たないマテリアルは 1 ピクセルも変わらない。
float3 UnpackEmissive(uint packed, float2 uv)
{
    if (packed == 0u) return 0.0;
    const float3 c = float3((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF) / 255.0;
    // 強度は二乗で伸長（0 付近を細かく、ブルームに要る 1 超の領域まで 8bit で届かせるため）
    const float n = float((packed >> 24) & 0xFF) / 255.0;
    float3 e = c * (n * n * 64.0);
    if (pbrFlags & 8u) e *= g_emissive.Sample(g_sampler, uv).rgb;
    return e;
}

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

// ===== シェーディング尾部（デカール → 影 → ライティング → IBL → 自己発光 → デバッグ）=====
// ★実体は ForwardShade.hlsli（Forward / ForwardSkinned / Terrain / マテリアルグラフで共有）。
//   上のリソース宣言・Lighting.hlsli・ShadowPcss.hlsli・DecalApply.hlsli より後で include すること。
//   SelectCascade / CalcShadow もそこにある。
#include "ForwardShade.hlsli"

float4 PSMain(PSInput input) : SV_TARGET
{
    // UV スクロール/連番アニメ (b2)。無効時は uvScaleOffset=(1,1,0,0) なので恒等変換
    float2 uv = input.texCoord * uvScaleOffset.xy + uvScaleOffset.zw;

    // ===== マテリアル評価（Forward.hlsl と同一。片方を直したら必ず両方直す）=====
    const float3 objectTint = float3((packedTint >> 16) & 0xFF,
                                     (packedTint >>  8) & 0xFF,
                                     (packedTint      ) & 0xFF) / 255.0;
    float4 albedo4 = g_albedo.Sample(g_sampler, uv) * input.color * float4(objectTint, 1.0);
    float3 albedo = albedo4.rgb;

    // ===== 透明（アルファクリップ / アルファブレンド）=====
    // ALPHA_TEST は -D ALPHA_TEST=1 でコンパイルした **専用バリアント**でのみ有効
    // （clip を含む PS は early-Z が効かなくなるので、不透明パスの PS には絶対に混ぜない）。
    // cutoff は b2 の pbrFlags bit8..15 に 8bit 量子化して載っている。
#ifdef ALPHA_TEST
    {
        const float cutoff = float((pbrFlags >> 8) & 0xFF) / 255.0;
        clip(albedo4.a - cutoff);
    }
#endif
    // 不透明度。BLEND 用 PSO でのみ意味を持つ（不透明 PSO はブレンド無効なので無視される）。
    // ★不透明時は 255 が入る。0 が入ると全部消えるので CPU 側は必ず書くこと。
    const float gOpacity = float((packedTint >> 24) & 0xFF) / 255.0;

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

    // ★ラフネスの下限 0.04 は UnoShadeLighting の中で掛かる。
    float metallic, roughness;
    // マテリアル AO（bit4 = MR の R が AO。bit16..23 = 強度）。1.0 = 遮蔽なし＝AO を持たないモデルは従来と同じ。
    float materialAo = 1.0;
    if (pbrFlags & 2u)
    {
        float4 mr = g_metalRoughness.Sample(g_sampler, uv);
        roughness = mr.g * defaultRoughness;
        metallic  = mr.b * defaultMetallic;
        if (pbrFlags & 16u)
            materialAo = 1.0 + (float((pbrFlags >> 16) & 0xFF) / 255.0) * (mr.r - 1.0);
    }
    else
    {
        metallic  = defaultMetallic;
        roughness = defaultRoughness;
    }

    UnoSurface s = UnoSurfaceDefault();
    s.baseColor = albedo;
    s.metallic  = metallic;
    s.roughness = roughness;
    s.ao        = materialAo;   // 間接光だけに掛かる（ForwardShade.hlsli: ao *= s.ao）

    // ★以降（デカール / 法線フィルタ / 影 / ライティング / IBL / **DDGI** / 自己発光 / デバッグ）は
    //   ForwardShade.hlsli の UnoShadeLighting / UnoShadeFinish。かつては Forward.hlsl の複製で、DDGI の追加が
    //   スキンドにだけ抜けて「キャラだけ GI が乗らない」不具合を出した。共有したのでその事故は起きない。
    const UnoShadeInput si = UnoMakeShadeInput(input.worldPos, input.worldNormal,
                                               input.positionSV, input.viewDepth);
    const float3 lit = UnoShadeLighting(s, N, si);

    // ===== 自己発光（emissive）=====
    // ★影・AO・ライティングを一切通さずそのまま足す（デカールの emissive と同じ扱い）。
    //   リニア HDR のまま出すので、1 を超えた分はブルームがそのまま拾う。
    //   ★サンプルはライティングの後ろに置く（外出し前の命令順を保つため。UnoShadeForward と結果は同じ）。
    const float3 color = lit + UnpackEmissive(packedEmissive, uv);

    return float4(UnoShadeFinish(color, si), albedo4.a * gOpacity);
}
