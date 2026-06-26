#pragma once
//
// ビルド時パッカー（エディタ exe の BuildGame から呼ばれる）。
// compress -> encrypt の順でエントリを書き、TOC + 文字列テーブル + フッタを最後に書く。
// .tmp へ書いてから rename する（中断したビルドが半端な pak を出荷しないため）。
//
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <unordered_set>

#include "core/vfs/PakFormat.h"
#include "core/vfs/Crypto.h"

namespace dx12e::vfs
{

class PakWriter
{
public:
    PakWriter() = default;
    ~PakWriter();
    PakWriter(const PakWriter&)            = delete;
    PakWriter& operator=(const PakWriter&) = delete;

    // outPath を開き、32 バイトのプレースホルダヘッダを書く。キーを組み立てる。
    bool Open(const std::string& outPath);

    // 1 ファイル追加: srcAbs を読み、（必要なら）圧縮、暗号化、16 境界へパディングして追記、
    // Normalize(relPath) をキーに TOC エントリを記録する。0 バイトファイルは警告して skip。
    bool AddFile(const std::string& srcAbs, const std::string& relPath);

    // メモリ上の blob を追加（__manifest__ 等）。
    bool AddBlob(const std::string& relPath, const uint8_t* data, std::size_t len);

    // TOC・文字列テーブル・フッタを書き、ヘッダをパッチし、.tmp -> 本ファイルへ rename。
    bool Finish(bool stripStrings);

private:
    bool addEntry(const std::string& relPath, const uint8_t* data, std::size_t len);

    std::ofstream                m_out;
    std::string                  m_tmpPath;
    std::string                  m_finalPath;
    std::vector<PakEntry>        m_entries;
    std::vector<char>            m_strtab;
    std::unordered_set<uint64_t> m_seenHashes;
    uint64_t                     m_dataOffset = 0;
    std::array<uint8_t, kKeyLen> m_key{};
    bool                         m_open      = false;
    bool                         m_anyXpress = false;
};

} // namespace dx12e::vfs
