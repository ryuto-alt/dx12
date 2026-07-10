#pragma once

#include <string>
#include <memory>
#include <cstdint>

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Texture;
class GraphicsDevice;

// MCP asset_info 用: GPU を使わずテクスチャのメタ情報だけ読む(DirectXTex のみ)。
struct TextureProbeInfo
{
    bool        ok = false;
    std::string error;
    uint32_t width = 0, height = 0, mipLevels = 0, arraySize = 0;
    std::string format;      // DXGI_FORMAT 名(未知の値は数値)
    bool isCubemap = false;
};

class TextureLoader
{
public:
    static TextureProbeInfo Probe(const std::wstring& filePath);

    // MCP read_texture 用: 対応形式(dds/tga/hdr/WIC 系)を PNG へ変換して保存する。
    // 長辺が maxSize を超える場合はアスペクト比を保って縮小。成功で true。
    static bool ConvertToPng(const std::wstring& srcPath, const std::wstring& dstPngPath,
                             uint32_t maxSize, std::string& outError,
                             uint32_t& outWidth, uint32_t& outHeight);

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
