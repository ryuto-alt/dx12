#include "resource/TextureLoader.h"

#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/Texture.h"
#include "graphics/GraphicsDevice.h"

#include <DirectXTex.h>

namespace dx12e
{

std::unique_ptr<Texture> TextureLoader::LoadFromFile(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const std::wstring& filePath)
{
    DirectX::ScratchImage scratchImage;

    // 拡張子で読み込み方法を判定
    const std::wstring ext = filePath.substr(filePath.find_last_of(L'.'));

    HRESULT hr = S_OK;
    if (ext == L".dds" || ext == L".DDS")
    {
        hr = DirectX::LoadFromDDSFile(
            filePath.c_str(),
            DirectX::DDS_FLAGS_NONE,
            nullptr,
            scratchImage);
    }
    else
    {
        hr = DirectX::LoadFromWICFile(
            filePath.c_str(),
            DirectX::WIC_FLAGS_NONE,
            nullptr,
            scratchImage);
    }

    if (FAILED(hr))
    {
        // wstring → string for logger
        int sz = WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string pathStr(static_cast<size_t>(sz - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, pathStr.data(), sz, nullptr, nullptr);
        Logger::Error("Failed to load texture: {}", pathStr);
        return nullptr;
    }

    // メタデータ取得・SRGB変換
    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    DXGI_FORMAT format = DirectX::MakeSRGB(meta.format);

    // D3D12_RESOURCE_DESC 構築
    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment          = 0;
    resourceDesc.Width              = static_cast<UINT64>(meta.width);
    resourceDesc.Height             = static_cast<UINT>(meta.height);
    resourceDesc.DepthOrArraySize   = static_cast<UINT16>(meta.arraySize);
    resourceDesc.MipLevels          = static_cast<UINT16>(meta.mipLevels);
    resourceDesc.Format             = format;
    resourceDesc.SampleDesc.Count   = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    // D3D12_SUBRESOURCE_DATA 構築 (mip 0 のみ)
    const DirectX::Image* image = scratchImage.GetImage(0, 0, 0);
    DX_ASSERT(image != nullptr, "Failed to get image data from ScratchImage");

    D3D12_SUBRESOURCE_DATA subresourceData = {};
    subresourceData.pData      = image->pixels;
    subresourceData.RowPitch   = static_cast<LONG_PTR>(image->rowPitch);
    subresourceData.SlicePitch = static_cast<LONG_PTR>(image->slicePitch);

    // Texture 作成・GPU アップロード
    auto texture = std::make_unique<Texture>();
    texture->Initialize(device, cmdList, resourceDesc, &subresourceData, 1);

    Logger::Info("Texture loaded: {}x{}, format={}",
                 static_cast<u32>(meta.width),
                 static_cast<u32>(meta.height),
                 static_cast<u32>(format));

    return texture;
}

std::unique_ptr<Texture> TextureLoader::LoadFromMemory(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const uint8_t* data, size_t dataSize,
    const char* formatHint)
{
    DirectX::ScratchImage scratchImage;

    HRESULT hr = S_OK;
    std::string hint = formatHint ? formatHint : "";

    if (hint == "dds")
    {
        hr = DirectX::LoadFromDDSMemory(data, dataSize,
            DirectX::DDS_FLAGS_NONE, nullptr, scratchImage);
    }
    else
    {
        // jpg, png 等は WIC で読める
        hr = DirectX::LoadFromWICMemory(data, dataSize,
            DirectX::WIC_FLAGS_NONE, nullptr, scratchImage);
    }

    if (FAILED(hr))
    {
        Logger::Error("Failed to load embedded texture (format={})", hint);
        return nullptr;
    }

    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    DXGI_FORMAT format = DirectX::MakeSRGB(meta.format);

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width              = static_cast<UINT64>(meta.width);
    resourceDesc.Height             = static_cast<UINT>(meta.height);
    resourceDesc.DepthOrArraySize   = static_cast<UINT16>(meta.arraySize);
    resourceDesc.MipLevels          = static_cast<UINT16>(meta.mipLevels);
    resourceDesc.Format             = format;
    resourceDesc.SampleDesc.Count   = 1;
    resourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    const DirectX::Image* image = scratchImage.GetImage(0, 0, 0);
    if (!image) return nullptr;

    D3D12_SUBRESOURCE_DATA subresourceData = {};
    subresourceData.pData      = image->pixels;
    subresourceData.RowPitch   = static_cast<LONG_PTR>(image->rowPitch);
    subresourceData.SlicePitch = static_cast<LONG_PTR>(image->slicePitch);

    auto texture = std::make_unique<Texture>();
    texture->Initialize(device, cmdList, resourceDesc, &subresourceData, 1);

    Logger::Info("Embedded texture loaded: {}x{} (format={})",
                 static_cast<u32>(meta.width), static_cast<u32>(meta.height), hint);

    return texture;
}

} // namespace dx12e
