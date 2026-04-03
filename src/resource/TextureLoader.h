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

    // FBX 埋め込みテクスチャ用：メモリバッファから読み込み
    static std::unique_ptr<Texture> LoadFromMemory(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const uint8_t* data, size_t dataSize,
        const char* formatHint);  // "jpg", "png" 等
};

} // namespace dx12e
