#pragma once

#include <string>
#include <vector>
#include "core/Types.h"

namespace dx12e
{

class ShaderCompiler
{
public:
    struct ShaderBytecode
    {
        std::vector<u8> data;
        const void* GetData() const { return data.data(); }
        size_t GetSize() const { return data.size(); }
    };

    static ShaderBytecode LoadFromFile(const std::wstring& csoPath);
};

} // namespace dx12e
