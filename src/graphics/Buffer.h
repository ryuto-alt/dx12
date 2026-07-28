#pragma once

#include "graphics/GpuResource.h"
#include "core/Types.h"

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <D3D12MemAlloc.h>

namespace dx12e
{

class GraphicsDevice;

// ---------------------------------------------------------------------------
// Buffer base
// ---------------------------------------------------------------------------
class Buffer : public GpuResource
{
public:
    u32 GetSizeInBytes() const { return m_sizeInBytes; }

protected:
    void CreateBuffer(
        D3D12MA::Allocator* allocator,
        u32 sizeInBytes,
        D3D12_RESOURCE_STATES initialState,
        D3D12_HEAP_TYPE heapType);

    u32 m_sizeInBytes = 0;
};

// ---------------------------------------------------------------------------
// VertexBuffer
// ---------------------------------------------------------------------------
class VertexBuffer : public Buffer
{
public:
    // cmd を渡すと DEFAULT ヒープ(VRAM)本体 + UPLOAD ステージングで作り、cmd にコピーを積む
    // （大型メッシュは毎フレームの PCIe 読み出しが消えて桁違いに速い）。ステージングは
    // FinishUpload()（DeferredRelease 連動）で解放する。cmd=nullptr は従来どおり UPLOAD 直
    // （動的/小型バッファ・コマンドリストが無い場面用）。
    void Initialize(GraphicsDevice& device, const void* data, u32 sizeInBytes, u32 strideInBytes,
                    ID3D12GraphicsCommandList* cmd = nullptr);
    void FinishUpload();

    const D3D12_VERTEX_BUFFER_VIEW& GetView() const { return m_view; }

    // ByteAddressBuffer(RAW) として SRV を張る（計画09 Step 5 / バインドレス）。
    // レイのヒット点で頂点属性（UV / 法線 / 頂点カラー）を読むために要る。
    // ★リソースは Initialize で GENERIC_READ に置かれたままなので追加バリアは不要。
    void CreateSRV(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

private:
    D3D12_VERTEX_BUFFER_VIEW                   m_view{};
    Microsoft::WRL::ComPtr<ID3D12Resource>     m_uploadBuffer;
};

// ---------------------------------------------------------------------------
// IndexBuffer
// ---------------------------------------------------------------------------
class IndexBuffer : public Buffer
{
public:
    // cmd の意味は VertexBuffer::Initialize と同じ（DEFAULT ヒープ + ステージングコピー）。
    void Initialize(GraphicsDevice& device, const u32* indices, u32 indexCount,
                    ID3D12GraphicsCommandList* cmd = nullptr);
    void FinishUpload();

    const D3D12_INDEX_BUFFER_VIEW& GetView()       const { return m_view; }
    u32                            GetIndexCount()  const { return m_indexCount; }

    // VertexBuffer::CreateSRV と同じ理由。インデックスは u32 固定なので RAW で読める。
    void CreateSRV(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

private:
    D3D12_INDEX_BUFFER_VIEW                    m_view{};
    Microsoft::WRL::ComPtr<ID3D12Resource>     m_uploadBuffer;
    u32                                        m_indexCount = 0;
};

// ---------------------------------------------------------------------------
// ConstantBuffer
// ---------------------------------------------------------------------------
class ConstantBuffer : public Buffer
{
public:
    ~ConstantBuffer() override;

    void Initialize(GraphicsDevice& device, u32 elementSizeInBytes, u32 frameCount);
    void Update(const void* data, u32 sizeInBytes, u32 frameIndex);
    D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress(u32 frameIndex) const;

private:
    u8* m_mappedData   = nullptr;
    u32 m_alignedSize  = 0;
    u32 m_frameCount   = 0;
};

} // namespace dx12e
