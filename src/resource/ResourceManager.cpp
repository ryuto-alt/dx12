#include "resource/ResourceManager.h"

#include "core/Assert.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"
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
    m_uploadsPending = true;  // 既定テクスチャのアップロードを初回フレームで確定させる
}

Texture* ResourceManager::GetOrLoadTexture(
    const std::wstring& filePath,
    ID3D12GraphicsCommandList* cmdList,
    bool srgb,
    TextureUsage usage)
{
    // キャッシュチェック
    auto it = m_textureCache.find(filePath);
    if (it != m_textureCache.end())
    {
        return it->second.get();
    }

    // VFS 経由で読み込みを試みる（ゲームモード: pak から復号+展開、ディスクモード: ルーズファイル）
    // 拡張子から formatHint を決定 (.dds → "dds"、それ以外 → "")
    std::unique_ptr<Texture> texture;
    {
        const auto dotPos = filePath.rfind(L'.');
        const std::string formatHint =
            (dotPos != std::wstring::npos &&
             (filePath.substr(dotPos) == L".dds" || filePath.substr(dotPos) == L".DDS"))
            ? "dds" : "";

        auto bytes = vfs::ReadAssetAbs(filePath);
        if (!bytes.empty())
        {
            texture = TextureLoader::LoadFromMemory(
                *m_device, cmdList,
                bytes.data(), bytes.size(),
                formatHint.c_str(),
                srgb, usage,
                /*cacheKey=*/PathResolver::WideToUtf8(filePath));
        }
    }

    // VFS が空（ディスクモード / マウント前 / ファイル不在）なら元のファイル読み込みにフォールバック
    if (!texture)
    {
        texture = TextureLoader::LoadFromFile(*m_device, cmdList, filePath, srgb, usage);
    }

    if (!texture)
    {
        Logger::Warn("テクスチャの読み込みに失敗しました（nullptr をキャッシュし、以後は再試行しません）");
        m_textureCache[filePath] = nullptr;   // 失敗もキャッシュ＝毎フレーム再ロード試行してログが埋まるのを防ぐ
        return nullptr;
    }

    // SRV 作成・インデックス設定
    u32 srvIdx = m_srvHeap->AllocateIndex();
    texture->SetSrvIndex(srvIdx);
    texture->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(srvIdx));

    // キャッシュ登録
    Texture* rawPtr = texture.get();
    m_textureCache[filePath] = std::move(texture);
    m_pendingUploads.push_back(rawPtr);   // フレーム末尾にステージングを遅延解放
    m_uploadsPending = true;

    Logger::Info("Texture cached (srvIndex={})", rawPtr->GetSrvIndex());

    return rawPtr;
}

const CachedModel* ResourceManager::GetOrLoadModel(
    const std::string& filePath,
    ID3D12GraphicsCommandList* cmdList)
{
    // キャッシュキーはパス正規化して作る。同じファイルでも D&D 配置(バックスラッシュ
    // 絶対パス)と Play→Stop のシーン復元(AssetsDir + 相対 = スラッシュ区切り)で
    // 文字列が異なりキャッシュミスし、Stop のたびにディスクから再ロードして編集中と
    // 別ジオメトリ(外部ツールで書き換わった後の内容等)を拾うバグがあった。
    std::error_code ec;
    std::string key = std::filesystem::weakly_canonical(
        std::filesystem::path(filePath), ec).generic_string();
    if (ec || key.empty())
        key = std::filesystem::path(filePath).lexically_normal().generic_string();

    auto it = m_modelCache.find(key);
    if (it != m_modelCache.end())
    {
        return it->second.get();
    }

    auto modelData = ModelLoader::LoadFromFile(*m_device, cmdList,
                                               std::filesystem::path(filePath), *this);

    if (modelData.meshes.empty())
    {
        Logger::Warn("モデルの読み込みに失敗しました（nullptr をキャッシュし、以後は再試行しません）: {}", filePath);
        m_modelCache[key] = nullptr;   // 失敗もキャッシュ＝毎フレーム再ロード試行してログが埋まるのを防ぐ
        return nullptr;
    }

    auto cached = std::make_unique<CachedModel>();
    cached->meshes        = std::move(modelData.meshes);
    cached->materials     = std::move(modelData.materials);
    cached->skeleton      = std::move(modelData.skeleton);
    cached->animClips     = std::move(modelData.animClips);
    cached->nodeGraph     = std::move(modelData.nodeGraph);
    cached->nodeAnimClips = std::move(modelData.nodeAnimClips);

    const CachedModel* rawPtr = cached.get();
    m_modelCache[key] = std::move(cached);
    // メッシュの VB/IB ステージングをフレーム末尾に遅延解放（テクスチャと同じ経路）
    for (auto& mesh : rawPtr->meshes)
        m_pendingMeshUploads.push_back(mesh.get());
    m_uploadsPending = true;

    Logger::Info("Model cached: {} ({} meshes)", filePath, rawPtr->meshes.size());
    return rawPtr;
}

Texture* ResourceManager::GetOrLoadEmbeddedTexture(
    const std::string& key,
    const uint8_t* data, size_t dataSize,
    const char* formatHint,
    ID3D12GraphicsCommandList* cmdList,
    bool srgb,
    TextureUsage usage)
{
    // wstring キーに変換してキャッシュ検索（UTF-8正変換。バイトコピーだと
    // 日本語キーが壊れて wstring パス経由のキャッシュと不一致になる）
    std::wstring wkey = PathResolver::Utf8ToWide(key);
    auto it = m_textureCache.find(wkey);
    if (it != m_textureCache.end())
        return it->second.get();

    auto texture = TextureLoader::LoadFromMemory(*m_device, cmdList, data, dataSize, formatHint,
                                                 srgb, usage, /*cacheKey=*/key);
    if (!texture) return nullptr;

    u32 srvIdx = m_srvHeap->AllocateIndex();
    texture->SetSrvIndex(srvIdx);
    texture->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(srvIdx));
    auto* rawPtr = texture.get();
    m_textureCache[wkey] = std::move(texture);
    m_pendingUploads.push_back(rawPtr);   // フレーム末尾にステージングを遅延解放
    m_uploadsPending = true;
    return rawPtr;
}

void ResourceManager::DeferPendingUploads()
{
    // Texture/Mesh::FinishUpload は DeferredRelease 有効時、フェンス連動の遅延解放になる
    for (Texture* t : m_pendingUploads)
        t->FinishUpload();
    m_pendingUploads.clear();
    for (Mesh* m : m_pendingMeshUploads)
        m->FinishUpload();
    m_pendingMeshUploads.clear();
    m_uploadsPending = false;
}

void ResourceManager::FinishUploads()
{
    if (m_defaultWhite) m_defaultWhite->FinishUpload();
    if (m_defaultNormal) m_defaultNormal->FinishUpload();
    if (m_defaultMetalRoughness) m_defaultMetalRoughness->FinishUpload();
    for (auto& [path, texture] : m_textureCache)
    {
        if (texture) texture->FinishUpload();   // 失敗キャッシュ(nullptr)はスキップ
    }
    for (auto& [path, model] : m_modelCache)
    {
        if (!model) continue;   // 失敗キャッシュ(nullptr)はスキップ(テクスチャ側と同じ)
        for (auto& mesh : model->meshes)
        {
            mesh->FinishUpload();
        }
    }
    m_uploadsPending = false;
}

} // namespace dx12e
