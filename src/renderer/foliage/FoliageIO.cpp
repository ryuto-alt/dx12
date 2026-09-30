#include "renderer/foliage/FoliageIO.h"

#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "core/Logger.h"
#include "core/vfs/Vfs.h"

namespace dx12e::foliage
{
namespace
{
struct Header
{
    char magic[4];
    u32  version;
    u32  instanceCount;
    u32  chunkCount;
    f32  bmin[3];
    f32  bmax[3];
    u32  crc;
    u32  flags;
    u32  reserved[4];
};
static_assert(sizeof(Header) == 64, "ヘッダは 64B");

const std::array<u32, 256>& CrcTable()
{
    static const std::array<u32, 256> t = []
    {
        std::array<u32, 256> a{};
        for (u32 i = 0; i < 256; ++i)
        {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            a[i] = c;
        }
        return a;
    }();
    return t;
}
void SetErr(std::string* e, const char* m) { if (e) *e = m; }
} // namespace

u32 Crc32(const u8* data, size_t size, u32 seed)
{
    const auto& t = CrcTable();
    u32 c = ~seed;
    for (size_t i = 0; i < size; ++i) c = t[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return ~c;
}

std::vector<u8> EncodeFoliage(const FoliageInstanceSet& set)
{
    const size_t chunkBytes = set.chunks.size() * sizeof(FoliageChunk);
    const size_t instBytes  = set.instances.size() * sizeof(FoliageInstance);
    std::vector<u8> out(sizeof(Header) + chunkBytes + instBytes);
    Header h{};
    std::memcpy(h.magic, "DXFL", 4);
    h.version = kFileVersion;
    h.instanceCount = static_cast<u32>(set.instances.size());
    h.chunkCount = static_cast<u32>(set.chunks.size());
    std::memcpy(h.bmin, set.boundsMin, 12);
    std::memcpy(h.bmax, set.boundsMax, 12);
    if (chunkBytes) std::memcpy(out.data() + sizeof(Header), set.chunks.data(), chunkBytes);
    if (instBytes)  std::memcpy(out.data() + sizeof(Header) + chunkBytes, set.instances.data(), instBytes);
    h.crc = Crc32(out.data() + sizeof(Header), chunkBytes + instBytes);
    std::memcpy(out.data(), &h, sizeof(h));
    return out;
}

bool DecodeFoliage(const u8* data, size_t size, FoliageInstanceSet& out, std::string* err)
{
    if (!data || size < sizeof(Header)) { SetErr(err, "ファイルが小さすぎる"); return false; }
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (std::memcmp(h.magic, "DXFL", 4) != 0) { SetErr(err, "マジックが違う（.dxfoliage ではない）"); return false; }
    if (h.version != kFileVersion) { SetErr(err, "未対応のバージョン"); return false; }
    if (h.instanceCount > kMaxFileInstances) { SetErr(err, "インスタンス数が上限を超えている"); return false; }
    if (h.chunkCount > h.instanceCount) { SetErr(err, "チャンク数が不正"); return false; }
    const size_t chunkBytes = static_cast<size_t>(h.chunkCount) * sizeof(FoliageChunk);
    const size_t instBytes  = static_cast<size_t>(h.instanceCount) * sizeof(FoliageInstance);
    if (size != sizeof(Header) + chunkBytes + instBytes) { SetErr(err, "サイズがヘッダと一致しない（途中で切れている）"); return false; }
    if (Crc32(data + sizeof(Header), chunkBytes + instBytes) != h.crc) { SetErr(err, "CRC が一致しない（壊れている）"); return false; }

    FoliageInstanceSet tmp;
    tmp.chunks.resize(h.chunkCount);
    tmp.instances.resize(h.instanceCount);
    if (chunkBytes) std::memcpy(tmp.chunks.data(), data + sizeof(Header), chunkBytes);
    if (instBytes)  std::memcpy(tmp.instances.data(), data + sizeof(Header) + chunkBytes, instBytes);
    // チャンクの整合（範囲・個数・被覆の連続）
    u32 expect = 0;
    for (const FoliageChunk& c : tmp.chunks)
    {
        if (c.count == 0 || c.count > kChunkMaxInstances) { SetErr(err, "チャンクの個数が不正"); return false; }
        if (c.first != expect) { SetErr(err, "チャンクが連続していない"); return false; }
        expect += c.count;
        for (int i = 0; i < 3; ++i)
            if (!(c.mn[i] <= c.mx[i])) { SetErr(err, "チャンクの境界が不正"); return false; }
    }
    if (expect != h.instanceCount) { SetErr(err, "チャンクがインスタンスを覆っていない"); return false; }
    for (const FoliageInstance& in : tmp.instances)
        if (!std::isfinite(in.px) || !std::isfinite(in.py) || !std::isfinite(in.pz)) { SetErr(err, "位置に NaN / 無限大がある"); return false; }
    std::memcpy(tmp.boundsMin, h.bmin, 12);
    std::memcpy(tmp.boundsMax, h.bmax, 12);
    tmp.version = out.version + 1;
    out = std::move(tmp);
    return true;
}

bool LoadFoliageAsset(const std::string& relPath, FoliageInstanceSet& out, std::string* err)
{
    if (relPath.empty()) { SetErr(err, "パスが空"); return false; }
    const std::vector<u8> bytes = vfs::ReadAsset(relPath);
    if (bytes.empty()) { SetErr(err, "ファイルが見つからない / 空"); return false; }
    return DecodeFoliage(bytes, out, err);
}

bool SaveFoliageFile(const std::string& absPath, const FoliageInstanceSet& set, std::string* err)
{
    if (absPath.empty()) { SetErr(err, "保存先が空"); return false; }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(absPath);
    if (p.has_parent_path())
    {
        fs::create_directories(p.parent_path(), ec);
        if (ec) { SetErr(err, "保存先ディレクトリを作れない"); return false; }
    }
    const std::vector<u8> bytes = EncodeFoliage(set);
    // 一時ファイルへ書いてから置き換える（書き込み途中の電源断で古いファイルまで壊さない）
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) { SetErr(err, "書き込めない"); return false; }
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!ofs) { SetErr(err, "書き込みに失敗"); return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec)
    {
        // 既存ファイルがあると rename が失敗する環境向け
        fs::remove(p, ec);
        std::error_code ec2;
        fs::rename(tmp, p, ec2);
        if (ec2) { SetErr(err, "置き換えに失敗"); return false; }
    }
    return true;
}

std::string MakeFoliageRelPath(const std::string& entityName)
{
    std::string safe;
    for (char c : entityName)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        const bool ok = (uc >= 0x80) || std::isalnum(uc) != 0 || c == '_' || c == '-';
        safe.push_back(ok ? c : '_');
    }
    if (safe.empty()) safe = "Foliage";
    return "foliage/" + safe + ".dxfoliage";
}

} // namespace dx12e::foliage
