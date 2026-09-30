#pragma once

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wrl/client.h>
#include <directx/d3d12.h>
#include <DirectXMath.h>
#include "core/Types.h"
#include "editor/ThumbnailCache.h"   // ThumbGpu / CreateThumbTexture

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;
class ResourceManager;
class RootSignature;
class PipelineState;
class Mesh;

// ===== 3D モデルのサムネイル =====
// 以前は起動時に全モデルを同期で描き（1 枚ごと WaitIdle）、保存先も起動時の assets 固定だった。今は:
//   ・アセットブラウザが「今フレームに見えているセル」だけを Request する（BeginFrameRequests で毎フレーム作り直す＝
//     スクロールで見えなくなった要求は自動で消える）。
//   ・まず <assets>/.thumbcache/<hash>.utb（ヘッダに元ファイルの更新時刻 / サイズ / 生成バージョン）を読み、合えばアップロードだけ。
//     無ければ 1 フレームに 1 枚だけ描き、GPU が終わった数フレーム後にリードバックしてディスクへ保存する（WaitIdle しない）。
//   ・常駐数に上限を置き、使われていない物から遅延解放する。
class ModelThumbnailRenderer
{
public:
    void Initialize(GraphicsDevice* device,
                    DescriptorHeap* srvHeap,
                    ResourceManager* resourceManager,
                    RootSignature* rootSignature,
                    PipelineState* pipelineState);

    // プロジェクトの assets フォルダ。サムネイルは <assetsDir>/.thumbcache/ に置く。切替で常駐を全部捨てる。
    void SetCacheRoot(const std::string& assetsDir);

    // 毎フレーム、要求を出す前に 1 回（アセットブラウザの Render 先頭）。未処理の要求を捨てる。
    void BeginFrameRequests();
    // サムネイルを要求（mtime / size は一覧の値。元ファイルが更新されたら描き直す）。
    void Request(const std::string& modelPath, int64_t mtime, uint64_t size);

    // キャッシュから取得 (0 = まだない)
    u64 GetCachedHandle(const std::string& modelPath) const;

    // メインスレッド・フレーム先頭（cmdList が有効な間）に 1 回。ディスクキャッシュのアップロード / 描画 / 保存 / 解放。
    void RenderPending(ID3D12GraphicsCommandList* cmdList, u32 frameIndex);

    size_t GetPendingCount() const { return m_queue.size(); }
    size_t ResidentCount() const { return m_resident; }
    u64    RenderedTotal() const { return m_renderedTotal; }
    u64    DiskLoadedTotal() const { return m_diskLoadedTotal; }

    // SSAO 白ダミー(R8_UNORM, AO=1.0)の SRV index。forward PS が t8 を無条件読みするので必ずバインドする。
    void SetAOWhiteSrv(u32 srvIndex) { m_aoWhiteSrvIndex = srvIndex; }

    // クラスタライト SRV テーブル(t13,t14,t15 + デカール予約)の先頭 index。
    // サムネイルは灯数 0 なので中身は読まれないが、forward PS が t13..t15 を参照する以上
    // テーブルのバインド自体は必要（未バインドはルートシグネチャ検証違反）。
    void SetClusterSrv(u32 srvIndex) { m_clusterSrvIndex = srvIndex; }
    // SSR(t16)/SSGI(t17) の 1x1 黒ダミー(RGBA16F)。サムネイルは常にこれを張る。
    void SetScreenSpaceBlackSrv(u32 srvIndex) { m_ssBlackSrvIndex = srvIndex; }

    static constexpr u32 kThumbSize = 128;
    static constexpr int kDiskLoadsPerFrame = 6;   // 1 フレームにディスクキャッシュから上げる枚数
    static constexpr int kRendersPerFrame   = 1;   // 1 フレームに描く枚数（モデルの読み込みを伴うので重い）
    static constexpr size_t kMaxResident    = 600;

private:
    static constexpr u32 kThumbRowPitch = kThumbSize * 4;              // 512（256 の倍数）
    static constexpr u32 kThumbDataSize = kThumbRowPitch * kThumbSize; // 65536

    struct Req { std::string path; int64_t mtime = 0; uint64_t size = 0; };
    struct ThumbEntry
    {
        ThumbGpu gpu;
        int64_t  mtime = 0;
        uint64_t size = 0;
        bool     failed = false;          // モデルが読めなかった（元ファイルが変わるまで再試行しない）
        mutable u64 lastUsed = 0;
    };
    struct PendingSave
    {
        std::string path;
        int64_t  mtime = 0;
        uint64_t size = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        u64 frame = 0;
    };
    struct Grave { ThumbGpu gpu; u64 frame = 0; };

    void CreateSharedResources();
    bool RenderOne(const Req& req, ID3D12GraphicsCommandList* cmdList);
    std::string CacheFilePath(const std::string& modelPath) const;

    GraphicsDevice*   m_device        = nullptr;
    DescriptorHeap*   m_srvHeap       = nullptr;
    ResourceManager*  m_resourceMgr   = nullptr;
    RootSignature*    m_rootSig       = nullptr;
    PipelineState*    m_pso           = nullptr;
    u32               m_aoWhiteSrvIndex = 0xFFFFFFFFu;  // SSAO 白ダミー(t8) SRV index
    u32               m_clusterSrvIndex = 0xFFFFFFFFu;  // クラスタライトテーブル(t13..) 先頭 index
    u32               m_ssBlackSrvIndex = 0xFFFFFFFFu;  // SSR/SSGI 黒ダミー(t16,t17) SRV index

    // 共有リソース
    Microsoft::WRL::ComPtr<ID3D12Resource>         m_depthBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_rtvHeap;   // 1 descriptor
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_dsvHeap;   // 1 descriptor
    Microsoft::WRL::ComPtr<ID3D12Resource>         m_perFrameUpload; // 1536B upload heap (FrameConstants)
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle{};

    std::unordered_map<std::string, ThumbEntry> m_cache;
    std::deque<Req> m_queue;                       // 今フレームの要求（BeginFrameRequests で空にする）
    std::unordered_set<std::string> m_queued;
    std::unordered_map<std::string, std::pair<int64_t, uint64_t>> m_diskMiss;   // ディスクキャッシュに無かった物（毎フレーム開き直さない）
    std::vector<PendingSave> m_saves;              // GPU の完了待ち（数フレーム後にリードバック → ディスク）
    std::deque<Grave> m_graveyard;
    std::deque<std::pair<u64, Microsoft::WRL::ComPtr<ID3D12Resource>>> m_uploadKeep;
    std::string m_cacheDir;
    size_t m_resident = 0;
    u64 m_frame = 0;
    u64 m_renderedTotal = 0;
    u64 m_diskLoadedTotal = 0;
};

} // namespace dx12e
