#include "graphics/Buffer.h"
#include "graphics/GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <cstring>

namespace dx12e
{

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
void VertexBuffer::Initialize(GraphicsDevice& device, const void* data, u32 sizeInBytes, u32 strideInBytes)
{
    DX_ASSERT(data, "Vertex data must not be null");
    DX_ASSERT(sizeInBytes > 0, "Vertex buffer size must be > 0");

    // UPLOAD ヒープに直接作成して map+memcpy するだけ。
    // 旧実装は DEFAULT へコピーするため毎回 一時CommandQueue＋フェンス待ち(GPUフルストール)を
    // していた → メッシュ生成のたびにメインスレッドが固まる原因。これを撤廃して即時生成にする。
    // （アイコン/デバッグ描画も UPLOAD ヒープの頂点バッファを使用＝実績あり。
    //   小〜中サイズのメッシュなら GPU 読み出しコストは無視できる）
    CreateBuffer(device.GetAllocator(), sizeInBytes,
                 D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);

    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    ThrowIfFailed(m_resource->Map(0, &readRange, &mapped));
    std::memcpy(mapped, data, sizeInBytes);
    m_resource->Unmap(0, nullptr);

    m_view.BufferLocation = m_resource->GetGPUVirtualAddress();
    m_view.SizeInBytes    = sizeInBytes;
    m_view.StrideInBytes  = strideInBytes;
}

void VertexBuffer::FinishUpload()
{
    m_uploadBuffer.Reset();
}

// ===========================================================================
// IndexBuffer
// ===========================================================================
void IndexBuffer::Initialize(GraphicsDevice& device, const u32* indices, u32 indexCount)
{
    DX_ASSERT(indices, "Index data must not be null");
    DX_ASSERT(indexCount > 0, "Index count must be > 0");

    u32 sizeInBytes = indexCount * sizeof(u32);
    m_indexCount    = indexCount;

    // UPLOAD ヒープに直接作成（VertexBuffer と同じく即時・GPUストールなし）
    CreateBuffer(device.GetAllocator(), sizeInBytes,
                 D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);

    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    ThrowIfFailed(m_resource->Map(0, &readRange, &mapped));
    std::memcpy(mapped, indices, sizeInBytes);
    m_resource->Unmap(0, nullptr);

    m_view.BufferLocation = m_resource->GetGPUVirtualAddress();
    m_view.SizeInBytes    = sizeInBytes;
    m_view.Format         = DXGI_FORMAT_R32_UINT;
}

void IndexBuffer::FinishUpload()
{
    m_uploadBuffer.Reset();
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
