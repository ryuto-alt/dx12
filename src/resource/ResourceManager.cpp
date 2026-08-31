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
#include "renderer/Mesh.h"
#include "renderer/Material.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

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

namespace
{
// テクスチャキャッシュのキー。パスだけだと、同じ画像を別の色空間/用途で要求しても
// 先に読まれた方が返ってしまう（法線マップが sRGB 版になる等）。
// ディスク上の更新時刻。取れなければ 0（= 常に「変わった」扱いにはせず force 待ちにする）。
int64_t FileStamp(const std::string& path)
{
    if (path.empty()) return 0;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(std::filesystem::path(path), ec);
    if (ec) return 0;
    return static_cast<int64_t>(t.time_since_epoch().count());
}

std::wstring MakeTextureCacheKey(const std::wstring& path, bool srgb, TextureUsage usage,
                                 uint32_t maxDim)
{
    return path + (srgb ? L"|s1|u" : L"|s0|u") + std::to_wstring(static_cast<int>(usage))
                + L"|m" + std::to_wstring(maxDim);
}
}

Texture* ResourceManager::GetOrLoadTexture(
    const std::wstring& filePath,
    ID3D12GraphicsCommandList* cmdList,
    bool srgb,
    TextureUsage usage,
    u32 maxDimension)
{
    // ★キーはパスだけではいけない。srgb / usage は「後から効かせる設定」ではなく
    //   テクスチャの実体（SRV フォーマット・BC 圧縮形式・ミップのフィルタ）を変える
    //   （TextureLoader.cpp の SelectViewFormat / SelectCompressedFormat / EnsureMipChain）。
    //   パス単独キーだと、シーン先読み（ApplicationScene.cpp の WarmSceneAssetRef は
    //   srgb=true 既定）やアセットブラウザのサムネイル（srgb=true 固定）が先に読んだだけで、
    //   後から srgb=false / TextureUsage::Normal で要求しても **sRGB 版が返る**。
    //   法線マップがガンマ解除されてライティングが静かに狂う（ログも警告も出ない）。
    //   1 層下のディスク圧縮キャッシュ（TextureLoader.cpp:228）は既に usage を混ぜてある。
    const std::wstring cacheKey = MakeTextureCacheKey(filePath, srgb, usage, maxDimension);

    auto it = m_textureCache.find(cacheKey);
    if (it != m_textureCache.end())
    {
        return it->second.texture.get();
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
                /*cacheKey=*/PathResolver::WideToUtf8(filePath),
                maxDimension);
        }
    }

    // VFS が空（ディスクモード / マウント前 / ファイル不在）なら元のファイル読み込みにフォールバック
    if (!texture)
    {
        texture = TextureLoader::LoadFromFile(*m_device, cmdList, filePath, srgb, usage,
                                              maxDimension);
    }

    if (!texture)
    {
        Logger::Warn("テクスチャの読み込みに失敗しました（nullptr をキャッシュし、以後は再試行しません）");
        // 失敗もキャッシュ＝毎フレーム再ロード試行してログが埋まるのを防ぐ。
        // ★パスと更新時刻は控えておく（ファイルを直せば dx12_reload_assets で復帰できる）。
        TextureCacheEntry failed;
        failed.path         = filePath;
        failed.srgb         = srgb;
        failed.usage        = usage;
        failed.maxDimension = maxDimension;
        failed.stamp        = FileStamp(PathResolver::WideToUtf8(filePath));
        m_textureCache[cacheKey] = std::move(failed);
        return nullptr;
    }

    // SRV 作成・インデックス設定
    u32 srvIdx = m_srvHeap->AllocateIndex();
    texture->SetSrvIndex(srvIdx);
    texture->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(srvIdx));

    // キャッシュ登録
    Texture* rawPtr = texture.get();
    TextureCacheEntry entry;
    entry.texture      = std::move(texture);
    entry.path         = filePath;
    entry.srgb         = srgb;
    entry.usage        = usage;
    entry.maxDimension = maxDimension;
    entry.stamp        = FileStamp(PathResolver::WideToUtf8(filePath));
    entry.plain2d      = (rawPtr->GetArraySize() <= 1);
    m_textureCache[cacheKey] = std::move(entry);
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
    const std::string key = NormalizeModelKey(filePath);

    auto it = m_modelCache.find(key);
    if (it != m_modelCache.end())
    {
        return it->second.model.get();
    }

    auto modelData = ModelLoader::LoadFromFile(*m_device, cmdList,
                                               std::filesystem::path(filePath), *this);

    if (modelData.meshes.empty())
    {
        Logger::Warn("モデルの読み込みに失敗しました（nullptr をキャッシュし、以後は再試行しません）: {}", filePath);
        // 失敗もキャッシュ＝毎フレーム再ロード試行してログが埋まるのを防ぐ
        ModelCacheEntry failed;
        failed.path  = filePath;
        failed.stamp = FileStamp(filePath);
        m_modelCache[key] = std::move(failed);
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
    ModelCacheEntry entry;
    entry.model = std::move(cached);
    entry.path  = filePath;
    entry.stamp = FileStamp(filePath);
    m_modelCache[key] = std::move(entry);
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
    // 埋め込みテクスチャは常に等倍（サムネイル経路が無い）
    std::wstring wkey = MakeTextureCacheKey(PathResolver::Utf8ToWide(key), srgb, usage, 0);
    auto it = m_textureCache.find(wkey);
    if (it != m_textureCache.end())
        return it->second.texture.get();

    auto texture = TextureLoader::LoadFromMemory(*m_device, cmdList, data, dataSize, formatHint,
                                                 srgb, usage, /*cacheKey=*/key);
    if (!texture) return nullptr;

    u32 srvIdx = m_srvHeap->AllocateIndex();
    texture->SetSrvIndex(srvIdx);
    texture->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(srvIdx));
    auto* rawPtr = texture.get();
    TextureCacheEntry entry;
    entry.texture = std::move(texture);
    entry.plain2d = (rawPtr->GetArraySize() <= 1);
    // path は空のまま = 埋め込みテクスチャ。ディスク上に実体が無いのでホットリロードの対象外
    //（モデル本体を読み直せば一緒に更新される）。
    m_textureCache[wkey] = std::move(entry);
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

