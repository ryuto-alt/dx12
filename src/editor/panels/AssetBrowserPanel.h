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
class ModelThumbnailRenderer;

class AssetBrowserPanel
{
public:
    void Initialize(const std::string& assetsDir,
                    const std::string& scriptsDir,
                    ResourceManager* resourceManager,
                    DescriptorHeap* srvHeap);

    void Render(EditorContext& ctx, f32 dt);
    void ForceRefresh() { Refresh(); }

    // プロジェクト切替時にルートを差し替える
    void SetRoots(const std::string& assetsDir, const std::string& scriptsDir)
    {
        m_assetsRoot  = std::filesystem::path(assetsDir);
        m_scriptsRoot = std::filesystem::path(scriptsDir);
        m_currentDir  = m_assetsRoot;
        m_thumbnailCache.clear();
        Refresh();
    }
    void SetThumbnailRenderer(ModelThumbnailRenderer* r) { m_thumbRenderer = r; }

    // フレーム先頭でcmdListが有効な間に呼ぶ（テクスチャアップロード用）
    void LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList);

    // ドラッグ&ドロップ用ペイロード型名
    static constexpr const char* kDragDropPayloadType = "ASSET_PATH";

    // テクスチャD&D(マテリアル割当)等、他パネルからも拡張子種別を判定したいので公開。
    enum class AssetType { Folder, Model, Texture, Scene, Script, Audio, Prefab, Shader, Material, Other };
    static AssetType ClassifyExtension(const std::string& ext);

    // 他パネル(InspectorPanelのマテリアルテクスチャプレビュー等)から、このパネルが持つ
    // サムネイルキャッシュを使い回すための公開アクセサ。キャッシュ済みならGPUハンドルを即返す。
    // 無ければロードキューに積んで 0 を返す(LoadPendingThumbnailsが数フレーム後に用意する)。
    u64 GetOrQueueThumbnail(const std::string& absPath);

private:
    struct AssetEntry
    {
        std::filesystem::path path;
        std::string           displayName;
        AssetType             type = AssetType::Other;
        bool                  isDirectory = false;
        // AssetType::Material のみ: .dxmat が参照する albedo テクスチャの絶対パス(サムネイル使い回し用)。
        // 未設定/パース失敗なら空(その場合は種別アイコンにフォールバック)。
        std::string           materialThumbSource;
    };

    struct ThumbnailInfo
    {
        u64  gpuHandle = 0;
        bool loaded    = false;
        bool failed    = false;
    };

    void Refresh();
    void DrawFolderTree(const std::filesystem::path& dir, bool& needRefresh);
    static const char* GetTypeIcon(AssetType type);
    // GetTypeColor は .cpp 内のみで使用（ImVec4 は imgui.h 依存）
    ThumbnailInfo& GetOrLoadThumbnail(const std::filesystem::path& path,
                                       ID3D12GraphicsCommandList* cmdList);

    std::filesystem::path        m_assetsRoot;
    std::filesystem::path        m_scriptsRoot;
    std::filesystem::path        m_currentDir;
    std::vector<AssetEntry>      m_entries;
    f32                          m_refreshTimer = 0.0f;
    static constexpr f32         kRefreshInterval = 0.5f;

    // フィルタ
    int m_filterIndex = 0;  // 0=All, 1=Models, 2=Scenes, 3=Textures, 4=Scripts, 5=Audio

    // サムネイル
    ResourceManager* m_resourceManager = nullptr;
    DescriptorHeap*  m_srvHeap         = nullptr;
    std::unordered_map<std::string, ThumbnailInfo> m_thumbnailCache;
    std::vector<std::string> m_pendingThumbnailLoads;

    // セルサイズ
    f32 m_cellSize = 96.0f;

    // 選択・削除（Del キー / 右クリック「削除」）
    std::filesystem::path m_selectedPath;       // 単一クリックで選択中のアセット
    std::filesystem::path m_pendingDeletePath;  // 削除確認中のアセット（確定で remove）
    bool                  m_deletePopupOpen = false;

    // モデルサムネイルレンダラー
    ModelThumbnailRenderer* m_thumbRenderer = nullptr;
};

} // namespace dx12e
