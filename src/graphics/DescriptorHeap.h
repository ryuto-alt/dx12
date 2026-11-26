#pragma once

#include <Windows.h>

#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"
#include "core/IndexAllocator.h"

namespace dx12e
{

class GraphicsDevice;

class DescriptorHeap
{
public:
    // 確保失敗を表すセンチネル(IndexAllocator と同一)。
    static constexpr u32 kInvalidIndex = IndexAllocator::kInvalid;

    DescriptorHeap() = default;
    ~DescriptorHeap();

    DescriptorHeap(const DescriptorHeap&) = delete;
    DescriptorHeap& operator=(const DescriptorHeap&) = delete;

    void Initialize(GraphicsDevice& device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                    u32 numDescriptors, bool shaderVisible = false);

    // D3D12 のヒープを作らず、インデックスの管理(確保・遅延返却)だけを初期化する。単体テスト用。
    void InitializeBookkeepingOnly(u32 numDescriptors, bool shaderVisible = true);

    // CPU ハンドルを直接返す確保(RTV/DSV 用途)。枯渇時は ptr==0 のハンドルを返す。
    D3D12_CPU_DESCRIPTOR_HANDLE Allocate();
    // インデックスを返す確保(bindless 風 SRV 用途)。枯渇時は kInvalidIndex。
    u32 AllocateIndex();
    // count 個の連続インデックスを確保し先頭を返す(PBR の albedo/normal/mr 等)。枯渇時は kInvalidIndex。
    u32 AllocateBlock(u32 count);

    // 確保済みインデックス/ブロックの返却。これにより動的ロード/アンロードでも枯渇しない。
    // W05-06: 即時にはフリーリストへ戻さず、DeferredRelease で「現在投入中のフレームのフェンス」が
    // 完了した後に戻す(GPU が前フレームでまだ参照しているディスクリプタの上書きを防ぐ)。
    // DeferredRelease が無効(初期化前/終了後)のときは従来どおり即時。
    // シェーダから見えないヒープ(RTV/DSV 等)は GPU が実行時に読まないので遅延せず即時に戻す。
    // VG / Foliage の専用ヒープは DescriptorHeap ではなく ComPtr を直接持つので対象外
    // (外部ヒープモードで ApplicationVg が m_srvHeap->FreeBlock する分だけが遅延される)。
    void Free(u32 index);
    void FreeBlock(u32 start, u32 count);

    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(u32 index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(u32 index) const;

    ID3D12DescriptorHeap* GetHeap()           const { return m_heap.Get(); }
    u32                   GetDescriptorSize() const { return m_descriptorSize; }
    u32                   GetCapacity()       const { return m_numDescriptors; }
    u32                   GetFreeCount()      const { return m_allocator.FreeCount(); }

private:
    void FreeNow(u32 index);
    void FreeBlockNow(u32 start, u32 count);

    u64                          m_heapId = 0;   // 遅延返却コールバックが引く世代 id(0=未登録)。表は .cpp
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    u32                          m_descriptorSize   = 0;
    u32                          m_numDescriptors   = 0;
    IndexAllocator               m_allocator;
    D3D12_DESCRIPTOR_HEAP_TYPE   m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    D3D12_CPU_DESCRIPTOR_HANDLE  m_cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE  m_gpuStart{};
    bool                         m_shaderVisible = false;
};

} // namespace dx12e
