#include "terrain/SculptIO.h"

#include "core/Logger.h"
#include "core/vfs/Vfs.h"

#include <cctype>
#include <filesystem>
#include <fstream>

namespace dx12e::sculpt
{

bool LoadSculptMeshAsset(const std::string& relPath, SculptMeshData& out)
{
    if (relPath.empty()) return false;

    const std::vector<u8> bytes = vfs::ReadAsset(relPath);
    if (bytes.empty())
    {
        Logger::Warn("スカルプトメッシュが見つかりません: {}", relPath);
        return false;
    }
    if (!out.Decode(bytes))
    {
        Logger::Error("スカルプトメッシュの形式が不正です: {}", relPath);
        return false;
    }
    return true;
}

bool SaveSculptMeshFile(const std::string& absPath, const SculptMeshData& mesh)
{
    if (absPath.empty() || !mesh.IsValid()) return false;

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(absPath);
    if (p.has_parent_path())
    {
        fs::create_directories(p.parent_path(), ec);
        if (ec)
        {
            Logger::Error("スカルプトメッシュの保存先を作れません: {}", p.parent_path().string());
            return false;
        }
    }

    const std::vector<u8> bytes = mesh.Encode();
    if (bytes.empty()) return false;

    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        Logger::Error("スカルプトメッシュを書き込めません: {}", absPath);
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!ofs)
    {
        Logger::Error("スカルプトメッシュの書き込みに失敗: {}", absPath);
        return false;
    }
    return true;
}

std::string MakeSculptMeshRelPath(const std::string& entityName)
{
    std::string safe;
    safe.reserve(entityName.size());
    for (char c : entityName)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        const bool ok = (uc >= 0x80)                       // マルチバイト（日本語名）はそのまま通す
                     || std::isalnum(uc) != 0
                     || c == '_' || c == '-';
        safe.push_back(ok ? c : '_');
    }
    if (safe.empty()) safe = "Sculpt";
    return "sculpt/" + safe + ".smsh";
}

} // namespace dx12e::sculpt
