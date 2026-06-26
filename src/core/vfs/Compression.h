#pragma once
//
// Windows Compression API（cabinet.dll / cabinet.lib）の XPRESS_HUFF ラッパー。
// バッファモード（COMPRESS_RAW 不使用）。link: cabinet.lib。
//
#include <cstdint>
#include <cstddef>
#include <vector>

namespace dx12e::vfs
{

// src を圧縮。成功時 outCompressed に結果を格納する。
// 結果が src より「厳密に小さい」場合のみ true を返す
// （そうでなければ呼び出し側は非圧縮で格納し compressed フラグをクリアする）。
bool XpressCompress(const uint8_t* src, std::size_t len, std::vector<uint8_t>& outCompressed);

// src（XPRESS_HUFF）を expectedOriginalSize バイトへ展開する（TOC の originalSize）。
bool XpressDecompress(const uint8_t* src, std::size_t len, std::size_t expectedOriginalSize,
                      std::vector<uint8_t>& outPlain);

} // namespace dx12e::vfs
