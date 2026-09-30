// UnoSurface.hlsli - マテリアルグラフ（生成 HLSL）と共通シェーディング尾部の間の「契約」構造体。
//
//   UnoMatEval（グラフが生成する関数）は UnoMatInput を受けて UnoSurface を返す。
//   ForwardShade.hlsli の UnoShadeForward が UnoSurface を受けてライティングする。
//
// ★フィールド名・型・順序は src/renderer/matgraph/hlsl/UnoMatContractRef.hlsli（G1 の参照実装。
//   DXC + WARP で数値検証済み）と 1 文字も違えてはいけない。tests/forward_shade_contract_test.cpp が
//   両者の構造体本体を機械的に突き合わせている。契約の全文は docs/MATGRAPH_FORMAT.md。
// ★ここには cbuffer もリソース宣言も置かない（b0 衝突の罠の回避。UnoCustom.hlsli の注意書きと同じ）。
//   パラメータプール / サンプラ / UnoSample は G2b の ForwardGraph.hlsl が足す。

#ifndef UNO_SURFACE_HLSLI
#define UNO_SURFACE_HLSLI

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

// ---- 法線（グラフの NormalMap ノード出力 → ワールド法線）--------------------------------------
// グラフ材質の接空間法線（UnoSurface::normalTS）→ ワールド法線。純関数（リソース不要）。
//   PBR.hlsli の PerturbNormal と同じガード（z >= 0.35 で打ち止め。2026-08-02 の黒斑点対策）を通す。
//   旧経路の PerturbNormal は法線マップのサンプル（0..1 の RG）を直接受けるので、そちらは無改造のまま。
//   ここは「すでに -1..1 の単位ベクトル」を受ける版（グラフの NormalMap ノード出力用）。G2b の ForwardGraph が呼ぶ。
float3 UnoNormalFromTangentSpace(float3 worldNormal, float3 worldTangent, float tangentW, float3 normalTS)
{
    float3 N = normalize(worldNormal);
    float3 T = normalize(worldTangent - dot(worldTangent, N) * N); // Gram-Schmidt orthogonalize
    float3 B = cross(N, T) * tangentW;
    float3x3 TBN = float3x3(T, B, N);
    float3 tn = normalize(float3(normalTS.xy, max(normalTS.z, 0.35)));
    return normalize(mul(tn, TBN));
}

#endif // UNO_SURFACE_HLSLI
