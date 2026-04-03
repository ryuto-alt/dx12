#include "resource/ResourceManager.h"

#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/Texture.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "resource/TextureLoader.h"
#include "resource/ModelLoader.h"

namespace dx12e
{

void ResourceManager::Initialize(GraphicsDevice* device, DescriptorHeap* srvHeap,
                                  ID3D12GraphicsCommandList* cmdList)
{
    DX_ASSERT(device != nullptr, "device must not be null");
    DX_ASSERT(srvHeap != nullptr, "srvHeap must not be null");

    m_device  = device;
    m_srvHeap = srvHeap;

    // 1x1 白テクスチャ（テクスチャ無しメッシュ用のデフォルト）
    {
        u32 white = 0xFFFFFFFF;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 1;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        desc.SampleDesc = {1, 0};
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_SUBRESOURCE_DATA subData{};
        subData.pData = &white;
        subData.RowPitch = 4;
        subData.SlicePitch = 4;

        m_defaultWhite = std::make_unique<Texture>();
        m_defaultWhite->Initialize(*device, cmdList, desc, &subData, 1);

        u32 srvIdx = m_srvHeap->AllocateIndex();
        m_defaultWhite->SetSrvIndex(srvIdx);
        m_defaultWhite->CreateSRV(*device, m_srvHeap->GetCpuHandle(srvIdx));

        Logger::Info("Default white texture created (srvIndex={})", srvIdx);
    }

    // 1x1 デフォルト Normal テクスチャ (128,128,255,255 = flat normal pointing up)
    {
        u32 normalColor = 0xFFFF8080; // RGBA: R=128, G=128, B=255, A=255
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 1; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // linear (not sRGB)
        desc.SampleDesc = {1, 0};

        D3D12_SUBRESOURCE_DATA subData{};
        subData.pData = &normalColor; subData.RowPitch = 4; subData.SlicePitch = 4;

        m_defaultNormal = std::make_unique<Texture>();
        m_defaultNormal->Initialize(*device, cmdList, desc, &subData, 1);
        u32 nIdx = m_srvHeap->AllocateIndex();
        m_defaultNormal->SetSrvIndex(nIdx);
        m_defaultNormal->CreateSRV(*device, m_srvHeap->GetCpuHandle(nIdx));
    }

    // 1x1 デフォルト MetalRoughness テクスチャ (0,128,0,255 = metallic=0, roughness=0.5)
    {
        u32 mrColor = 0xFF008000; // RGBA: R=0, G=128, B=0, A=255
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 1; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // linear
        desc.SampleDesc = {1, 0};

        D3D12_SUBRESOURCE_DATA subData{};
        subData.pData = &mrColor; subData.RowPitch = 4; subData.SlicePitch = 4;

        m_defaultMetalRoughness = std::make_unique<Texture>();
        m_defaultMetalRoughness->Initialize(*device, cmdList, desc, &subData, 1);
        u32 mIdx = m_srvHeap->AllocateIndex();
        m_defaultMetalRoughness->SetSrvIndex(mIdx);
        m_defaultMetalRoughness->CreateSRV(*device, m_srvHeap->GetCpuHandle(mIdx));
    }
}

Texture* ResourceManager::GetOrLoadTexture(
    const std::wstring& filePath,
    ID3D12GraphicsCommandList* cmdList,
    bool srgb)
{
    // キャッシュチェック
    auto it = m_textureCache.find(filePath);
    if (it != m_textureCache.end())
    {
        return it->second.get();
    }

    // テクスチャ読み込み
    auto texture = TextureLoader::LoadFromFile(*m_device, cmdList, filePath, srgb);
    if (!texture)
    {
        Logger::Warn("Failed to load texture, returning nullptr");
        return nullptr;
    }

    // SRV 作成・インデックス設定
    u32 srvIdx = m_srvHeap->AllocateIndex();
    texture->SetSrvIndex(srvIdx);
    texture->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(srvIdx));

    // キャッシュ登録
    Texture* rawPtr = texture.get();
    m_textureCache[filePath] = std::move(texture);

    Logger::Info("Texture cached (srvIndex={})", rawPtr->GetSrvIndex());

    return rawPtr;
}

const CachedModel* ResourceManager::GetOrLoadModel(
    const std::string& filePath,
    ID3D12GraphicsCommandList* cmdList)
{
    auto it = m_modelCache.find(filePath);
    if (it != m_modelCache.end())
    {
        return it->second.get();
    }

    auto modelData = ModelLoader::LoadFromFile(*m_device, cmdList,
                                               std::filesystem::path(filePath), *this);

    auto cached = std::make_unique<CachedModel>();
    cached->meshes        = std::move(modelData.meshes);
    cached->materials     = std::move(modelData.materials);
    cached->skeleton      = std::move(modelData.skeleton);
    cached->animClips     = std::move(modelData.animClips);
    cached->nodeGraph     = std::move(modelData.nodeGraph);
    cached->nodeAnimClips = std::move(modelData.nodeAnimClips);

    const CachedModel* rawPtr = cached.get();
    m_modelCache[filePath] = std::move(cached);

    Logger::Info("Model cached: {} ({} meshes)", filePath, rawPtr->meshes.size());
    return rawPtr;
}

Texture* ResourceManager::GetOrLoadEmbeddedTexture(
    const std::string& key,
    const uint8_t* data, size_t dataSize,
    const char* formatHint,
    ID3D12GraphicsCommandList* cmdList)
{
    // wstring キーに変換してキャッシュ検索
    std::wstring wkey(key.begin(), key.end());
    auto it = m_textureCache.find(wkey);
    if (it != m_textureCache.end())
        return it->second.get();

    auto texture = TextureLoader::LoadFromMemory(*m_device, cmdList, data, dataSize, formatHint);
    if (!texture) return nullptr;

    u32 srvIdx = m_srvHeap->AllocateIndex();
    texture->SetSrvIndex(srvIdx);
    texture->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(srvIdx));
    auto* rawPtr = texture.get();
    m_textureCache[wkey] = std::move(texture);
    return rawPtr;
}

void ResourceManager::FinishUploads()
{
    if (m_defaultWhite) m_defaultWhite->FinishUpload();
    if (m_defaultNormal) m_defaultNormal->FinishUpload();
    if (m_defaultMetalRoughness) m_defaultMetalRoughness->FinishUpload();
    for (auto& [path, texture] : m_textureCache)
    {
        texture->FinishUpload();
    }
    for (auto& [path, model] : m_modelCache)
    {
        for (auto& mesh : model->meshes)
        {
            mesh->FinishUpload();
        }
    }
}

} // namespace dx12e
