#pragma once
//
// ランタイム側のマウント済みアーカイブ（リーダ）。
// Mount でヘッダ + フッタを検証し、TOC と lookup マップを RAM へ常駐させる。
// Read は normalize -> hash -> lookup -> 復号 -> 展開。
//
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>

#include "core/vfs/PakFormat.h"
#include "core/vfs/Crypto.h"

namespace dx12e::vfs
{

class PakArchive
{
public:
    PakArchive() = default;
    ~PakArchive();
    PakArchive(const PakArchive&)            = delete;
    PakArchive& operator=(const PakArchive&) = delete;

    // 開いてヘッダ + フッタを検証し、TOC + lookup を読み込む。失敗で false。
    bool Mount(const std::string& path);

    // relPath を normalize -> hash -> lookup し、復号 + 展開した平文を out へ。
    bool Read(const std::string& relPath, std::vector<uint8_t>& out) const;

    // エントリの存在判定。
    bool Has(const std::string& relPath) const;

private:
    void closeFile();
    bool readAt(uint64_t offset, void* buf, std::size_t size) const;

    void*                                   m_handle = nullptr; // HANDLE（cpp 内で reinterpret_cast）
    std::vector<PakEntry>                    m_toc;
    std::unordered_map<uint64_t, uint32_t>   m_lookup;
    std::array<uint8_t, kKeyLen>             m_key{};
    bool                                     m_mounted = false;
};

} // namespace dx12e::vfs
