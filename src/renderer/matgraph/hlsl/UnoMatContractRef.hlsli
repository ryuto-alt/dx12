// ============================================================================
// UnoMatContractRef.hlsli — マテリアルグラフが生成する HLSL の「契約」の参照実装
//
//   生成された HLSL（UnoMatEval を定義する 1 ファイル）は、先頭で include する 1 本のヘッダに次を要求する。
//   本ファイルはその最小の実装で、G1 のテスト（DXC コンパイル・WARP での数値比較）が使う。
//   エンジン側（G2a の UnoSurface.hlsli / G2b の ForwardGraph.hlsl）は、同じ名前・同じ意味で提供すること。
//   契約の全文は docs/MATGRAPH_FORMAT.md「生成 HLSL の契約」。
//
//   ★ここに無いものを生成コードは使わない（cbuffer も自前で宣言しない。b0 衝突の罠の回避）。
// ============================================================================
#ifndef UNO_MAT_CONTRACT_REF_HLSLI
#define UNO_MAT_CONTRACT_REF_HLSLI

// ---- サーフェス（UnoMatEval の出力）---------------------------------------------
struct UnoSurface
{
    float3 baseColor;
    float  metallic;
    float  roughness;
    float3 normalTS;            // 接空間の法線（hasNormal のときだけ有効）
    bool   hasNormal;
    float3 emissive;
    float  ao;
    float  opacity;
    float  opacityMask;
    // ---- 予約（現行の共通シェーディングは無視する）----
    float3 subsurfaceColor;
    float  subsurfaceOpacity;
    float  clearCoat;
    float  clearCoatRoughness;
    float  anisotropy;
    uint   shadingModel;
};

UnoSurface UnoSurfaceDefault()
{
    UnoSurface s;
    s.baseColor          = float3(0.5, 0.5, 0.5);
    s.metallic           = 0.0;
    s.roughness          = 0.5;
    s.normalTS           = float3(0.0, 0.0, 1.0);
    s.hasNormal          = false;
    s.emissive           = float3(0.0, 0.0, 0.0);
    s.ao                 = 1.0;
    s.opacity            = 1.0;
    s.opacityMask        = 1.0;
    s.subsurfaceColor    = float3(0.0, 0.0, 0.0);
    s.subsurfaceOpacity  = 0.0;
    s.clearCoat          = 0.0;
    s.clearCoatRoughness = 0.0;
    s.anisotropy         = 0.0;
    s.shadingModel       = 0;
    return s;
}

// ---- ピクセル入力（UnoMatEval の入力）------------------------------------------
struct UnoMatInput
{
    float2 uv;
    float3 worldPos;
    float3 vertexNormalWS;
    float3 vertexTangentWS;
    float  tangentW;
    float4 vertexColor;
    float4 svPos;
    float3 cameraPos;
    float  time;
};

// ---- パラメータプール ------------------------------------------------------------
// レコード = float4 の並び。スロット番号は CompileResult::slots の slot（float4 単位）。
//   スカラー / ベクトル : F1(slot) .. F4(slot)
//   テクスチャ          : Tex(slot)   x = SRV 添字（asuint）、y = 1/幅、z = 1/高さ
// 参照実装は StructuredBuffer を直接読む。エンジン（G2b）は b2 の recordBase / プール SRV 添字から
// ResourceDescriptorHeap 経由で読む（DWORD +0）。メソッドの名前と意味だけを揃えること。
StructuredBuffer<float4> g_unoPool : register(t0);

struct UnoMatPool
{
    uint recordBase;

    float4 Raw(uint slot)  { return g_unoPool[recordBase + slot]; }
    float  F1(uint slot)   { return Raw(slot).x; }
    float2 F2(uint slot)   { return Raw(slot).xy; }
    float3 F3(uint slot)   { return Raw(slot).xyz; }
    float4 F4(uint slot)   { return Raw(slot); }
    Texture2D<float4> Tex(uint slot)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[asuint(Raw(slot).x)];
        return t;
    }
};

// ---- サンプラー（マクロ名は TextureSample の samplerState と 1 対 1）-----------------
SamplerState g_unoSampAnisoWrap  : register(s0);
SamplerState g_unoSampAnisoClamp : register(s1);
SamplerState g_unoSampLinearWrap : register(s2);
SamplerState g_unoSampLinearClamp: register(s3);
SamplerState g_unoSampPointWrap  : register(s4);
SamplerState g_unoSampPointClamp : register(s5);
#define UNO_SAMP_ANISO_WRAP    g_unoSampAnisoWrap
#define UNO_SAMP_ANISO_CLAMP   g_unoSampAnisoClamp
#define UNO_SAMP_LINEAR_WRAP   g_unoSampLinearWrap
#define UNO_SAMP_LINEAR_CLAMP  g_unoSampLinearClamp
#define UNO_SAMP_POINT_WRAP    g_unoSampPointWrap
#define UNO_SAMP_POINT_CLAMP   g_unoSampPointClamp

// ---- サンプル（生成コードは Sample を直接書かず、必ずこのマクロを通す）-----------------
//   フォワード = 暗黙微分の Sample。将来の VG resolve = SampleGrad に差し替える点。
//   UNO_CONTRACT_COMPUTE を定義すると（CS のテスト用）SampleLevel になる。
#ifdef UNO_CONTRACT_COMPUTE
#define UnoSample(tex, samp, uv)            (tex).SampleLevel((samp), (uv), 0.0)
#define UnoSampleBias(tex, samp, uv, bias)  (tex).SampleLevel((samp), (uv), (bias))
#else
#define UnoSample(tex, samp, uv)            (tex).Sample((samp), (uv))
#define UnoSampleBias(tex, samp, uv, bias)  (tex).SampleBias((samp), (uv), (bias))
#endif

#endif // UNO_MAT_CONTRACT_REF_HLSLI
