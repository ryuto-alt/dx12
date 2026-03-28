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
}

Texture* ResourceManager::GetOrLoadTexture(
    const std::wstring& filePath,
    ID3D12GraphicsCommandList* cmdList)
{
    // キャッシュチェック
    auto it = m_textureCache.find(filePath);
    if (it != m_textureCache.end())
    {
        return it->second.get();
    }

    // テクスチャ読み込み
    auto texture = TextureLoader::LoadFromFile(*m_device, cmdList, filePath);
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
    cached->meshes    = std::move(modelData.meshes);
    cached->materials = std::move(modelData.materials);
    cached->skeleton  = std::move(modelData.skeleton);
    cached->animClips = std::move(modelData.animClips);

    const CachedModel* rawPtr = cached.get();
    m_modelCache[filePath] = std::move(cached);

    Logger::Info("Model cached: {} ({} meshes)", filePath, rawPtr->meshes.size());
    return rawPtr;
}

void ResourceManager::FinishUploads()
{
    if (m_defaultWhite) m_defaultWhite->FinishUpload();
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
