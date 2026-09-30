// ForwardGraph.hlsl - マテリアルグラフ（G2b）のフォワード PS / VS の枠。
//
//   グラフが生成する HLSL（UnoMatEval を定義する 1 ファイル。docs/MATGRAPH_FORMAT.md §7）は、先頭でこのファイルを
//   include して、UnoMatEval だけを書く。VSMain / PSMain / cbuffer / リソース宣言はこちらに【固定】で持つ
//   （生成物に cbuffer を書かせない = b0 衝突の罠の回避。設計書 §3.5）。
//
//   ★このファイルはビルド時に .cso にならない（グラフごとに UnoMatEval が違うので、GraphMaterialSystem が
//     生成 HLSL と一緒に実行時に DXC でコンパイルする。ps_6_6 + -HV 2021）。
//   ★メイン RS（graphics/RootSignature.cpp）のバインドレスフラグ（CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED。G2a）が前提。
//   ★シェーディング尾部は ForwardShade.hlsli（Forward / Skinned / Terrain と共通）。ここは「材質評価 → 尾部」だけ。
//
// パラメータプール（設計書 §3.6・ルート DWORD +0）:
//   材質 1 個 = プール（StructuredBuffer<float4>）内の連続した float4 = レコード。プール自体は
//   ResourceDescriptorHeap[poolSrvIndex]。テクスチャはスロットに SRV 添字が入っていて Tex(slot) で引く。
//   b2（9 DWORD）を読み替える（地形が TerrainMaterial として読み替える前例と同じ。バイトレイアウトは不変）:
//     DWORD0 recordBase    レコード先頭（float4 単位）      ← 従来の defaultMetallic の位置
//     DWORD1 poolSrvIndex  プールの SRV 添字               ← 従来の defaultRoughness の位置
//     DWORD2 graphFlags    予約（0）                        ← 従来の pbrFlags の位置
//     DWORD3 packedTint    エンティティの色ティント RGB888 + 上位 8bit opacity（従来と同じ詰め方）
//     DWORD4-7 uvScaleOffset  UV スクロール / 連番アニメ（従来と同じ。UnoMatInput.uv に掛かる）
//     DWORD8 packedEmissive  エンティティ側の発光上書き（従来と同じ詰め方。テクスチャ無し）。グラフの Emissive に加算
//
// 契約の参照実装: src/renderer/matgraph/hlsl/UnoMatContractRef.hlsli（G1。名前と意味は同じ）。

#include "forward/Lighting.hlsli"

// ---- リソース宣言（t/s 番号は graphics/RootSignature.cpp が正。Forward.hlsl と同じ）----------------
SamplerState g_sampler : register(s0);   // ANISO WRAP

Texture2DArray g_shadowMap : register(t4);
#include "forward/ShadowPcss.hlsli"

TextureCube  g_irradianceMap  : register(t5);
TextureCube  g_prefilteredMap : register(t6);
Texture2D    g_brdfLUT        : register(t7);
SamplerState g_iblSampler     : register(s2);  // LINEAR CLAMP（mip 有）
SamplerState g_brdfSampler    : register(s3);  // LINEAR CLAMP（mip なし）

Texture2D<float> g_ssao        : register(t8);
SamplerState     g_ssaoSampler : register(s4);  // POINT CLAMP

Texture2D<float> g_contactShadow : register(t11);
Texture2D<float4> g_ssr  : register(t16);
Texture2D<float4> g_ssgi : register(t17);

#include "forward/DecalApply.hlsli"

// s6..s8: マテリアルグラフ専用の静的サンプラ（DWORD を消費しない）
SamplerState g_unoSampLinearWrap : register(s6);   // LINEAR WRAP（mip 有）
SamplerState g_unoSampAnisoClamp : register(s7);   // ANISO(8) CLAMP（mip 有）
SamplerState g_unoSampPointWrap  : register(s8);   // POINT WRAP

// TextureSample.samplerState と 1 対 1（G1 の契約 / docs/MATGRAPH_G2A.md §3）
#define UNO_SAMP_ANISO_WRAP    g_sampler
#define UNO_SAMP_ANISO_CLAMP   g_unoSampAnisoClamp
#define UNO_SAMP_LINEAR_WRAP   g_unoSampLinearWrap
#define UNO_SAMP_LINEAR_CLAMP  g_iblSampler
#define UNO_SAMP_POINT_WRAP    g_unoSampPointWrap
#define UNO_SAMP_POINT_CLAMP   g_ssaoSampler

// 生成コードは Sample を直接書かず、必ずこのマクロを通す（VG resolve が SampleGrad へ差し替える点）。
#define UnoSample(tex, samp, uv)            (tex).Sample((samp), (uv))
#define UnoSampleBias(tex, samp, uv, bias)  (tex).SampleBias((samp), (uv), (bias))

#include "forward/ForwardShade.hlsli"

// ---- 定数バッファ ----------------------------------------------------------------------
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
    // ★ObjectParameter ノード（G3）用の互換: b0 の後ろ 8 float（作者の自由枠）。UnoCustom.hlsli の cbuffer とバイト単位で同じレイアウト
    //   （effectValue 128 / shaderParamsB 132 / shaderParams 144）。Lua の scene:setMeshEffect / setMeshParams・Trigger の SetShaderParam・
    //   tween が MeshRenderer 経由で書く値がそのまま届く（drawEntity は従来どおり 40 DWORD を送る）。使わなければコンパイルで消える。
    float    effectValue;
    float3   shaderParamsB;
    float4   shaderParams;
};