std::string ResourceManager::NormalizeModelKey(const std::string& filePath)
{
    // ★GetOrLoadModel のキーと必ず同じ式であること。ズレると「読み直したのに
    //   MeshRenderer が古い実体を指したまま」になる（絵が変わらない = 元の痛みに逆戻り）。
    std::error_code ec;
    std::string key = std::filesystem::weakly_canonical(
        std::filesystem::path(filePath), ec).generic_string();
    if (ec || key.empty())
        key = std::filesystem::path(filePath).lexically_normal().generic_string();
    return key;
}

const CachedModel* ResourceManager::FindModel(const std::string& key) const
{
    auto it = m_modelCache.find(key);
    return (it == m_modelCache.end()) ? nullptr : it->second.model.get();
}

void ResourceManager::RefreshMaterialSrvBlocks()
{
    // Material::srvBlockIndex は albedo/normal/metalRoughness の SRV を**コピーではなく
    // その場で作った**ものなので、テクスチャの ID3D12Resource を差し替えたら張り直しが要る。
    // 張り直さないと「テクスチャ単体を見る dx12_view_texture は新しいのに、
    // モデルに貼られた絵だけ古い」という最悪の食い違いになる。
    if (!m_device || !m_srvHeap) return;
    for (auto& [key, entry] : m_modelCache)
    {
        if (!entry.model) continue;
        for (auto& mat : entry.model->materials)
        {
            if (!mat || mat->srvBlockIndex == 0xFFFFFFFFu) continue;
            Texture* albedo = mat->albedoTexture         ? mat->albedoTexture         : m_defaultWhite.get();
            Texture* normal = mat->normalMapTexture      ? mat->normalMapTexture      : m_defaultNormal.get();
            Texture* mr     = mat->metalRoughnessTexture ? mat->metalRoughnessTexture : m_defaultMetalRoughness.get();
            if (!albedo || !normal || !mr) continue;
            albedo->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(mat->srvBlockIndex));
            normal->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(mat->srvBlockIndex + 1));
            mr    ->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(mat->srvBlockIndex + 2));
        }
    }
}

