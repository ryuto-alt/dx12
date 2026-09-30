// ============================================================================
// GraphIO.h — .dxmg（マテリアルグラフ）の読み書き
//
//   ・JSON。キー順は固定（version, kind, guid, name, settings, nodes, comments, layout）、
//     ノード / ピン / プロパティは辞書順、1 ノード 1 行、layout は 1 ノード 1 行の "x y"。
//     → 何かを 1 個いじると git 差分が 1 行に収まる。
//   ・往復でバイト一致: Save(Load(Save(g))) == Save(g)。
//   ・未知のノード型 / 未知のプロパティ / 宣言に無いピンもそのまま保持する（新しいエンジンで作ったファイルを
//     古いエンジンで開いて保存しても情報が消えない。診断では E_UNKNOWN_NODE などになる）。
//   ・書式の詳細は docs/MATGRAPH_FORMAT.md。
// ============================================================================
#pragma once

#include "renderer/matgraph/GraphModel.h"

#include <string>

namespace dx12e::matgraph
{

constexpr int kDxmgVersion = 1;

// text を読んで out を置き換える（out の NodeLibrary はそのまま使う）。失敗したら false、error に日本語の理由。
bool LoadDxmg(const std::string& text, MaterialGraph& out, std::string* error = nullptr);

// 正準形の .dxmg テキスト（末尾に改行 1 個）。includeView = false で "view"（エディタのパン・ズーム）を書かない（未保存判定用）
std::string SaveDxmg(const MaterialGraph& g, bool includeView = true);

// ファイル入出力（バイナリ。改行は常に LF）
bool LoadDxmgFile(const std::string& path, MaterialGraph& out, std::string* error = nullptr);
// 内容が同じなら書かない（mtime を汚さない）。書いたら true、変更なし / 失敗なら false（失敗は error に理由）。
bool SaveDxmgFileIfChanged(const std::string& path, const MaterialGraph& g, std::string* error = nullptr);

// 正準 JSON の小道具（テストからも使う）
std::string FormatJsonDouble(double d);

} // namespace dx12e::matgraph
