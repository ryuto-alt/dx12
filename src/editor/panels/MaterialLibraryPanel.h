#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/Types.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class EditorContext;
class ResourceManager;
class DescriptorHeap;
class MaterialAssetManager;

// Poly Haven(CC0・APIキー不要)のPBRテクスチャセットをエディタから直接検索・ダウンロードし、
// assets/textures/<id>/ へ保存 + assets/materials/<id>.dxmat を自動生成する
// 「エディタ内蔵マテリアルダウンローダー」（Unrealで言うQuixel Bridge/Fab相当）。
// ネットワークI/OはすべてワーカースレッドでUIをブロックしない。ImGui/D3D12呼び出しはUIスレッドのみ。
class MaterialLibraryPanel
{
public:
    void Initialize(ResourceManager* resourceManager, DescriptorHeap* srvHeap,
                    MaterialAssetManager* materialAssetManager);
    ~MaterialLibraryPanel();

    // フレーム先頭でcmdListが有効な間に呼ぶ(サムネイル/取得画像のGPUアップロード用)。
    void LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList);

    void RenderWindow(EditorContext& ctx, const std::string& assetsDir);

private:
    struct CatalogItem
    {
        std::string id;
        std::string name;
        std::vector<std::string> categories;
        std::vector<std::string> tags;
    };
    enum class CatalogState { Idle, Fetching, Ready, Failed };

    struct ThumbFetch
    {
        std::string id;
        std::thread worker;
        std::atomic<bool> done{false};
        std::vector<uint8_t> bytes;  // worker が書き込み、done=true 後に UI スレッドが読む
    };

    struct DownloadJob
    {
        std::string id;
        std::string resolution;
        std::thread worker;
        std::atomic<bool> done{false};
        std::atomic<bool> cancel{false};
        std::atomic<int>  phase{0};     // 0=取得中 1=書込中 2=完了 -1=失敗
        std::string error;              // phase==-1 のとき worker が書く。done=true 後のみ UI が読む(以降不変)
    };

    void EnsureCatalogRequested(const std::string& assetsDir);
    void ReapFinishedThumbFetches();
    void RequestThumbnail(const std::string& id);
    void DrawGrid(EditorContext& ctx, const std::string& assetsDir,
                  const std::vector<const CatalogItem*>& items);
    void StartDownload(const std::string& id, const std::string& assetsDir);
    bool IsDownloaded(const std::string& id, const std::string& assetsDir) const;

    ResourceManager*       m_resourceManager = nullptr;
    DescriptorHeap*        m_srvHeap = nullptr;
    MaterialAssetManager*  m_materialAssetManager = nullptr;

    // カタログ(Poly Haven /assets?type=textures)。ワーカー1本→m_catalogMutex経由でUIへ。
    std::atomic<CatalogState> m_catalogState{CatalogState::Idle};
    std::mutex                m_catalogMutex;
    std::vector<CatalogItem>  m_catalog;          // m_catalogMutex 保護
    std::thread               m_catalogThread;
    std::atomic<bool>         m_catalogDone{false};

    // サムネイル(cdn.polyhaven.com/asset_img/thumbs/<id>.png)。同時フェッチ数を制限する簡易プール。
    static constexpr size_t kMaxConcurrentThumbFetch = 4;
    std::unordered_map<std::string, u64> m_thumbGpuHandle;   // id -> GPUハンドル(0=未ロード/失敗)
    std::vector<std::string>             m_thumbRequested;   // 要求済み(再要求防止、成否問わず)
    std::vector<std::unique_ptr<ThumbFetch>> m_thumbInFlight;

    // ダウンロードジョブ(パネルの生存期間ぶん保持。数が少ないため破棄しない)。
    std::vector<std::unique_ptr<DownloadJob>> m_jobs;

    // UI状態
    char        m_searchBuf[128] = "";
    std::string m_activeCategory;    // 空 = フィルタなし
    int         m_resolutionIndex = 1;   // 0=1k, 1=2k, 2=4k
    bool        m_templatesTab = true;
    std::string m_statusMsg;
    f32         m_statusFlash = 0.0f;
};

} // namespace dx12e
