#pragma once
// ===========================================================================
// 植生 F1: .dxfoliage（インスタンス表のバイナリ）の入出力。
// ---------------------------------------------------------------------------
// シーン JSON にはインスタンスを直書きしない（100 万個で 32 MB）。コンポーネントはパスだけを持ち、実体はここ。
//
//   [64 B ヘッダ]  magic "DXFL" / version(1) / instanceCount / chunkCount / boundsMin[3] / boundsMax[3] /
//                  crc32（チャンク表 + インスタンス表）/ flags(0) / reserved[4]
//   [chunkCount × 32 B]  FoliageChunk
//   [instanceCount × 32 B]  FoliageInstance（チャンク順）
//   全部リトルエンディアン・パディング無し。読み込みは memcpy 2 回だけ（BuildChunks を再実行しない）。
// ★破損 / 不正値（範囲外のチャンク・件数の暴走・CRC 不一致・NaN の位置）は false とエラー文字列で断る（描画側へ渡さない）。
// ===========================================================================
#include <string>
#include <vector>

#include "core/Types.h"
#include "renderer/foliage/FoliageTypes.h"

namespace dx12e::foliage
{

constexpr u32 kMaxFileInstances = 64u * 1024u * 1024u;   // 6400 万（2 GB）。これを超えるファイルは不正扱い

std::vector<u8> EncodeFoliage(const FoliageInstanceSet& set);
bool DecodeFoliage(const u8* data, size_t size, FoliageInstanceSet& out, std::string* err = nullptr);
inline bool DecodeFoliage(const std::vector<u8>& b, FoliageInstanceSet& out, std::string* err = nullptr)
{
    return DecodeFoliage(b.data(), b.size(), out, err);
}

// assets 相対パス（vfs 経由。ゲームは game.pak）から読む。ファイルが無い / 不正なら false。
bool LoadFoliageAsset(const std::string& relPath, FoliageInstanceSet& out, std::string* err = nullptr);
// 絶対パスへ書く（エディタ専用。中間ディレクトリは作る）。
bool SaveFoliageFile(const std::string& absPath, const FoliageInstanceSet& set, std::string* err = nullptr);
// エンティティ名から assets 相対パスを作る（"foliage/<safeName>.dxfoliage"）。
std::string MakeFoliageRelPath(const std::string& entityName);

u32 Crc32(const u8* data, size_t size, u32 seed = 0);

} // namespace dx12e::foliage
