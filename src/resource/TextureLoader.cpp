#include "resource/TextureLoader.h"

#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/Texture.h"
#include "graphics/GraphicsDevice.h"

#include <DirectXTex.h>

#include <vector>
#include <algorithm>   // ConvertToPng の縮小サイズ計算
#include <cstdio>      // Probe のエラーメッセージ整形
#include <cwctype>     // 拡張子の小文字化

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

// 拡張子で適切な DirectXTex ローダを選ぶ(Probe / ConvertToPng 共用)。
HRESULT LoadScratchByExt(const std::wstring& filePath, DirectX::ScratchImage& scratch)
{
    const size_t dot = filePath.find_last_of(L'.');
    std::wstring ext = (dot != std::wstring::npos) ? filePath.substr(dot) : L"";
    for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));
    if (ext == L".dds")
        return DirectX::LoadFromDDSFile(filePath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, scratch);
    if (ext == L".tga")
        return DirectX::LoadFromTGAFile(filePath.c_str(), nullptr, scratch);
    if (ext == L".hdr")
        return DirectX::LoadFromHDRFile(filePath.c_str(), nullptr, scratch);
    return DirectX::LoadFromWICFile(filePath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, scratch);
}

// 同上のメタデータ専用版(画素を読まないので速い)。
HRESULT LoadMetadataByExt(const std::wstring& filePath, DirectX::TexMetadata& meta)
{
    const size_t dot = filePath.find_last_of(L'.');
    std::wstring ext = (dot != std::wstring::npos) ? filePath.substr(dot) : L"";
    for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));
    if (ext == L".dds")
        return DirectX::GetMetadataFromDDSFile(filePath.c_str(), DirectX::DDS_FLAGS_NONE, meta);
    if (ext == L".tga")
        return DirectX::GetMetadataFromTGAFile(filePath.c_str(), meta);
    if (ext == L".hdr")
        return DirectX::GetMetadataFromHDRFile(filePath.c_str(), meta);
    return DirectX::GetMetadataFromWICFile(filePath.c_str(), DirectX::WIC_FLAGS_NONE, meta);
}

// よく使う DXGI_FORMAT だけ名前を返す(それ以外は数値)。asset_info の表示用。
std::string DxgiFormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:      return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:      return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8X8_UNORM:      return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:  return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:  return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R32G32B32_FLOAT:     return "R32G32B32_FLOAT";
    case DXGI_FORMAT_R8_UNORM:            return "R8_UNORM";
    case DXGI_FORMAT_R8G8_UNORM:          return "R8G8_UNORM";
    case DXGI_FORMAT_BC1_UNORM:           return "BC1_UNORM";
    case DXGI_FORMAT_BC1_UNORM_SRGB:      return "BC1_UNORM_SRGB";
    case DXGI_FORMAT_BC3_UNORM:           return "BC3_UNORM";
    case DXGI_FORMAT_BC3_UNORM_SRGB:      return "BC3_UNORM_SRGB";
    case DXGI_FORMAT_BC5_UNORM:           return "BC5_UNORM";
    case DXGI_FORMAT_BC6H_UF16:           return "BC6H_UF16";
    case DXGI_FORMAT_BC7_UNORM:           return "BC7_UNORM";
    case DXGI_FORMAT_BC7_UNORM_SRGB:      return "BC7_UNORM_SRGB";
    default: return "DXGI_FORMAT(" + std::to_string(static_cast<int>(f)) + ")";
    }
}

} // namespace

TextureProbeInfo TextureLoader::Probe(const std::wstring& filePath)
{
    TextureProbeInfo info;
    DirectX::TexMetadata meta{};
    const HRESULT hr = LoadMetadataByExt(filePath, meta);
    if (FAILED(hr))
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "metadata load failed (hr=0x%08X)", static_cast<unsigned>(hr));
        info.error = buf;
        return info;
    }
    info.width     = static_cast<uint32_t>(meta.width);
    info.height    = static_cast<uint32_t>(meta.height);
    info.mipLevels = static_cast<uint32_t>(meta.mipLevels);
    info.arraySize = static_cast<uint32_t>(meta.arraySize);
    info.format    = DxgiFormatName(meta.format);
    info.isCubemap = meta.IsCubemap();
    info.ok = true;
    return info;
}

