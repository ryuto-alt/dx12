#pragma once
// ===========================================================================
// パストレーサーの出力(線形 HDR + LDR プレビュー)を書く、依存ゼロの画像 IO。
//   PFM  … 線形 float RGB(比較ツールの入力。flip-evaluator / OpenCV / Photoshop が読める)
//   EXR  … 線形 float32 RGB・無圧縮・単一パート scanline(flip-evaluator が読める)
//   PNG  … 8bit sRGB の簡易プレビュー(無圧縮 deflate。トーンマップは比較側で行うので簡易)
// すべて行優先・上から下の rgb float(w × h × 3)を受け取る。
// ===========================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace dx12e::pt::io
{

// ---- PFM(3 チャンネル・リトルエンディアン。行は下から上)----
inline bool WritePfm(const std::filesystem::path& path, uint32_t w, uint32_t h, const float* rgb)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    std::fprintf(f, "PF\n%u %u\n-1.0\n", w, h);
    for (int32_t y = static_cast<int32_t>(h) - 1; y >= 0; --y)
        std::fwrite(rgb + static_cast<size_t>(y) * w * 3, sizeof(float), static_cast<size_t>(w) * 3, f);
    std::fclose(f);
    return true;
}

inline bool ReadPfm(const std::filesystem::path& path, uint32_t& w, uint32_t& h, std::vector<float>& rgb)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    char tag[3] = {};
    float scale = 0;
    unsigned int ww = 0, hh = 0;
    if (fscanf_s(f, "%2s %u %u %f", tag, 3u, &ww, &hh, &scale) != 4 || std::strcmp(tag, "PF") != 0) { std::fclose(f); return false; }
    std::fgetc(f);   // 改行 1 個
    w = ww; h = hh;
    rgb.assign(static_cast<size_t>(w) * h * 3, 0.0f);
    for (int32_t y = static_cast<int32_t>(h) - 1; y >= 0; --y)
        if (std::fread(rgb.data() + static_cast<size_t>(y) * w * 3, sizeof(float), static_cast<size_t>(w) * 3, f) != static_cast<size_t>(w) * 3)
        { std::fclose(f); return false; }
    std::fclose(f);
    return true;
}

// ---- EXR(無圧縮 float32、チャンネルは B,G,R の順)----
namespace detail
{
inline void PutU32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
inline void PutU64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
inline void PutStr(std::vector<uint8_t>& v, const char* s) { while (*s) v.push_back(static_cast<uint8_t>(*s++)); v.push_back(0); }
inline void PutF32(std::vector<uint8_t>& v, float f) { uint32_t u; std::memcpy(&u, &f, 4); PutU32(v, u); }
inline void Attr(std::vector<uint8_t>& v, const char* name, const char* type, const std::vector<uint8_t>& payload)
{
    PutStr(v, name); PutStr(v, type); PutU32(v, static_cast<uint32_t>(payload.size()));
    v.insert(v.end(), payload.begin(), payload.end());
}
} // namespace detail

