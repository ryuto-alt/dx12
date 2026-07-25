#include "resource/MaterialAssetManager.h"

#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/Texture.h"
#include "resource/ResourceManager.h"

#include <cctype>

namespace dx12e
{

// GetOrLoad/Invalidate 共通のキー正規化(小文字・'/'区切り)。
// Application::NormalizeCustomShaderKey と同じ方針(大文字小文字/区切り文字の揺れ吸収)。
static std::string NormalizeKey(const std::string& relPath)
{
    std::string key = relPath;
    for (char& c : key)
    {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
}

void MaterialAssetManager::Initialize(ResourceManager* resourceManager, GraphicsDevice* device, DescriptorHeap* srvHeap)
{
    m_resourceManager = resourceManager;
    m_device  = device;
    m_srvHeap = srvHeap;
}

void MaterialAssetManager::LoadInto(Entry& entry, const std::string& relPath, ID3D12GraphicsCommandList* cmdList)
{
    entry.valid = false;
    entry.attempted = true;
    entry.hasNormalTex = false;
    entry.hasMRTex = false;

    // mtime は成否に関わらずここで先に刻む(失敗時も含めて)。こうすることで
    // PollHotReload は「ファイルが変化したときだけ」再試行し、壊れたままの .dxmat を毎回叩かない。
    if (!vfs::InGameMode())
    {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(PathResolver::AssetsDir() + relPath, ec);
        if (!ec) entry.mtime = t;
    }

    std::vector<uint8_t> bytes = vfs::ReadAsset(relPath);
    MaterialAssetData data;
    if (!ParseMaterialAsset(bytes, data))
    {
        Logger::Warn("マテリアルアセットの読み込みに失敗しました: {}", relPath);
        return;
    }
    entry.data = data;

    // テクスチャ解決は Application::EnsureMaterialOverrideSrv と同じ方針:
    // albedo=sRGB、normal・metalRoughness(ARM)=linear。欠落分はデフォルトへフォールバックする。
    auto resolve = [&](const std::string& texRelPath, Texture* fallback, bool srgb,
                       TextureUsage usage) -> Texture* {
        if (texRelPath.empty())
            return fallback;
        if (!vfs::Exists(texRelPath))
        {
            Logger::Warn("マテリアルアセットのテクスチャが見つかりません: {} ({})", texRelPath, relPath);
            return fallback;
        }
        std::string fullPath = PathResolver::AssetsDir() + texRelPath;
        return m_resourceManager->GetOrLoadTexture(PathResolver::Utf8ToWide(fullPath), cmdList, srgb, usage);
    };

    Texture* albedo = resolve(data.albedoPath, m_resourceManager->GetDefaultWhiteTexture(),
                              /*srgb=*/true,  TextureUsage::BaseColor);
    Texture* normal = resolve(data.normalPath, m_resourceManager->GetDefaultNormalTexture(),
                              /*srgb=*/false, TextureUsage::Normal);
    Texture* mr     = resolve(data.metalRoughnessPath, m_resourceManager->GetDefaultMetalRoughnessTexture(),
                              /*srgb=*/false, TextureUsage::NonColor);
    if (!albedo || !normal || !mr)
        return;

    if (entry.srvBlockStart == 0xFFFFFFFF)
        entry.srvBlockStart = m_srvHeap->AllocateBlock(3);

    albedo->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(entry.srvBlockStart));
    normal->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(entry.srvBlockStart + 1));
    mr->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(entry.srvBlockStart + 2));

    entry.hasNormalTex = !data.normalPath.empty();
    entry.hasMRTex     = !data.metalRoughnessPath.empty();
    entry.valid = true;
}

const MaterialAssetManager::Entry* MaterialAssetManager::GetOrLoad(const std::string& relPath, ID3D12GraphicsCommandList* cmdList)
{
    if (relPath.empty())
        return nullptr;

    const std::string key = NormalizeKey(relPath);
    auto it = m_cache.find(key);
    if (it != m_cache.end() && (it->second.valid || it->second.attempted))
        return &it->second;

    Entry& entry = m_cache[key];  // 新規 or Invalidate 済み(blockStart はあれば再利用)
    LoadInto(entry, relPath, cmdList);
    return &entry;
}

void MaterialAssetManager::Invalidate(const std::string& relPath)
{
    const std::string key = NormalizeKey(relPath);
    auto it = m_cache.find(key);
    if (it == m_cache.end())
        return;

    // SRV ブロックは再利用する(LoadInto が blockStart==0xFFFFFFFF のときだけ確保するため)。
    // valid と attempted の両方を落として、次の GetOrLoad で確実に読み直させる。
    it->second.valid = false;
    it->second.attempted = false;
}

void MaterialAssetManager::UpdateScalarsOnly(const std::string& relPath, f32 metallic, f32 roughness,
                                              f32 uvTilingU, f32 uvTilingV)
{
    const std::string key = NormalizeKey(relPath);
    auto it = m_cache.find(key);
    if (it == m_cache.end() || !it->second.valid)
        return;
    it->second.data.metallic  = metallic;
    it->second.data.roughness = roughness;
    it->second.data.uvTilingU = uvTilingU;
    it->second.data.uvTilingV = uvTilingV;
}

void MaterialAssetManager::PollHotReload(f32 dt, ID3D12GraphicsCommandList* cmdList)
{
    if (vfs::InGameMode())
        return;

    m_pollTimer += dt;
    if (m_pollTimer < 0.5f)
        return;
    m_pollTimer = 0.0f;

    for (auto& [key, entry] : m_cache)
    {
        if (!entry.attempted)
            continue;  // 一度もロードを試みていない(=まだ誰も参照していない)エントリは対象外
        std::error_code ec;
        auto t = std::filesystem::last_write_time(PathResolver::AssetsDir() + key, ec);
        if (ec || t == entry.mtime)
            continue;
        LoadInto(entry, key, cmdList);
    }
}

} // namespace dx12e
