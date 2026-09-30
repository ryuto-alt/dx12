#pragma once

// 「グラフ化」= 従来の PBR 材質（.dxmat の 4 枚テクスチャ + 係数）を、等価なマテリアルグラフのインスタンスへ変換する。
//   ・手動だけ（MCP dx12_material_graph_graphize / 将来のエディタのボタン）。エンジンが自動で変換することは絶対にしない。
//   ・元の .dxmat は「<name>.dxmat.bak」へ退避（既にあれば上書きしない = 最初の原本を守る）。
//   ・グラフ本体は共有テンプレート（matgraph/PbrTemplate。構造ごとに 1 個: materials/_graphs/pbr_std_n?m?e?.dxmg）。
//     構造が同じ材質は同じ HLSL = PSO を共有する。値とテクスチャのパスは .dxmat の params に入る。
//   純ロジック（GPU 非依存）。仕様: docs/MATGRAPH_G2B.md

#include <string>

#include "renderer/matgraph/Compiler.h"
#include "resource/MaterialAssetIO.h"

namespace dx12e
{

// .dxmat の params（MaterialAssetData::graphParams）→ グラフのパラメータ上書き（PackParamRecord に渡す形）。
//   Scalar → v[0] / Vector（2〜4）→ v[..]（3 成分のときは w = 1）/ Texture → texture + isTexture。
matgraph::ParamOverrides ToParamOverrides(const MaterialAssetData& data);


struct GraphizeResult
{
    bool        ok = false;
    std::string error;                 // ok=false の理由（日本語）
    std::string templateRel;           // 例 "materials/_graphs/pbr_std_n1m1e2.dxmg"（assets 相対）
    std::string templateText;          // 正準形の .dxmg
    std::string instanceText;          // 新しい .dxmat（version 2 + graph + params）
    MaterialAssetData instance;        // instanceText の中身
};

// legacy（従来の .dxmat を Parse したもの）→ 変換結果。ファイルは触らない。legacy.IsGraph() なら ok=false（既にグラフ材質）。
GraphizeResult GraphizeLegacyMaterial(const MaterialAssetData& legacy, const std::string& graphDirRel = "materials/_graphs/");

// ファイルまで面倒を見る版。assetsDir は末尾 '/' 付きの絶対パス、dxmatRel は assets 相対。
//   dryRun = true なら何も書かず、GraphizeResult だけ返す。
//   書くもの: テンプレート .dxmg（無い / 内容が違うときだけ）・<dxmatRel>.bak（無いときだけ）・<dxmatRel>（変換後）。
struct GraphizeFileResult
{
    GraphizeResult conv;
    bool           wroteTemplate = false;
    bool           wroteBackup = false;
    bool           wroteInstance = false;
};
GraphizeFileResult GraphizeMaterialFile(const std::string& assetsDir, const std::string& dxmatRel, bool dryRun);

} // namespace dx12e
