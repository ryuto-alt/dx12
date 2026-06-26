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
        const std::wstring& filePath,
        bool srgb = true);  // false = linear (normal/metalRoughness maps)

    // IBL 環境キューブ用：.dds の TEXTURECUBE を 6 面×全 mip でロードする。
    // SRV(TextureCube) は呼び出し側で Texture::CreateCubeSRV を使って張ること。
    static std::unique_ptr<Texture> LoadCubeFromFile(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const std::wstring& filePath,
        bool srgb = false);

    // FBX 埋め込みテクスチャ用：メモリバッファから読み込み
    static std::unique_ptr<Texture> LoadFromMemory(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const uint8_t* data, size_t dataSize,
        const char* formatHint,
        bool srgb = true);

    // VFS / pak 経由キューブマップ用：メモリバッファから .dds キューブマップを読み込む。
    // IsCubemap() && arraySize==6 でなければ nullptr を返す。
    static std::unique_ptr<Texture> LoadCubeFromMemory(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const uint8_t* data, size_t dataSize,
        bool srgb = false);
};

} // namespace dx12e
