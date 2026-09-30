// ============================================================================
// PbrTemplate.h — 従来の PBR 材質（.dxmat の 4 枚テクスチャ + 係数）と等価な「標準 PBR テンプレートグラフ」
//
//   「グラフ化」（MCP / コマンド。手動だけ。自動では絶対に変換しない）の中身。
//   従来の描画（Forward.hlsl の PSMain）と同じ絵になるグラフを、G1 のノードで組む:
//     BaseColor = Albedo(sRGB).rgb * VertexColor.rgb            （エンティティの色ティントは ForwardGraph.hlsl 側で掛かる）
//     Normal    = NormalMap(Normal)                             （従来の PerturbNormal と同じ規約: xy*2-1、z 再構成）
//     Roughness = MetalRoughness.G * Roughness  / Roughness     （テクスチャ無しなら係数だけ）
//     Metallic  = MetalRoughness.B * Metallic   / Metallic
//     Emissive  = Emissive(sRGB).rgb * EmissiveColor * EmissiveIntensity  （テクスチャ無しなら 色 * 強度）
//   構造（どのテクスチャを持つか）が同じ材質は同じグラフ = 同じ HLSL ハッシュ = PSO を共有する。値とテクスチャのパスは
//   .dxmat の params（インスタンス）が持つ。テンプレートは全部で 2 (N) x 2 (M) x 3 (E) = 12 通り。
//
//   ★uvTiling は TexCoord に掛けない（従来の描画は頂点バッファへ焼き込み済みの UV をそのまま使い、
//     描画時に uvTiling を掛けていない。二重に掛けると絵が変わる）。.dxmat の uvTiling は「これから割り当てるとき
//     の初期値」として引き継ぐだけ（MaterialAssetIO の uvTiling キー）。
//   ★AO（MetalRoughness.R）は従来どおり使わない。
//
//   純ロジック（GPU 非依存）。仕様: docs/MATGRAPH_G2B.md
// ============================================================================
#pragma once

#include "renderer/matgraph/GraphModel.h"

#include <string>
#include <vector>

namespace dx12e::matgraph
{

// 従来の PBR 材質の値（MaterialAssetData から必要な分だけ写したもの。matgraph は Resource に依存しない）
struct LegacyPbr
{
    std::string albedo;            // assets 相対。空 = 白
    std::string normal;            // 空 = 法線マップ無し
    std::string metalRoughness;    // 空 = MR テクスチャ無し（係数だけ）
    std::string emissive;          // 空 = 発光テクスチャ無し
    float metallic  = 1.0f;
    float roughness = 1.0f;
    float emissiveColor[3] = {0.0f, 0.0f, 0.0f};
    float emissiveIntensity = 0.0f;
};

// インスタンスの params の 1 エントリ（名前 → 値）。MaterialAssetData::GraphParam へ写す。
struct PbrParam
{
    enum class Kind : uint8_t { Scalar, Vector, Texture };
    std::string name;
    Kind        kind = Kind::Scalar;
    float       v[4] = {0, 0, 0, 0};
    int         n = 1;               // Scalar = 1 / Vector = 3
    std::string texture;
};

// 構造の識別（テンプレートの名前 = ファイル名）。例 "pbr_std_n1m1e2"（n = 法線マップ 0/1、m = MR 0/1、e = 0 無し / 1 色だけ / 2 テクスチャ + 色）
std::string PbrTemplateName(const LegacyPbr& m);
// 発光を持つか（強度が正、または発光テクスチャがある）
bool PbrHasEmissive(const LegacyPbr& m);

// テンプレートグラフを out へ組む（out は空の MaterialGraph。名前・GUID・ノード・接続・既定値まで）。決定論（同じ構造なら同じ .dxmg）。
void BuildPbrTemplateGraph(const LegacyPbr& m, MaterialGraph& out);

// インスタンス（.dxmat の params）。名前は上のテンプレートの Parameter 名と 1 対 1。
std::vector<PbrParam> PbrTemplateParams(const LegacyPbr& m);

} // namespace dx12e::matgraph
