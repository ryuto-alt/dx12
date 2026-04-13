#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include "core/Types.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class EditorContext;
class ResourceManager;
class DescriptorHeap;

class AssetBrowserPanel
{
public:
    void Initialize(const std::string& assetsDir,
                    ResourceManager* resourceManager,
                    DescriptorHeap* srvHeap);

    void Render(EditorContext& ctx, f32 dt);

    // フレーム先頭でcmdListが有効な間に呼ぶ（テクスチャアップロード用）
    void LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList);

    // ドラッグ&ドロップ用ペイロード型名
    static constexpr const char* kDragDropPayloadType = "ASSET_PATH";

private:
    enum class AssetType { Folder, Model, Texture, Scene, Script, Audio, Other };

    struct AssetEntry
    {
        std::filesystem::path path;
        std::string           displayName;
        AssetType             type = AssetType::Other;
        bool                  isDirectory = false;
    };

    // サムネイルキャッシュ（SRVのGPUハンドル値）
    struct ThumbnailInfo
    {
        u64  gpuHandle = 0;  // ImTextureID として使う
        bool loaded    = false;
        bool failed    = false;
    };

    void Refresh();
    static AssetType ClassifyExtension(const std::string& ext);
    ThumbnailInfo& GetOrLoadThumbnail(const std::filesystem::path& path,
                                       ID3D12GraphicsCommandList* cmdList);

    std::filesystem::path        m_assetsRoot;
    std::filesystem::path        m_currentDir;
    std::vector<AssetEntry>      m_entries;
    f32                          m_refreshTimer = 0.0f;
    static constexpr f32         kRefreshInterval = 2.0f;

    // サムネイル
    ResourceManager* m_resourceManager = nullptr;
    DescriptorHeap*  m_srvHeap         = nullptr;
    std::unordered_map<std::string, ThumbnailInfo> m_thumbnailCache;
    std::vector<std::string> m_pendingThumbnailLoads;  // ロード待ちパス

    // セルサイズ
    f32 m_cellSize = 96.0f;
};

} // namespace dx12e
