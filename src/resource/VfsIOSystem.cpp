#include "resource/VfsIOSystem.h"

#include "core/vfs/Vfs.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace dx12e
{

namespace
{

// std::vector<uint8_t> をラップする読み取り専用 Assimp IOStream。
// セマンティクスは assimp の MemoryIOStream に合わせる（Read は要素数を返す等）。
class VfsIOStream : public Assimp::IOStream
{
public:
    explicit VfsIOStream(std::vector<uint8_t>&& data)
        : m_data(std::move(data)), m_pos(0)
    {
    }

    ~VfsIOStream() override = default;

    // pCount 個（各 pSize バイト）読んで、読めた「要素数」を返す（fread 互換）。
    size_t Read(void* pvBuffer, size_t pSize, size_t pCount) override
    {
        if (pvBuffer == nullptr || pSize == 0)
        {
            return 0;
        }

        const size_t remaining = m_data.size() - m_pos;
        const size_t cnt       = (std::min)(pCount, remaining / pSize);
        const size_t bytes     = pSize * cnt;
        if (bytes > 0)
        {
            std::memcpy(pvBuffer, m_data.data() + m_pos, bytes);
            m_pos += bytes;
        }
        return cnt;
    }

    // 読み取り専用なので Write は no-op。
    size_t Write(const void* /*pvBuffer*/, size_t /*pSize*/, size_t /*pCount*/) override
    {
        return 0;
    }

    aiReturn Seek(size_t pOffset, aiOrigin pOrigin) override
    {
        const size_t length = m_data.size();

        if (pOrigin == aiOrigin_SET)
        {
            if (pOffset > length)
            {
                return aiReturn_FAILURE;
            }
            m_pos = pOffset;
        }
        else if (pOrigin == aiOrigin_END)
        {
            if (pOffset > length)
            {
                return aiReturn_FAILURE;
            }
            m_pos = length - pOffset;
        }
        else // aiOrigin_CUR
        {
            if (pOffset + m_pos > length)
            {
                return aiReturn_FAILURE;
            }
            m_pos += pOffset;
        }
        return aiReturn_SUCCESS;
    }

    size_t Tell() const override
    {
        return m_pos;
    }

    size_t FileSize() const override
    {
        return m_data.size();
    }

    void Flush() override
    {
        // 読み取り専用: 何もしない。
    }

private:
    std::vector<uint8_t> m_data;
    size_t               m_pos;
};

// 絶対 / 相対パスからディスクのルーズファイルを直接読む（フォールバック用）。
std::vector<uint8_t> ReadDiskFile(const char* pFile)
{
    std::error_code ec;
    std::filesystem::path p(pFile);
    if (!std::filesystem::exists(p, ec))
    {
        return {};
    }

    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in.is_open())
    {
        return {};
    }

    const std::streamoff size = in.tellg();
    if (size <= 0)
    {
        return {};
    }
    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!in)
    {
        return {};
    }
    return bytes;
}

} // anonymous namespace

bool VfsIOSystem::Exists(const char* pFile) const
{
    if (pFile == nullptr)
    {
        return false;
    }

    if (vfs::ExistsAbs(std::string(pFile)))
    {
        return true;
    }

    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(pFile), ec);
}

Assimp::IOStream* VfsIOSystem::Open(const char* pFile, const char* /*pMode*/)
{
    if (pFile == nullptr)
    {
        return nullptr;
    }

    // 読み取り専用なので mode は無視（assimp は "rb" 相当しか要求しない）。
    // assimp は .gltf の隣の .bin / テクスチャを gltf のディレクトリ基準（多くは絶対パス）で
    // 解決して渡してくる。ReadAssetAbs が AssetsDir プレフィックスを剥がして相対キーへ正規化する。
    std::vector<uint8_t> bytes = vfs::ReadAssetAbs(std::string(pFile));

    // VFS が空: ディスクのルーズファイルへフォールバック（エディタ disk-mode を不変に保つ）。
    if (bytes.empty())
    {
        bytes = ReadDiskFile(pFile);
        if (bytes.empty())
        {
            return nullptr; // 本当に存在しない。
        }
    }

    return new VfsIOStream(std::move(bytes));
}

void VfsIOSystem::Close(Assimp::IOStream* pFile)
{
    delete pFile;
}

} // namespace dx12e
