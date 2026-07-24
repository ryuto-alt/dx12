#include "graphics/Buffer.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DeferredRelease.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <cstring>

namespace dx12e
{

namespace
{
// UPLOAD ヒープのステージングバッファ（コピー元）。GpuResource 管理外の生 ComPtr。
Microsoft::WRL::ComPtr<ID3D12Resource> CreateStagingBuffer(GraphicsDevice& device, u32 sizeInBytes)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = sizeInBytes;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> r;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

// staging へ書き込み → cmd にコピー + 状態遷移を積む（VB/IB 共通）。
void RecordBufferUpload(ID3D12GraphicsCommandList* cmd, ID3D12Resource* dst,
                        ID3D12Resource* staging, const void* data, u32 sizeInBytes)
{
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    ThrowIfFailed(staging->Map(0, &readRange, &mapped));
    std::memcpy(mapped, data, sizeInBytes);
    staging->Unmap(0, nullptr);

    cmd->CopyBufferRegion(dst, 0, staging, 0, sizeInBytes);
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = dst;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_GENERIC_READ;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
}
} // anonymous namespace

// ===========================================================================
// Buffer base
// ===========================================================================
void Buffer::CreateBuffer(
    D3D12MA::Allocator* allocator,
    u32 sizeInBytes,
    D3D12_RESOURCE_STATES initialState,
    D3D12_HEAP_TYPE heapType)
{
    m_sizeInBytes = sizeInBytes;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment          = 0;
    desc.Width              = sizeInBytes;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = heapType;

    CreateResource(allocator, desc, initialState, allocDesc);
}

// ===========================================================================
// VertexBuffer
// ===========================================================================
void VertexBuffer::Initialize(GraphicsDevice& device, const void* data, u32 sizeInBytes, u32 strideInBytes,
                              ID3D12GraphicsCommandList* cmd)
{
    DX_ASSERT(data, "Vertex data must not be null");
    DX_ASSERT(sizeInBytes > 0, "Vertex buffer size must be > 0");

    if (cmd)
    {
        // DEFAULT ヒープ(VRAM)本体 + ステージングコピー。UPLOAD 直読みは PCIe 越えの
        // 頂点フェッチが毎フレーム発生し、大型メッシュ×多パスで GPU が数十倍遅くなる
        // （PerfTest 25万tri×201体で実測: 影23ms/メイン17ms → の主因）。
        // コピーは呼び出し側の cmd に積むだけ＝GPU ストールなし。
        CreateBuffer(device.GetAllocator(), sizeInBytes,
                     D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_DEFAULT);
        m_uploadBuffer = CreateStagingBuffer(device, sizeInBytes);
        RecordBufferUpload(cmd, m_resource.Get(), m_uploadBuffer.Get(), data, sizeInBytes);
    }
    else
    {
        // UPLOAD ヒープに直接作成して map+memcpy（動的/小型・コマンドリストが無い場面用）。
        CreateBuffer(device.GetAllocator(), sizeInBytes,
                     D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);

        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        ThrowIfFailed(m_resource->Map(0, &readRange, &mapped));
        std::memcpy(mapped, data, sizeInBytes);
        m_resource->Unmap(0, nullptr);
    }

    m_view.BufferLocation = m_resource->GetGPUVirtualAddress();
    m_view.SizeInBytes    = sizeInBytes;
    m_view.StrideInBytes  = strideInBytes;
}

void VertexBuffer::FinishUpload()
{
    // ステージングはコピーコマンドの GPU 完了まで生存が必要 → Texture と同じく
    // DeferredRelease 経由（有効時フェンス連動 / 無効時は即時 = WaitIdle 済み前提）。
    if (m_uploadBuffer)
        DeferredRelease::Defer(std::move(m_uploadBuffer), nullptr);
}

// ===========================================================================
// IndexBuffer
// ===========================================================================
void IndexBuffer::Initialize(GraphicsDevice& device, const u32* indices, u32 indexCount,
                             ID3D12GraphicsCommandList* cmd)
{
    DX_ASSERT(indices, "Index data must not be null");
    DX_ASSERT(indexCount > 0, "Index count must be > 0");

    u32 sizeInBytes = indexCount * sizeof(u32);
    m_indexCount    = indexCount;

    if (cmd)
    {
        // DEFAULT ヒープ + ステージングコピー（理由は VertexBuffer::Initialize 参照）
        CreateBuffer(device.GetAllocator(), sizeInBytes,
                     D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_DEFAULT);
        m_uploadBuffer = CreateStagingBuffer(device, sizeInBytes);
        RecordBufferUpload(cmd, m_resource.Get(), m_uploadBuffer.Get(), indices, sizeInBytes);
    }
    else
    {
        // UPLOAD ヒープに直接作成（動的/小型・コマンドリストが無い場面用）
        CreateBuffer(device.GetAllocator(), sizeInBytes,
                     D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);

        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        ThrowIfFailed(m_resource->Map(0, &readRange, &mapped));
        std::memcpy(mapped, indices, sizeInBytes);
        m_resource->Unmap(0, nullptr);
    }

    m_view.BufferLocation = m_resource->GetGPUVirtualAddress();
    m_view.SizeInBytes    = sizeInBytes;
    m_view.Format         = DXGI_FORMAT_R32_UINT;
}

void IndexBuffer::FinishUpload()
{
    if (m_uploadBuffer)
        DeferredRelease::Defer(std::move(m_uploadBuffer), nullptr);
}

// ===========================================================================
// ConstantBuffer
// ===========================================================================
ConstantBuffer::~ConstantBuffer()
{
    if (m_mappedData && m_resource)
    {
        m_resource->Unmap(0, nullptr);
        m_mappedData = nullptr;
    }
}

void ConstantBuffer::Initialize(GraphicsDevice& device, u32 elementSizeInBytes, u32 frameCount)
{
    DX_ASSERT(elementSizeInBytes > 0, "Element size must be > 0");
    DX_ASSERT(frameCount > 0, "Frame count must be > 0");

    m_alignedSize = (elementSizeInBytes + 255u) & ~255u;
    m_frameCount  = frameCount;

    u32 totalSize = m_alignedSize * frameCount;

    CreateBuffer(device.GetAllocator(), totalSize, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);

    // 永続 Map
    D3D12_RANGE readRange{0, 0};
    void* mapped = nullptr;
    ThrowIfFailed(m_resource->Map(0, &readRange, &mapped));
    m_mappedData = static_cast<u8*>(mapped);

    Logger::Info("ConstantBuffer created: aligned {} x {} frames = {} bytes", m_alignedSize, frameCount, totalSize);
}

void ConstantBuffer::Update(const void* data, u32 sizeInBytes, u32 frameIndex)
{
    DX_ASSERT(data, "Data must not be null");
    DX_ASSERT(frameIndex < m_frameCount, "Frame index out of range");
    DX_ASSERT(sizeInBytes <= m_alignedSize, "Data size exceeds aligned element size");

    std::memcpy(m_mappedData + static_cast<size_t>(frameIndex) * m_alignedSize, data, sizeInBytes);
}

D3D12_GPU_VIRTUAL_ADDRESS ConstantBuffer::GetGpuAddress(u32 frameIndex) const
{
    DX_ASSERT(frameIndex < m_frameCount, "Frame index out of range");
    return m_resource->GetGPUVirtualAddress() + static_cast<u64>(frameIndex) * m_alignedSize;
}

} // namespace dx12e
