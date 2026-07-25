#pragma once

// スカルプトメッシュ（.smsh）の入出力。地形の TerrainIO と同じ流儀。
//
// 頂点配列はシーン JSON に入れない（数万頂点で JSON が破綻する）。
// `<project>/assets/sculpt/<name>.smsh` にバイナリで置き、コンポーネントはパスだけを持つ。
// 読み込みは必ず vfs 経由（配布時に game.pak へ封入されるため）。書き込みはエディタ専用の
// ディスク直書き（ゲームは書かない）。元の .glb 等には絶対に書き戻さない。

#include <string>
#include "terrain/SculptMesh.h"

namespace dx12e::sculpt
{

// assets 相対パス（例 "sculpt/Rock.smsh"）から読む。
// エディタ = ルーズファイル / ゲーム = game.pak（vfs が吸収する）。
bool LoadSculptMeshAsset(const std::string& relPath, SculptMeshData& out);

// 絶対パスへ書き出す（エディタ専用）。中間ディレクトリは自動作成する。
bool SaveSculptMeshFile(const std::string& absPath, const SculptMeshData& mesh);

// エンティティ名から assets 相対のスカルプトメッシュパスを作る（"sculpt/<safeName>.smsh"）。
// パス区切り・危険な文字は '_' に潰す。
std::string MakeSculptMeshRelPath(const std::string& entityName);

} // namespace dx12e::sculpt
