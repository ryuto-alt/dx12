#include "core/vfs/Compression.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <compressapi.h>

#pragma comment(lib, "cabinet.lib")

namespace dx12e::vfs
{

bool XpressCompress(const uint8_t* src, std::size_t len, std::vector<uint8_t>& outCompressed)
{
    if (src == nullptr || len == 0)
        return false;

    COMPRESSOR_HANDLE h = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &h))
        return false;

    // 必要バッファサイズを問い合わせる（NULL 出力で ERROR_INSUFFICIENT_BUFFER）。
    SIZE_T needed = 0;
    BOOL r = Compress(h, src, static_cast<SIZE_T>(len), nullptr, 0, &needed);
    if (!r && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        CloseCompressor(h);
        return false;
    }

    std::vector<uint8_t> buf(needed != 0 ? needed : len);
    SIZE_T produced = 0;
    r = Compress(h, src, static_cast<SIZE_T>(len), buf.data(), buf.size(), &produced);
    CloseCompressor(h);

    if (!r)
        return false;
    if (produced >= len) // 縮まなかった -> 非圧縮で格納させる
        return false;

    buf.resize(produced);
    outCompressed.swap(buf);
    return true;
}

bool XpressDecompress(const uint8_t* src, std::size_t len, std::size_t expectedOriginalSize,
                      std::vector<uint8_t>& outPlain)
{
    DECOMPRESSOR_HANDLE h = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &h))
        return false;

    outPlain.resize(expectedOriginalSize);
    SIZE_T produced = 0;
    BOOL r = Decompress(h, src, static_cast<SIZE_T>(len),
                        outPlain.empty() ? nullptr : outPlain.data(),
                        outPlain.size(), &produced);
    CloseDecompressor(h);

    if (!r)
    {
        outPlain.clear();
        return false;
    }
    if (produced != static_cast<SIZE_T>(expectedOriginalSize))
    {
        outPlain.clear();
        return false;
    }
    return true;
}

} // namespace dx12e::vfs