AssetReloadResult ResourceManager::ReloadChangedAssets(ID3D12GraphicsCommandList* cmdList,
                                                       const std::string& prefixUtf8, bool force)
{
    AssetReloadResult out;
    if (!m_device || !m_srvHeap || !cmdList) return out;

    const std::string prefix = prefixUtf8.empty()
        ? std::string()
        : std::filesystem::path(prefixUtf8).lexically_normal().generic_string();
    auto matches = [&](const std::string& p)
    {
        if (prefix.empty()) return true;
        const std::string n = std::filesystem::path(p).lexically_normal().generic_string();
        // フォルダ指定でも 1 ファイル指定でも同じ前方一致で拾う
        return n.size() >= prefix.size() && _strnicmp(n.c_str(), prefix.c_str(), prefix.size()) == 0;
    };

    // ---- 1) テクスチャ（同じ Texture / 同じ SRV インデックスのまま中身だけ差し替える）----
    bool anyTexture = false;
    for (auto& [key, entry] : m_textureCache)
    {
        if (entry.path.empty()) continue;                 // 埋め込み = ディスクに実体が無い
        const std::string u8 = PathResolver::WideToUtf8(entry.path);
        if (!matches(u8)) continue;
        ++out.checkedTextures;

        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(entry.path), ec)) continue;
        const int64_t now = FileStamp(u8);
        if (!force && (now == 0 || now == entry.stamp)) continue;

        if (entry.texture && !entry.plain2d)
        {
            out.skipped.push_back(u8 + ": cube/array テクスチャは張り直せない(シーンを開き直すこと)");
            continue;
        }

        auto fresh = TextureLoader::LoadFromFile(*m_device, cmdList, entry.path,
                                                 entry.srgb, entry.usage, entry.maxDimension);
        if (!fresh)
        {
            out.skipped.push_back(u8 + ": 読み込みに失敗(古い実体のまま)");
            continue;
        }

        if (entry.texture)
        {
            // 実体だけ差し替え、SRV は自分のスロットへ張り直す。
            // これで Material の SRV ブロック以外（スプライト / UI / スカイ）は何も直さずに済む。
            entry.texture->AdoptFrom(*fresh);
            entry.texture->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(entry.texture->GetSrvIndex()));
        }
        else
        {
            // 失敗キャッシュだった枠 = 初めて実体が入る。SRV インデックスを新規に払い出す。
            const u32 srvIdx = m_srvHeap->AllocateIndex();
            fresh->SetSrvIndex(srvIdx);
            fresh->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(srvIdx));
            entry.texture = std::move(fresh);
        }
        entry.plain2d = (entry.texture->GetArraySize() <= 1);
        entry.stamp   = now;
        m_pendingUploads.push_back(entry.texture.get());
        m_uploadsPending = true;
        anyTexture = true;
        out.textures.push_back(u8);
    }
    if (anyTexture) RefreshMaterialSrvBlocks();

    // ---- 2) モデル（CachedModel ごと作り直す＝Mesh*/Material* が全部変わる）----
    //  ★参照の張り替えは呼び出し側（Application::McpReloadAssets）。ここで返す models を
    //    見て MeshRenderer を再バインドし、BLAS キャッシュを捨てるまでが 1 セット。
    for (auto& [key, entry] : m_modelCache)
    {
        if (entry.path.empty() || !matches(entry.path)) continue;
        ++out.checkedModels;

        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(entry.path), ec)) continue;
        const int64_t now = FileStamp(entry.path);
        if (!force && (now == 0 || now == entry.stamp)) continue;

        auto data = ModelLoader::LoadFromFile(*m_device, cmdList,
                                              std::filesystem::path(entry.path), *this);
        if (data.meshes.empty())
        {
            out.skipped.push_back(entry.path + ": モデルの読み込みに失敗(古い実体のまま)");
            continue;
        }
        auto rebuilt = std::make_unique<CachedModel>();
        rebuilt->meshes        = std::move(data.meshes);
        rebuilt->materials     = std::move(data.materials);
        rebuilt->skeleton      = std::move(data.skeleton);
        rebuilt->animClips     = std::move(data.animClips);
        rebuilt->nodeGraph     = std::move(data.nodeGraph);
        rebuilt->nodeAnimClips = std::move(data.nodeAnimClips);

        for (auto& mesh : rebuilt->meshes) m_pendingMeshUploads.push_back(mesh.get());
        m_uploadsPending = true;

        // ★ここで古い CachedModel が死ぬ = 既存の MeshRenderer.meshes/materials と
        //   Mesh::m_material が全部ぶら下がる。呼び出し側は **同じ MCP コマンドの中で**
        //   （= 次の描画が始まる前に）必ず再バインドすること。
        //   今フレーム読んだばかりの古いメッシュがステージング待ち行列に残っていると
        //   死んだポインタを FinishUpload することになるので先に抜く。
        if (entry.model)
        {
            // 古いマテリアルが握っていた 3 連続 SRV ブロックを返す。Material は POD で
            // デストラクタが無いので、返さないと読み直すたびにディスクリプタが 3×マテリアル数
            // ずつ減り続ける(Mesh は自前で VB/IB の SRV を返すので対象外)。
            // ★解放は **新しい実体を作り終えてから**。先に返すと同じブロックが即再確保され、
            //   今フレームの GPU がまだ読んでいる SRV を書き換えることになる。
            for (auto& oldMat : entry.model->materials)
                if (oldMat && oldMat->srvBlockIndex != 0xFFFFFFFFu)
                    m_srvHeap->FreeBlock(oldMat->srvBlockIndex, 3);
            for (auto& oldMesh : entry.model->meshes)
                m_pendingMeshUploads.erase(
                    std::remove(m_pendingMeshUploads.begin(), m_pendingMeshUploads.end(),
                                oldMesh.get()),
                    m_pendingMeshUploads.end());
        }
        entry.model = std::move(rebuilt);
        entry.stamp = now;
        out.models.push_back(entry.path);
    }
    return out;
}

void ResourceManager::FinishUploads()
{
    if (m_defaultWhite) m_defaultWhite->FinishUpload();
    if (m_defaultNormal) m_defaultNormal->FinishUpload();
    if (m_defaultMetalRoughness) m_defaultMetalRoughness->FinishUpload();
    for (auto& [key, entry] : m_textureCache)
    {
        if (entry.texture) entry.texture->FinishUpload();   // 失敗キャッシュ(null)はスキップ
    }
    for (auto& [key, entry] : m_modelCache)
    {
        if (!entry.model) continue;   // 失敗キャッシュ(null)はスキップ(テクスチャ側と同じ)
        for (auto& mesh : entry.model->meshes)
        {
            mesh->FinishUpload();
        }
    }
    m_uploadsPending = false;
}

} // namespace dx12e
