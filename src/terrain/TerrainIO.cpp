#include "terrain/TerrainIO.h"

#include "core/Logger.h"
#include "core/vfs/Vfs.h"

#include <cctype>
#include <filesystem>
#include <fstream>

namespace dx12e::terrain
{

bool LoadHeightFieldAsset(const std::string& relPath, HeightField& out)
{
    if (relPath.empty()) return false;

    const std::vector<u8> bytes = vfs::ReadAsset(relPath);
    if (bytes.empty())
    {
        Logger::Warn("地形ハイトマップが見つかりません: {}", relPath);
        return false;
    }
    if (!out.Decode(bytes))
    {
        Logger::Error("地形ハイトマップの形式が不正です: {}", relPath);
        return false;
    }
    return true;
}

bool SaveHeightFieldFile(const std::string& absPath, const HeightField& hf)
{
    if (absPath.empty() || !hf.IsValid()) return false;

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(absPath);
    if (p.has_parent_path())
    {
        fs::create_directories(p.parent_path(), ec);
        if (ec)
        {
            Logger::Error("地形ハイトマップの保存先を作れません: {}", p.parent_path().string());
            return false;
        }
    }

    const std::vector<u8> bytes = hf.Encode();
    if (bytes.empty()) return false;

    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        Logger::Error("地形ハイトマップを書き込めません: {}", absPath);
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!ofs)
    {
        Logger::Error("地形ハイトマップの書き込みに失敗: {}", absPath);
        return false;
    }
    return true;
}

// ファイル名に使える形へ潰す（パス区切り・記号は '_'、マルチバイトはそのまま）。
static std::string SafeStem(const std::string& entityName)
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
    if (safe.empty()) safe = "Terrain";
    return safe;
}

std::string MakeHeightFieldRelPath(const std::string& entityName)
{
    return "terrain/" + SafeStem(entityName) + ".hf";
}

std::string MakeSplatRelPath(const std::string& entityName)
{
    return "terrain/" + SafeStem(entityName) + ".splat";
}

bool LoadSplatMapAsset(const std::string& relPath, TerrainSplatMap& out)
{
    if (relPath.empty()) return false;

    const std::vector<u8> bytes = vfs::ReadAsset(relPath);
    if (bytes.empty())
        return false;   // ★未ペイントは正常。警告を出さない（自動ペイントで作られる）
    if (!out.Decode(bytes))
    {
        Logger::Error("地形スプラットマップの形式が不正です: {}", relPath);
        return false;
    }
    return true;
}

bool SaveSplatMapFile(const std::string& absPath, const TerrainSplatMap& splat)
{
    if (absPath.empty() || !splat.IsValid()) return false;

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(absPath);
    if (p.has_parent_path())
    {
        fs::create_directories(p.parent_path(), ec);
        if (ec)
        {
            Logger::Error("地形スプラットマップの保存先を作れません: {}", p.parent_path().string());
            return false;
        }
    }

    const std::vector<u8> bytes = splat.Encode();
    if (bytes.empty()) return false;

    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        Logger::Error("地形スプラットマップを書き込めません: {}", absPath);
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!ofs)
    {
        Logger::Error("地形スプラットマップの書き込みに失敗: {}", absPath);
        return false;
    }
    return true;
}

} // namespace dx12e::terrain
