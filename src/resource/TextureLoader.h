#pragma once

#include <string>
#include <memory>

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Texture;
class GraphicsDevice;

class TextureLoader
{
public:
    static std::unique_ptr<Texture> LoadFromFile(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const std::wstring& filePath);
};

} // namespace dx12e
