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
        Logger::Error("PakWriter: {} を作成できません", m_tmpPath);
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

bool PakWriter::OpenAppend(const std::string& pakPath)
{
    // 既存の pak（Finish 済み・文字列テーブル strip 済み）の TOC を読み込み、旧 TOC の位置から
    // 続けてエントリを追記できる状態にする。ビルド時に「pak を作る → ゲームを走らせて使われた
    // テクスチャ圧縮結果を得る → 同じ pak へ足す」ために使う（全アセットを暗号化し直さずに済む）。
    m_finalPath = pakPath;
    m_tmpPath.clear();
    m_entries.clear();
    m_strtab.clear();
    m_seenHashes.clear();
    m_anyXpress = false;
    m_append    = true;

    PakHeader h{};
    {
        std::ifstream in(pakPath, std::ios::binary);
        if (!in) { Logger::Error("PakWriter: {} を開けません", pakPath); return false; }
        in.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (!in || h.magic != kPakMagic || h.version != kPakVersion
            || (h.flags & kHeaderStringsStripped) == 0 || h.strtabSize != 0)
        {
            Logger::Error("PakWriter: 追記できる形式の pak ではありません: {}", pakPath);
            return false;
        }
        m_entries.resize(h.entryCount);
        in.seekg(static_cast<std::streamoff>(h.tocOffset));
        if (h.entryCount > 0)
            in.read(reinterpret_cast<char*>(m_entries.data()),
                    static_cast<std::streamsize>(sizeof(PakEntry) * h.entryCount));
        if (!in) { Logger::Error("PakWriter: TOC を読めません: {}", pakPath); return false; }
    }
    for (const PakEntry& e : m_entries) m_seenHashes.insert(e.pathHash);
    m_anyXpress  = (h.flags & kHeaderAnyXpress) != 0;
    m_dataOffset = h.tocOffset;

    m_out.open(pakPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!m_out.is_open())
    {
        Logger::Error("PakWriter: {} を追記用に開けません", pakPath);
        return false;
    }
    m_out.seekp(static_cast<std::streamoff>(m_dataOffset));
    AssembleKey(m_key);
    m_open = true;
    return static_cast<bool>(m_out);
}

bool PakWriter::AddFile(const std::string& srcAbs, const std::string& relPath)
{
    std::ifstream in(srcAbs, std::ios::binary | std::ios::ate);
    if (!in.is_open())
    {
        Logger::Error("PakWriter: {} を開けません", srcAbs);
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
        Logger::Warn("PakWriter: 0バイトのエントリをスキップしました: '{}'", relPath);
        return true; // skip（成功扱い）
    }

    const std::string norm = Normalize(relPath);
    const uint64_t    hash = FnvHash(norm);
    if (!m_seenHashes.insert(hash).second)
    {
        Logger::Error("PakWriter: ハッシュ衝突または重複エントリのためスキップしました: '{}'", norm);
        return false;
    }

    // 1) compress（縮んだ時のみ）
    std::vector<uint8_t> compressed;
    // ビルド時に焼いた BC 圧縮テクスチャ("texcache/")は圧縮しない。BC7 は XPRESS で 2〜4 割しか縮まない一方、
    // 起動時に数百枚を展開するコストの方が大きい（読み込みの支配項になる）。暗号化だけ掛ける。
    const bool   noCompress  = norm.compare(0, 9, "texcache/") == 0;
    const bool   didCompress = !noCompress && XpressCompress(data, len, compressed);
    const uint8_t* payload   = didCompress ? compressed.data() : data;
    const std::size_t payLen = didCompress ? compressed.size() : len;

    // 2) encrypt（compress の後）
    uint8_t nonce[kNonceLen] = {};
    uint8_t tag[kTagLen]     = {};
    std::vector<uint8_t> cipher;
    if (!AesGcmEncrypt(m_key.data(), payload, payLen, nonce, tag, cipher))
    {
        Logger::Error("PakWriter: '{}' の暗号化に失敗しました", norm);
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
    if (m_append)
        return true;   // 追記は同じファイルの中で完結（TOC の位置が変わるだけ）

    // アトミックに置き換え
    std::error_code ec;
    std::filesystem::remove(m_finalPath, ec);
    ec.clear();
    std::filesystem::rename(m_tmpPath, m_finalPath, ec);
    if (ec)
    {
        Logger::Error("PakWriter: 出力ファイルのリネームに失敗しました: {}", ec.message());
        return false;
    }
    return true;
}

} // namespace dx12e::vfs
