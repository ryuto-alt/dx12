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

private:
    static constexpr u32 kThumbSize = 128;

    struct ThumbEntry
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        u32 srvIndex = 0xFFFFFFFF;
        u64 gpuHandle = 0;
    };

    void CreateSharedResources();
    void RenderOne(const std::string& modelPath, ID3D12GraphicsCommandList* cmdList);

    GraphicsDevice*   m_device        = nullptr;
    DescriptorHeap*   m_srvHeap       = nullptr;
    ResourceManager*  m_resourceMgr   = nullptr;
    RootSignature*    m_rootSig       = nullptr;
    PipelineState*    m_pso           = nullptr;

    // 共有リソース
    Microsoft::WRL::ComPtr<ID3D12Resource>         m_depthBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_rtvHeap;   // 1 descriptor
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_dsvHeap;   // 1 descriptor
    Microsoft::WRL::ComPtr<ID3D12Resource>         m_perFrameUpload; // 256B upload heap
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle{};

    // キャッシュ
    std::unordered_map<std::string, ThumbEntry> m_cache;
    std::vector<std::string> m_pendingQueue;
    size_t m_totalScanned = 0;
};

} // namespace dx12e
