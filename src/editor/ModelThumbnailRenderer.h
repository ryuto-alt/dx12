#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <wrl/client.h>
#include <directx/d3d12.h>
#include <DirectXMath.h>
#include "core/Types.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;
class ResourceManager;
class RootSignature;
class PipelineState;
class Mesh;

class ModelThumbnailRenderer
{
public:
    void Initialize(GraphicsDevice* device,
                    DescriptorHeap* srvHeap,
                    ResourceManager* resourceManager,
                    RootSignature* rootSignature,
                    PipelineState* pipelineState);

    // 1フレームに1つずつサムネイルをレンダリング
    void RenderPending(ID3D12GraphicsCommandList* cmdList, u32 frameIndex);

    // 全モデルをスキャンしてキューに積む。モデル数を返す
    size_t ScanAllModels(const std::string& assetsDir);

    // キューから1つレンダリング。残り数を返す
    size_t RenderNext(ID3D12GraphicsCommandList* cmdList);

    size_t GetPendingCount() const { return m_pendingQueue.size(); }
    size_t GetTotalScanned() const { return m_totalScanned; }

    // サムネイルをリクエスト
    void Request(const std::string& modelPath);

    // キャッシュから取得 (0 = まだない)
    u64 GetCachedHandle(const std::string& modelPath) const;

    // ディスクキャッシュからサムネイルを一括ロード（コマンドリストに積む）
    void LoadCachedThumbnails(ID3D12GraphicsCommandList* cmdList);
    size_t GetCachedCount() const { return m_cachedPaths.size(); }

    // 直前にレンダリングしたサムネイルをディスクに保存（WaitIdle後に呼ぶ）
    void SavePendingCache();

    // SSAO 白ダミー(R8_UNORM, AO=1.0)の SRV index。forward PS が t8 を無条件読みするので必ずバインドする。
    void SetAOWhiteSrv(u32 srvIndex) { m_aoWhiteSrvIndex = srvIndex; }

private:
    static constexpr u32 kThumbSize     = 128;
    static constexpr u32 kThumbRowPitch = kThumbSize * 4; // 512, aligned to 256
    static constexpr u32 kThumbDataSize = kThumbRowPitch * kThumbSize; // 65536

    struct ThumbEntry
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        u32 srvIndex = 0xFFFFFFFF;
        u64 gpuHandle = 0;
    };

    void CreateSharedResources();
    void RenderOne(const std::string& modelPath, ID3D12GraphicsCommandList* cmdList);
    std::string GetCacheFilePath(const std::string& modelPath) const;

    GraphicsDevice*   m_device        = nullptr;
    DescriptorHeap*   m_srvHeap       = nullptr;
    ResourceManager*  m_resourceMgr   = nullptr;
    RootSignature*    m_rootSig       = nullptr;
    PipelineState*    m_pso           = nullptr;
    u32               m_aoWhiteSrvIndex = 0xFFFFFFFFu;  // SSAO 白ダミー(t8) SRV index

    // 共有リソース
    Microsoft::WRL::ComPtr<ID3D12Resource>         m_depthBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_rtvHeap;   // 1 descriptor
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_dsvHeap;   // 1 descriptor
    Microsoft::WRL::ComPtr<ID3D12Resource>         m_perFrameUpload; // 256B upload heap
    Microsoft::WRL::ComPtr<ID3D12Resource>         m_readbackBuffer; // for cache save
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle{};

    // キャッシュ
    std::unordered_map<std::string, ThumbEntry> m_cache;
    std::vector<std::string> m_pendingQueue;   // レンダリングが必要
    std::vector<std::string> m_cachedPaths;    // ディスクキャッシュあり
    // アップロードバッファ保持（コマンド実行完了まで生存させる）
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_uploadBuffers;
    std::string m_cacheDir;
    std::string m_lastRenderedPath; // SavePendingCache 用
    size_t m_totalScanned = 0;
};

} // namespace dx12e