// b2 の読み替え（上のコメント）。Forward.hlsl の cbuffer PBRMaterial とバイトレイアウトは同一（9 DWORD）。
cbuffer GraphMaterial : register(b2)
{
    uint   recordBase;
    uint   poolSrvIndex;
    uint   graphFlags;
    uint   packedTint;
    float4 uvScaleOffset;
    uint   packedEmissive;
};

// ---- パラメータプール --------------------------------------------------------------------
// 生成コードが呼ぶメソッド: F1 / F2 / F3 / F4 / Tex（名前と意味は G1 の契約）。
// ★ドロー内で添字が一様なので NonUniformResourceIndex は付けない（RtBindless.hlsli と同じ方針）。
struct UnoMatPool
{
    uint poolSrv;
    uint recordBase;

    float4 Raw(uint slot)
    {
        StructuredBuffer<float4> p = ResourceDescriptorHeap[poolSrv];
        return p[recordBase + slot];
    }
    float  F1(uint slot) { return Raw(slot).x; }
    float2 F2(uint slot) { return Raw(slot).xy; }
    float3 F3(uint slot) { return Raw(slot).xyz; }
    float4 F4(uint slot) { return Raw(slot); }
    Texture2D<float4> Tex(uint slot)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[asuint(Raw(slot).x)];
        return t;
    }
};

// 生成コードが定義する（UnoMatEval）
void UnoMatEval(UnoMatInput mi, UnoMatPool pool, out UnoSurface s);

// ---- 頂点 / ピクセル入出力（Forward.hlsl と同一）-----------------------------------------------
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
    float  viewDepth    : TEXCOORD4;
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

// packedEmissive（エンティティの発光上書き。テクスチャ無し）。Forward.hlsl の UnpackEmissive と同じ復元。
float3 UnoUnpackEntityEmissive(uint packed)
{
    if (packed == 0u) return 0.0;
    const float3 c = float3((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF) / 255.0;
    const float n = float((packed >> 24) & 0xFF) / 255.0;
    return c * (n * n * 64.0);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // ===== マテリアル評価（グラフ）=====
    UnoMatInput mi;
    mi.uv              = input.texCoord * uvScaleOffset.xy + uvScaleOffset.zw;   // 従来と同じ UV 変換（スクロール / 連番）
    mi.worldPos        = input.worldPos;
    mi.vertexNormalWS  = normalize(input.worldNormal);
    mi.vertexTangentWS = input.worldTangent;
    mi.tangentW        = input.tangentW;
    mi.vertexColor     = input.color;
    mi.svPos           = input.positionSV;
    mi.cameraPos       = cameraPos;
    mi.time            = time;

    UnoMatPool pool;
    pool.poolSrv    = poolSrvIndex;
    pool.recordBase = recordBase;

    UnoSurface s;
    UnoMatEval(mi, pool, s);

    // エンティティ単位の色ティント（従来と同じ b2 の詰め方。0xFFFFFF = 白 = 影響なし）と発光の上書き
    const float3 objectTint = float3((packedTint >> 16) & 0xFF,
                                     (packedTint >>  8) & 0xFF,
                                     (packedTint      ) & 0xFF) / 255.0;
    s.baseColor *= objectTint;
    s.emissive  += UnoUnpackEntityEmissive(packedEmissive);

    // ===== 法線: グラフの Normal（接空間）→ ワールド（PerturbNormal と同じ z >= 0.35 ガード）=====
    const float3 N = s.hasNormal
        ? UnoNormalFromTangentSpace(input.worldNormal, input.worldTangent, input.tangentW, s.normalTS)
        : normalize(input.worldNormal);

    // ===== シェーディング尾部（デカール → 影 → 直接光 → 環境光 → 自己発光 → デバッグ）=====
    return UnoShadeForward(s, N,
        UnoMakeShadeInput(input.worldPos, input.worldNormal, input.positionSV, input.viewDepth));
}

// ---- ノード内サムネイル / ノード単位プレビュー用の PS（G2c）--------------------------------------------
//   部分グラフ出力（CompileOptions::previewNode）の UnoMatEval は s.baseColor / s.opacity に「表示色」を書く。
//   このエントリはそれを RT へ書くだけ（ライティングなし）。VS は上の VSMain（全画面の四角 + 恒等の MVP で使う）。
//   UNO_NODE_PREVIEW_RAW: 値をそのまま（RGBA32F の RT へ。数値テスト用）。既定は LDR 表示（RGBA8 の RT へガンマ 2.2）。
float4 PSNodePreview(PSInput input) : SV_TARGET
{
    UnoMatInput mi;
    mi.uv              = input.texCoord;
    mi.worldPos        = input.worldPos;
    mi.vertexNormalWS  = normalize(input.worldNormal);
    mi.vertexTangentWS = input.worldTangent;
    mi.tangentW        = input.tangentW;
    mi.vertexColor     = input.color;
    mi.svPos           = input.positionSV;
    mi.cameraPos       = cameraPos;
    mi.time            = time;

    UnoMatPool pool;
    pool.poolSrv    = poolSrvIndex;
    pool.recordBase = recordBase;

    UnoSurface s;
    UnoMatEval(mi, pool, s);
#ifdef UNO_NODE_PREVIEW_RAW
    return float4(s.baseColor, s.opacity);
#else
    return float4(pow(saturate(s.baseColor), 1.0 / 2.2), 1.0);
#endif
}
