#include "core/vfs/PakArchive.h"
#include "core/vfs/Compression.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace dx12e::vfs
{

namespace
{
std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                           static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), needed);
    return w;
}
} // namespace

PakArchive::~PakArchive()
{
    closeFile();
    SecureZeroMemory(m_key.data(), m_key.size());
}

void PakArchive::closeFile()
{
    if (m_handle != nullptr)
    {
        CloseHandle(reinterpret_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
}

bool PakArchive::readAt(uint64_t offset, void* buf, std::size_t size) const
{
    // スレッドセーフ: SetFilePointerEx + ReadFile はファイルポインタを共有するため、並列の Read が
    // 互いの位置を壊す（AUD-05）。OVERLAPPED の Offset/OffsetHigh で位置を ReadFile に渡す 1 回の
    // 呼び出しにし、共有状態に触れない。ハンドルは同期のまま（FILE_FLAG_OVERLAPPED は付けない）。
    if (m_handle == nullptr)
        return false;
    HANDLE f = reinterpret_cast<HANDLE>(m_handle);
    auto* dst = static_cast<uint8_t*>(buf);
    // DWORD に収まらない巨大な読み取りは 1 GiB 単位に分ける。
    constexpr std::size_t kChunk = 1u << 30;
    while (size != 0)
    {
        const DWORD want = static_cast<DWORD>(size < kChunk ? size : kChunk);
        OVERLAPPED ov{};
        ov.Offset     = static_cast<DWORD>(offset & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(f, dst, want, &got, &ov))
            return false;
        if (got != want) // ファイル末尾をまたぐ読み取りは失敗（部分読みを成功にしない）
            return false;
        dst    += want;
        offset += want;
        size   -= want;
    }
    return true;
}

bool PakArchive::Mount(const std::string& path)
{
    // 再マウント時は古いハンドルを閉じる（idempotent）。
    closeFile();
    m_toc.clear();
    m_lookup.clear();
    m_mounted = false;

    const std::wstring wpath = Utf8ToWide(path);
    HANDLE f = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    m_handle = f;

    PakHeader h{};
    if (!readAt(0, &h, sizeof(h)))
    {
        closeFile();
        return false;
    }
    if (h.magic != kPakMagic || h.version != kPakVersion || h.endianSentinel != kEndianSentinel)
    {
        closeFile();
        return false;
    }

    // フッタ（書き込み完了センチネル）= 末尾 4 バイト。
    LARGE_INTEGER fsize;
    if (!GetFileSizeEx(f, &fsize) || fsize.QuadPart < static_cast<LONGLONG>(sizeof(PakHeader) + 4))
    {
        closeFile();
        return false;
    }
    uint32_t footer = 0;
    if (!readAt(static_cast<uint64_t>(fsize.QuadPart) - 4, &footer, sizeof(footer)) ||
        footer != kPakMagic)
    {
        closeFile();
        return false;
    }

    // TOC
    m_toc.resize(h.entryCount);
    if (h.entryCount != 0)
    {
        if (!readAt(h.tocOffset, m_toc.data(),
                    static_cast<std::size_t>(h.entryCount) * sizeof(PakEntry)))
        {
            closeFile();
            m_toc.clear();
            return false;
        }
    }

    m_lookup.reserve(h.entryCount);
    for (uint32_t i = 0; i < h.entryCount; ++i)
        m_lookup[m_toc[i].pathHash] = i;

    AssembleKey(m_key);
    m_mounted = true;
    return true;
}

bool PakArchive::Has(const std::string& relPath) const
{
    if (!m_mounted)
        return false;
    return m_lookup.find(FnvHash(Normalize(relPath))) != m_lookup.end();
}

bool PakArchive::Read(const std::string& relPath, std::vector<uint8_t>& out) const
{
    out.clear();
    if (!m_mounted)
        return false;

    const uint64_t hash = FnvHash(Normalize(relPath));
    const auto it = m_lookup.find(hash);
    if (it == m_lookup.end())
        return false;
    const PakEntry& e = m_toc[it->second];

    std::vector<uint8_t> stored(e.storedSize);
    if (e.storedSize != 0)
    {
        if (!readAt(e.dataOffset, stored.data(), e.storedSize))
            return false;
    }

    // decrypt（compress の逆順 = 先に decrypt）
    std::vector<uint8_t> plain;
    if (e.flags & kEntryEncrypted)
    {
        if (!AesGcmDecrypt(m_key.data(), stored.data(), stored.size(), e.nonce, e.tag, plain))
            return false; // tag mismatch（改竄）含め失敗
    }
    else
    {
        plain.swap(stored);
    }

    // decompress
    if (e.flags & kEntryCompressed)
    {
        std::vector<uint8_t> dec;
        if (!XpressDecompress(plain.data(), plain.size(), e.originalSize, dec))
            return false;
        out.swap(dec);
    }
    else
    {
        out.swap(plain);
    }
    return true;
}

} // namespace dx12e::vfs