bool TextureLoader::ConvertToPng(const std::wstring& srcPath, const std::wstring& dstPngPath,
                                 uint32_t maxSize, std::string& outError,
                                 uint32_t& outWidth, uint32_t& outHeight)
{
    using namespace DirectX;
    ScratchImage scratch;
    HRESULT hr = LoadScratchByExt(srcPath, scratch);
    if (FAILED(hr)) { outError = "load failed"; return false; }

    // BC 圧縮はまずデコード
    if (IsCompressed(scratch.GetMetadata().format))
    {
        ScratchImage decompressed;
        hr = Decompress(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(),
                        DXGI_FORMAT_R8G8B8A8_UNORM, decompressed);
        if (FAILED(hr)) { outError = "decompress failed"; return false; }
        scratch = std::move(decompressed);
    }
    // PNG 保存できる 8bit RGBA へ(HDR は 0..1 クランプ)
    if (scratch.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        ScratchImage converted;
        hr = Convert(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(),
                     DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) { outError = "format convert failed"; return false; }
        scratch = std::move(converted);
    }
    // 長辺 maxSize 超は縮小(AI に見せる用途なのでフル解像度は不要)
    {
        const TexMetadata& meta = scratch.GetMetadata();
        const size_t longSide = (std::max)(meta.width, meta.height);
        if (maxSize > 0 && longSide > maxSize)
        {
            const double scale = static_cast<double>(maxSize) / static_cast<double>(longSide);
            size_t nw = static_cast<size_t>(static_cast<double>(meta.width) * scale);
            size_t nh = static_cast<size_t>(static_cast<double>(meta.height) * scale);
            if (nw < 1) nw = 1;
            if (nh < 1) nh = 1;
            ScratchImage resized;
            hr = Resize(scratch.GetImages(), scratch.GetImageCount(), meta,
                        nw, nh, TEX_FILTER_DEFAULT, resized);
            if (FAILED(hr)) { outError = "resize failed"; return false; }
            scratch = std::move(resized);
        }
    }
    const Image* img = scratch.GetImage(0, 0, 0);   // キューブ/配列は先頭面のみ
    if (!img) { outError = "no image"; return false; }
    hr = SaveToWICFile(*img, WIC_FLAGS_NONE, GetWICCodec(WIC_CODEC_PNG), dstPngPath.c_str());
    if (FAILED(hr)) { outError = "png save failed"; return false; }
    outWidth  = static_cast<uint32_t>(img->width);
    outHeight = static_cast<uint32_t>(img->height);
    return true;
}

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
        Logger::Error("テクスチャの読み込みに失敗しました: {}", pathStr);
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
        Logger::Error("ScratchImage から画像データを取得できません");
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
        Logger::Error("LoadCubeFromFile: キューブマップは .dds のみ対応です: {}", toUtf8(filePath));
        return nullptr;
    }

    DirectX::ScratchImage scratchImage;
    HRESULT hr = DirectX::LoadFromDDSFile(filePath.c_str(),
        DirectX::DDS_FLAGS_NONE, nullptr, scratchImage);
    if (FAILED(hr))
    {
        Logger::Error("キューブマップ DDS の読み込みに失敗しました: {}", toUtf8(filePath));
        return nullptr;
    }

    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    if (!meta.IsCubemap() || meta.arraySize != 6)
    {
        Logger::Error("LoadCubeFromFile: 6面キューブマップではありません: {}", toUtf8(filePath));
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
                Logger::Error("LoadCubeFromFile: 画像がありません（face={}, mip={}）: {}",
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
        Logger::Error("埋め込みテクスチャの読み込みに失敗しました（format={}）", hint);
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
        Logger::Error("LoadCubeFromMemory: DDS バッファのデコードに失敗しました");
        return nullptr;
    }

    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    if (!meta.IsCubemap() || meta.arraySize != 6)
    {
        Logger::Error("LoadCubeFromMemory: 6面キューブマップではありません（arraySize={}）",
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
                Logger::Error("LoadCubeFromMemory: 画像がありません（face={}, mip={}）",
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
