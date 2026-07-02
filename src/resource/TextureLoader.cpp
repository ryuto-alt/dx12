#include "resource/TextureLoader.h"

#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/Texture.h"
#include "graphics/GraphicsDevice.h"

#include <DirectXTex.h>

#include <vector>

namespace dx12e
{

namespace
{

// mip が 1 枚しか無い非圧縮画像はフルミップチェーンを生成する（遠景のシマー/
// エイリアシング防止）。BC 圧縮はデコード無しで再生成できないためそのまま。
// 生成失敗は品質低下のみなので mip1 のまま続行する。
void EnsureMipChain(DirectX::ScratchImage& scratch, bool srgb)
{
    const DirectX::TexMetadata& meta = scratch.GetMetadata();
    if (meta.mipLevels > 1) return;
    if (DirectX::IsCompressed(meta.format)) return;
    if (meta.width <= 1 && meta.height <= 1) return;

    // sRGB コンテンツはガンマ考慮のフィルタで縮小しないと mip が暗くなる
    const DirectX::TEX_FILTER_FLAGS filter =
        srgb ? DirectX::TEX_FILTER_SRGB : DirectX::TEX_FILTER_DEFAULT;

    DirectX::ScratchImage mipChain;
    if (SUCCEEDED(DirectX::GenerateMipMaps(
            scratch.GetImages(), scratch.GetImageCount(), meta, filter, 0, mipChain)))
    {
        scratch = std::move(mipChain);
    }
}

// arraySize × mipLevels の全 subresource を組む（D3D12 の順序: item major, mip minor）
std::vector<D3D12_SUBRESOURCE_DATA> BuildSubresources(const DirectX::ScratchImage& scratch)
{
    const DirectX::TexMetadata& meta = scratch.GetMetadata();
    std::vector<D3D12_SUBRESOURCE_DATA> subs;
    subs.reserve(meta.arraySize * meta.mipLevels);
    for (size_t item = 0; item < meta.arraySize; ++item)
    {
        for (size_t mip = 0; mip < meta.mipLevels; ++mip)
        {
            const DirectX::Image* img = scratch.GetImage(mip, item, 0);
            if (!img)
                return {};
            subs.push_back({img->pixels,
                            static_cast<LONG_PTR>(img->rowPitch),
                            static_cast<LONG_PTR>(img->slicePitch)});
        }
    }
    return subs;
}

} // namespace

std::unique_ptr<Texture> TextureLoader::LoadFromFile(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const std::wstring& filePath,
    bool srgb)
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

    // mip が無ければ生成（メタデータは生成後に取り直す）
    EnsureMipChain(scratchImage, srgb);
    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    DXGI_FORMAT format = srgb ? DirectX::MakeSRGB(meta.format) : meta.format;

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

    // 全 subresource（arraySize×mip）をアップロード
    // (旧: mip0 のみ = DDS のミップは VRAM を確保するだけの未初期化ゴミだった)
    auto subs = BuildSubresources(scratchImage);
    if (subs.empty())
    {
        Logger::Error("Failed to get image data from ScratchImage");
        return nullptr;
    }

    auto texture = std::make_unique<Texture>();
    texture->Initialize(device, cmdList, resourceDesc, subs.data(), static_cast<u32>(subs.size()));

    Logger::Info("Texture loaded: {}x{}, mips={}, format={}",
                 static_cast<u32>(meta.width),
                 static_cast<u32>(meta.height),
                 static_cast<u32>(meta.mipLevels),
                 static_cast<u32>(format));

    return texture;
}

std::unique_ptr<Texture> TextureLoader::LoadCubeFromFile(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const std::wstring& filePath,
    bool srgb)
{
    auto toUtf8 = [](const std::wstring& w) -> std::string {
        int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (sz <= 1) return std::string{};
        std::string s(static_cast<size_t>(sz - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), sz, nullptr, nullptr);
        return s;
    };

    const std::wstring ext = filePath.substr(filePath.find_last_of(L'.'));
    if (!(ext == L".dds" || ext == L".DDS"))
    {
        Logger::Error("LoadCubeFromFile: only .dds cubemaps are supported: {}", toUtf8(filePath));
        return nullptr;
    }

    DirectX::ScratchImage scratchImage;
    HRESULT hr = DirectX::LoadFromDDSFile(filePath.c_str(),
        DirectX::DDS_FLAGS_NONE, nullptr, scratchImage);
    if (FAILED(hr))
    {
        Logger::Error("Failed to load cube DDS: {}", toUtf8(filePath));
        return nullptr;
    }

    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    if (!meta.IsCubemap() || meta.arraySize != 6)
    {
        Logger::Error("LoadCubeFromFile: not a 6-face cubemap: {}", toUtf8(filePath));
        return nullptr;
    }

    DXGI_FORMAT format = srgb ? DirectX::MakeSRGB(meta.format) : meta.format;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment          = 0;
    resourceDesc.Width              = static_cast<UINT64>(meta.width);
    resourceDesc.Height             = static_cast<UINT>(meta.height);
    resourceDesc.DepthOrArraySize   = 6;
    resourceDesc.MipLevels          = static_cast<UINT16>(meta.mipLevels);
    resourceDesc.Format             = format;
    resourceDesc.SampleDesc.Count   = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    // 6 面 × 全 mip ぶんの subresource を ScratchImage から手動で組む。
    // D3D12 のサブリソース順序は item(face) major, mip minor:
    //   subresource = face * mipLevels + mip
    const size_t mipLevels = meta.mipLevels;
    std::vector<D3D12_SUBRESOURCE_DATA> subs(6 * mipLevels);
    for (size_t face = 0; face < 6; ++face)
    {
        for (size_t mip = 0; mip < mipLevels; ++mip)
        {
            const DirectX::Image* img = scratchImage.GetImage(mip, face, 0);
            if (!img)
            {
                Logger::Error("LoadCubeFromFile: missing image (face={}, mip={}): {}",
                              static_cast<u32>(face), static_cast<u32>(mip), toUtf8(filePath));
                return nullptr;
            }
            auto& sd      = subs[face * mipLevels + mip];
            sd.pData      = img->pixels;
            sd.RowPitch   = static_cast<LONG_PTR>(img->rowPitch);
            sd.SlicePitch = static_cast<LONG_PTR>(img->slicePitch);
        }
    }

    auto texture = std::make_unique<Texture>();
    texture->Initialize(device, cmdList, resourceDesc, subs.data(), static_cast<u32>(subs.size()));

    Logger::Info("Cube texture loaded: {}x{}, mips={}, format={}",
                 static_cast<u32>(meta.width), static_cast<u32>(meta.height),
                 static_cast<u32>(meta.mipLevels), static_cast<u32>(format));

    return texture;
}