inline bool WriteExr(const std::filesystem::path& path, uint32_t w, uint32_t h, const float* rgb)
{
    using namespace detail;
    std::vector<uint8_t> hdr;
    hdr.push_back(0x76); hdr.push_back(0x2f); hdr.push_back(0x31); hdr.push_back(0x01);
    PutU32(hdr, 2);   // version 2、フラグ無し

    std::vector<uint8_t> ch;
    for (const char* n : {"B", "G", "R"})
    {
        PutStr(ch, n);
        PutU32(ch, 2);           // FLOAT
        ch.push_back(0);         // pLinear
        ch.push_back(0); ch.push_back(0); ch.push_back(0);
        PutU32(ch, 1); PutU32(ch, 1);   // x/y sampling
    }
    ch.push_back(0);
    Attr(hdr, "channels", "chlist", ch);
    Attr(hdr, "compression", "compression", {0});   // NO_COMPRESSION
    std::vector<uint8_t> box;
    PutU32(box, 0); PutU32(box, 0); PutU32(box, w - 1); PutU32(box, h - 1);
    Attr(hdr, "dataWindow", "box2i", box);
    Attr(hdr, "displayWindow", "box2i", box);
    Attr(hdr, "lineOrder", "lineOrder", {0});
    std::vector<uint8_t> f1; PutF32(f1, 1.0f);
    Attr(hdr, "pixelAspectRatio", "float", f1);
    std::vector<uint8_t> v2; PutF32(v2, 0.0f); PutF32(v2, 0.0f);
    Attr(hdr, "screenWindowCenter", "v2f", v2);
    Attr(hdr, "screenWindowWidth", "float", f1);
    hdr.push_back(0);   // ヘッダ終端

    const uint64_t rowBytes = static_cast<uint64_t>(w) * 3 * 4;
    const uint64_t blockBytes = 8 + rowBytes;   // y(4) + size(4) + data
    const uint64_t tableBytes = static_cast<uint64_t>(h) * 8;
    uint64_t offset = hdr.size() + tableBytes;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    std::fwrite(hdr.data(), 1, hdr.size(), f);
    std::vector<uint8_t> table;
    for (uint32_t y = 0; y < h; ++y) { PutU64(table, offset); offset += blockBytes; }
    std::fwrite(table.data(), 1, table.size(), f);
    std::vector<float> line(static_cast<size_t>(w) * 3);
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint32_t yy = y, sz = static_cast<uint32_t>(rowBytes);
        std::fwrite(&yy, 4, 1, f);
        std::fwrite(&sz, 4, 1, f);
        for (uint32_t x = 0; x < w; ++x) line[x] = rgb[(static_cast<size_t>(y) * w + x) * 3 + 2];               // B
        for (uint32_t x = 0; x < w; ++x) line[w + x] = rgb[(static_cast<size_t>(y) * w + x) * 3 + 1];           // G
        for (uint32_t x = 0; x < w; ++x) line[2 * w + x] = rgb[(static_cast<size_t>(y) * w + x) * 3 + 0];       // R
        std::fwrite(line.data(), sizeof(float), line.size(), f);
    }
    std::fclose(f);
    return true;
}

// ---- LDR プレビュー(簡易)----
inline float SrgbEncode(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return (c <= 0.0031308f) ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// exposure(倍率)→ ACES(Narkowicz)→ sRGB。NaN は 0。
inline void TonemapPreview(const float* rgb, uint32_t w, uint32_t h, float exposure, std::vector<uint8_t>& out8)
{
    out8.resize(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < static_cast<size_t>(w) * h * 3; ++i)
    {
        float x = rgb[i] * exposure;
        if (!(x >= 0.0f)) x = 0.0f;
        const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
        const float t = std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
        out8[i] = static_cast<uint8_t>(SrgbEncode(t) * 255.0f + 0.5f);
    }
}

// ---- PNG(8bit RGB・無圧縮 deflate)----
namespace detail
{
inline uint32_t Crc32(const uint8_t* d, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
inline void PutBE32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
inline void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
    PutBE32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    PutBE32(out, Crc32(td.data(), td.size()));
}
} // namespace detail

inline bool WritePngRgb8(const std::filesystem::path& path, uint32_t w, uint32_t h, const uint8_t* rgb8)
{
    using namespace detail;
    // 生データ = 各行の先頭にフィルタ 0
    std::vector<uint8_t> raw;
    raw.reserve((static_cast<size_t>(w) * 3 + 1) * h);
    for (uint32_t y = 0; y < h; ++y)
    {
        raw.push_back(0);
        raw.insert(raw.end(), rgb8 + static_cast<size_t>(y) * w * 3, rgb8 + static_cast<size_t>(y + 1) * w * 3);
    }
    // zlib(stored ブロック)
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size() || raw.empty())
    {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = (pos + n >= raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF)); z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF)); z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    PutBE32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    PutBE32(ihdr, w); PutBE32(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    Chunk(out, "IHDR", ihdr);
    Chunk(out, "IDAT", z);
    Chunk(out, "IEND", {});
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return true;
}

// 便利: 線形 HDR → プレビュー PNG(簡易トーンマップ)
inline bool WritePreviewPng(const std::filesystem::path& path, uint32_t w, uint32_t h, const float* rgb, float exposure = 1.0f)
{
    std::vector<uint8_t> px;
    TonemapPreview(rgb, w, h, exposure, px);
    return WritePngRgb8(path, w, h, px.data());
}

} // namespace dx12e::pt::io
