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
    // ---- 拡張シェーディング（ForwardShade.hlsli が消費する。既定値なら従来と同じ絵）----
    float  clearCoat;           // 0..1（KHR_materials_clearcoat）
    float  clearCoatRoughness;  // 0..1
    float  anisotropy;          // -1..1（KHR_materials_anisotropy の強さ。負は 90° 回した向き）
    uint   shadingModel;        // 予約
    float  anisotropyRotation;  // ラジアン（接空間で +U から反時計回り）
    float3 sheenColor;          // リニア（KHR_materials_sheen。0 = 無し）
    float  sheenRoughness;      // 0..1
    float  specular;            // UE 流 0..1（既定 0.5 → 誘電体の F0 = 0.08·specular = 0.04）
    float3 specularTint;        // 誘電体の F0 に掛かる色（既定 1。glTF の specularColor / ior はここへ畳む）
    float  specularF90;         // 誘電体の F90（既定 1。glTF の specularFactor）
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
    s.anisotropyRotation = 0.0;
    s.sheenColor         = float3(0.0, 0.0, 0.0);
    s.sheenRoughness     = 0.0;
    s.specular           = 0.5;
    s.specularTint       = float3(1.0, 1.0, 1.0);
    s.specularF90        = 1.0;
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
    // ★壊れた接線（0・NaN）では基底が作れない → NaN 法線で直射光が消える。幾何法線へ落とす（PBR.hlsli の PerturbNormal と同じ）
    float3 Tp = worldTangent - dot(worldTangent, N) * N;
    if (!(dot(Tp, Tp) > 1e-12)) return N;
    float3 T = normalize(Tp); // Gram-Schmidt orthogonalize
    float3 B = cross(N, T) * tangentW;
    float3x3 TBN = float3x3(T, B, N);
    float3 tn = normalize(float3(normalTS.xy, max(normalTS.z, 0.35)));
    return normalize(mul(tn, TBN));
}

// ---- 拡張シェーディングの 8 DWORD（src/renderer/ShadingExt.h の PackShadingExt と一対一）→ UnoSurface ----
//   e0 = w0..w3 / e1 = w4..w7。w0 の上位 8bit が 0xA5 でなければ「拡張無し」（何も書かない＝既定値のまま）。
//   フォワードの b0 の自由枠・パストレーサーの材質・RT の GeometryInfo・VG の材質表が同じ並びで持つ。
void UnoApplyShadingExt(inout UnoSurface s, uint4 e0, uint4 e1)
{
    if ((e0.x & 0xFF000000u) != 0xA5000000u) return;
    const uint f = e0.x & 0xFFu;
    if (f & 1u)
    {
        s.clearCoat          = float(e0.y & 0xFFFFu) / 65535.0;
        s.clearCoatRoughness = float(e0.y >> 16) / 65535.0;
    }
    if (f & 2u)
    {
        const int sv = int(e0.z << 16) >> 16;   // snorm16 の符号拡張
        s.anisotropy         = max(float(sv) / 32767.0, -1.0);
        s.anisotropyRotation = float(e0.z >> 16) / 65535.0 * 6.28318530718;
    }
    if (f & 4u)
    {
        s.sheenColor     = float3((e0.w >> 16) & 0xFFu, (e0.w >> 8) & 0xFFu, e0.w & 0xFFu) / 255.0;
        s.sheenRoughness = float(e0.w >> 24) / 255.0;
    }
    if (f & 8u)
    {
        // F0 の色はそのまま specularTint へ（specular = 0.5 → 0.08·0.5 = 0.04 を掛けて戻る）
        s.specular     = 0.5;
        s.specularTint = asfloat(e1.xyz) / 0.04;
        s.specularF90  = asfloat(e1.w);
    }
}

#endif // UNO_SURFACE_HLSLI
