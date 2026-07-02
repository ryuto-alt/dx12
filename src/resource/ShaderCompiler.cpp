#include "resource/ShaderCompiler.h"
#include "core/Logger.h"
#include "core/vfs/Vfs.h"

#include <fstream>
#include <stdexcept>

namespace dx12e
{

ShaderCompiler::ShaderBytecode ShaderCompiler::LoadFromFile(const std::wstring& csoPath)
{
    // ゲームモード(pak マウント済み)では VFS 経由で復号して読む。
    // エディタ/dev では VFS がディスクを生読みする（下の ifstream は欠落時の最終手段）。
    {
        auto bytes = vfs::ReadAssetAbs(csoPath);
        if (!bytes.empty())
        {
            ShaderBytecode bc;
            bc.data = std::move(bytes);
            return bc;
        }
    }

    std::ifstream file(csoPath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        Logger::Error("ShaderCompiler: .cso ファイルを開けません");
        throw std::runtime_error("ShaderCompiler: .cso ファイルを開けません");
    }

    auto fileSize = file.tellg();
    if (fileSize <= 0)
    {
        Logger::Error("ShaderCompiler: .cso ファイルが空か読み取れません");
        throw std::runtime_error("ShaderCompiler: .cso ファイルが空か読み取れません");
    }
    
    ShaderBytecode bytecode;
    bytecode.data.resize(static_cast<size_t>(fileSize));

    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytecode.data.data()), fileSize);

    Logger::Info("ShaderCompiler: Loaded .cso (%zu bytes)", static_cast<size_t>(fileSize));
    return bytecode;
}

} // namespace dx12e
