#include "resource/ShaderCompiler.h"
#include "core/Logger.h"

#include <fstream>
#include <stdexcept>

namespace dx12e
{

ShaderCompiler::ShaderBytecode ShaderCompiler::LoadFromFile(const std::wstring& csoPath)
{
    std::ifstream file(csoPath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        Logger::Error("ShaderCompiler: Failed to open .cso file");
        throw std::runtime_error("ShaderCompiler: Failed to open .cso file");
    }

    auto fileSize = file.tellg();
    if (fileSize <= 0)
    {
        Logger::Error("ShaderCompiler: .cso file is empty or unreadable");
        throw std::runtime_error("ShaderCompiler: .cso file is empty or unreadable");
    }

    ShaderBytecode bytecode;
    bytecode.data.resize(static_cast<size_t>(fileSize));

    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytecode.data.data()), fileSize);

    Logger::Info("ShaderCompiler: Loaded .cso (%zu bytes)", static_cast<size_t>(fileSize));
    return bytecode;
}

} // namespace dx12e
