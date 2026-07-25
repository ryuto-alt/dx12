#pragma once

// ハイトマップ（.hf）の入出力。
//
// 高さ配列はシーン JSON に入れない（解像度 256 で 6 万要素になり、JSON では肥大して実用にならない）。
// `<project>/assets/terrain/<name>.hf` にバイナリで置き、コンポーネントはパスだけを持つ。
// 読み込みは必ず vfs 経由（配布時に game.pak へ封入されるため）。書き込みはエディタ専用の
// ディスク直書き（ゲームは書かない）。

#include <string>
#include "terrain/HeightField.h"
#include "terrain/TerrainSplatMap.h"

namespace dx12e::terrain
{

// assets 相対パス（例 "terrain/Terrain.hf"）から読む。
// エディタ = ルーズファイル / ゲーム = game.pak（vfs が吸収する）。
bool LoadHeightFieldAsset(const std::string& relPath, HeightField& out);

// 絶対パスへ書き出す（エディタ専用）。中間ディレクトリは自動作成する。
bool SaveHeightFieldFile(const std::string& absPath, const HeightField& hf);

// エンティティ名から assets 相対のハイトマップパスを作る（"terrain/<safeName>.hf"）。
// パス区切り・危険な文字は '_' に潰す。
std::string MakeHeightFieldRelPath(const std::string& entityName);

// ---- スプラット重みマップ（.splat）----
// .hf とまったく同じ流儀。別ファイルにしてあるのは .hf のフォーマット版を上げないため
// （HeightField::Decode は version != kVersion を即拒否する）。
// ★ファイルが無いのは正常（未ペイント）。呼び出し側は false を「まだ塗っていない」と解釈する。
bool LoadSplatMapAsset(const std::string& relPath, TerrainSplatMap& out);
bool SaveSplatMapFile(const std::string& absPath, const TerrainSplatMap& splat);
std::string MakeSplatRelPath(const std::string& entityName);

} // namespace dx12e::terrain
