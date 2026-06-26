#include "core/vfs/PakWriter.h"
#include "core/vfs/Compression.h"
#include "core/Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <filesystem>

namespace dx12e::vfs
{

PakWriter::~PakWriter()
{
    SecureZeroMemory(m_key.data(), m_key.size());
    if (m_out.is_open())
        m_out.close();
}

bool PakWriter::Open(const std::string& outPath)
{
    m_finalPath = outPath;
    m_tmpPath   = outPath + ".tmp";
    m_entries.clear();
    m_strtab.clear();
    m_seenHashes.clear();
    m_anyXpress = false;

    m_out.open(m_tmpPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!m_out.is_open())
    {
        Logger::Error("PakWriter::Open failed to create {}", m_tmpPath);
        return false;
    }

    // 32 バイトのプレースホルダヘッダ（Finish でパッチ）。
    const char zeros[sizeof(PakHeader)] = {};
    m_out.write(zeros, sizeof(zeros));
    m_dataOffset = sizeof(PakHeader);

    AssembleKey(m_key);
    m_open = true;
    return static_cast<bool>(m_out);
}

bool PakWriter::AddFile(const std::string& srcAbs, const std::string& relPath)
{
    std::ifstream in(srcAbs, std::ios::binary | std::ios::ate);
    if (!in.is_open())
    {
        Logger::Error("PakWriter::AddFile cannot open {}", srcAbs);
        return false;
    }
    const std::streamoff size = in.tellg();
    if (size < 0)
        return false;
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), size);
    in.close();

    return addEntry(relPath, bytes.data(), bytes.size());
}

bool PakWriter::AddBlob(const std::string& relPath, const uint8_t* data, std::size_t len)
{
    return addEntry(relPath, data, len);
}

bool PakWriter::addEntry(const std::string& relPath, const uint8_t* data, std::size_t len)
{
    if (!m_open)
        return false;

    if (len == 0)
    {
        Logger::Warn("PakWriter: skipping zero-byte entry '{}'", relPath);
        return true; // skip（成功扱い）
    }

    const std::string norm = Normalize(relPath);
    const uint64_t    hash = FnvHash(norm);
    if (!m_seenHashes.insert(hash).second)
    {
        Logger::Error("PakWriter: hash collision / duplicate entry '{}' (skipped)", norm);
        return false;
    }

    // 1) compress（縮んだ時のみ）
    std::vector<uint8_t> compressed;
    const bool   didCompress = XpressCompress(data, len, compressed);
    const uint8_t* payload   = didCompress ? compressed.data() : data;
    const std::size_t payLen = didCompress ? compressed.size() : len;

    // 2) encrypt（compress の後）
    uint8_t nonce[kNonceLen] = {};
    uint8_t tag[kTagLen]     = {};
    std::vector<uint8_t> cipher;
    if (!AesGcmEncrypt(m_key.data(), payload, payLen, nonce, tag, cipher))
    {
        Logger::Error("PakWriter: encrypt failed for '{}'", norm);
        return false;
    }

    // 3) 16 バイト境界へパディング
    while (m_dataOffset % 16 != 0)
    {
        const char z = 0;
        m_out.write(&z, 1);
        ++m_dataOffset;
    }

    // 4) TOC エントリ
    PakEntry e{};
    e.pathHash     = hash;
    e.dataOffset   = m_dataOffset;
    e.storedSize   = static_cast<uint32_t>(cipher.size());
    e.originalSize = static_cast<uint32_t>(len);
    e.flags        = static_cast<uint16_t>((didCompress ? kEntryCompressed : 0) | kEntryEncrypted);
    e._pad0        = 0;
    std::memcpy(e.nonce, nonce, kNonceLen);
    std::memcpy(e.tag, tag, kTagLen);
    e.nameOff      = static_cast<uint32_t>(m_strtab.size());
    e._reserved    = 0;

    // 文字列テーブル（dev 用。Finish(stripStrings) で消す）。
    m_strtab.insert(m_strtab.end(), norm.begin(), norm.end());
    m_strtab.push_back('\0');

    // 5) ciphertext 書き込み
    if (!cipher.empty())
        m_out.write(reinterpret_cast<const char*>(cipher.data()),
                    static_cast<std::streamsize>(cipher.size()));
    m_dataOffset += cipher.size();

    if (didCompress)
        m_anyXpress = true;

    m_entries.push_back(e);
    return static_cast<bool>(m_out);
}

bool PakWriter::Finish(bool stripStrings)
{
    if (!m_open)
        return false;

    const uint64_t tocOffset = m_dataOffset;

    // TOC
    for (const PakEntry& src : m_entries)
    {
        PakEntry e = src;
        if (stripStrings)
            e.nameOff = kNoNameOff;
        m_out.write(reinterpret_cast<const char*>(&e), sizeof(PakEntry));
    }

    // 文字列テーブル
    uint32_t strtabSize = 0;
    if (!stripStrings && !m_strtab.empty())
    {
        m_out.write(m_strtab.data(), static_cast<std::streamsize>(m_strtab.size()));
        strtabSize = static_cast<uint32_t>(m_strtab.size());
    }

    // フッタ（書き込み完了センチネル）
    const uint32_t footer = kPakMagic;
    m_out.write(reinterpret_cast<const char*>(&footer), sizeof(footer));

    // ヘッダをパッチ
    PakHeader h{};
    h.magic          = kPakMagic;
    h.version        = kPakVersion;
    h.endianSentinel = kEndianSentinel;
    h.flags          = (m_anyXpress ? kHeaderAnyXpress : 0u) | kHeaderEntriesEncrypted |
                       (stripStrings ? kHeaderStringsStripped : 0u);
    h.tocOffset      = tocOffset;
    h.entryCount     = static_cast<uint32_t>(m_entries.size());
    h.strtabSize     = strtabSize;

    m_out.seekp(0, std::ios::beg);
    m_out.write(reinterpret_cast<const char*>(&h), sizeof(PakHeader));
    m_out.flush();
    const bool streamOk = static_cast<bool>(m_out);
    m_out.close();
    m_open = false;

    SecureZeroMemory(m_key.data(), m_key.size());

    if (!streamOk)
        return false;

    // アトミックに置き換え
    std::error_code ec;
    std::filesystem::remove(m_finalPath, ec);
    ec.clear();
    std::filesystem::rename(m_tmpPath, m_finalPath, ec);
    if (ec)
    {
        Logger::Error("PakWriter::Finish rename failed: {}", ec.message());
        return false;
    }
    return true;
}

} // namespace dx12e::vfs