std::unique_ptr<Texture> TextureLoader::LoadFromMemory(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const uint8_t* data, size_t dataSize,
    const char* formatHint,
    bool srgb)
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

    // mip が無ければ生成（メタデータは生成後に取り直す）
    EnsureMipChain(scratchImage, srgb);
    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    DXGI_FORMAT format = srgb ? DirectX::MakeSRGB(meta.format) : meta.format;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width              = static_cast<UINT64>(meta.width);
    resourceDesc.Height             = static_cast<UINT>(meta.height);
    resourceDesc.DepthOrArraySize   = static_cast<UINT16>(meta.arraySize);
    resourceDesc.MipLevels          = static_cast<UINT16>(meta.mipLevels);
    resourceDesc.Format             = format;
    resourceDesc.SampleDesc.Count   = 1;
    resourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    auto subs = BuildSubresources(scratchImage);
    if (subs.empty()) return nullptr;

    auto texture = std::make_unique<Texture>();
    texture->Initialize(device, cmdList, resourceDesc, subs.data(), static_cast<u32>(subs.size()));

    Logger::Info("Embedded texture loaded: {}x{}, mips={} (format={})",
                 static_cast<u32>(meta.width), static_cast<u32>(meta.height),
                 static_cast<u32>(meta.mipLevels), hint);

    return texture;
}

std::unique_ptr<Texture> TextureLoader::LoadCubeFromMemory(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const uint8_t* data, size_t dataSize,
    bool srgb)
{
    DirectX::ScratchImage scratchImage;
    HRESULT hr = DirectX::LoadFromDDSMemory(data, dataSize,
        DirectX::DDS_FLAGS_NONE, nullptr, scratchImage);
    if (FAILED(hr))
    {
        Logger::Error("LoadCubeFromMemory: failed to decode DDS buffer");
        return nullptr;
    }

    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    if (!meta.IsCubemap() || meta.arraySize != 6)
    {
        Logger::Error("LoadCubeFromMemory: not a 6-face cubemap (arraySize={})",
                      static_cast<u32>(meta.arraySize));
        return nullptr;
    }

    DXGI_FORMAT format = srgb ? DirectX::MakeSRGB(meta.format) : meta.format;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment          = 0;
    resourceDesc.Width              = static_cast<UINT64>(meta.width);
    resourceDesc.Height             = static_cast<UINT>(meta.height);
    resourceDesc.DepthOrArraySize   = 6;
    resourceDesc.MipLevels          = static_cast<UINT16>(meta.mipLevels);
    resourceDesc.Format             = format;
    resourceDesc.SampleDesc.Count   = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    // 6 面 × 全 mip ぶんの subresource を ScratchImage から手動で組む。
    // D3D12 のサブリソース順序は item(face) major, mip minor:
    //   subresource = face * mipLevels + mip
    const size_t mipLevels = meta.mipLevels;
    std::vector<D3D12_SUBRESOURCE_DATA> subs(6 * mipLevels);
    for (size_t face = 0; face < 6; ++face)
    {
        for (size_t mip = 0; mip < mipLevels; ++mip)
        {
            const DirectX::Image* img = scratchImage.GetImage(mip, face, 0);
            if (!img)
            {
                Logger::Error("LoadCubeFromMemory: missing image (face={}, mip={})",
                              static_cast<u32>(face), static_cast<u32>(mip));
                return nullptr;
            }
            auto& sd      = subs[face * mipLevels + mip];
            sd.pData      = img->pixels;
            sd.RowPitch   = static_cast<LONG_PTR>(img->rowPitch);
            sd.SlicePitch = static_cast<LONG_PTR>(img->slicePitch);
        }
    }

    auto texture = std::make_unique<Texture>();
    texture->Initialize(device, cmdList, resourceDesc, subs.data(), static_cast<u32>(subs.size()));

    Logger::Info("Cube texture (memory) loaded: {}x{}, mips={}, format={}",
                 static_cast<u32>(meta.width), static_cast<u32>(meta.height),
                 static_cast<u32>(meta.mipLevels), static_cast<u32>(format));

    return texture;
}

} // namespace dx12e
