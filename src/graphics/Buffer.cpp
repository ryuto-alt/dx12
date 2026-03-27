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

    auto* d3dDevice  = device.GetDevice();
    auto* allocator  = device.GetAllocator();

    // DEFAULT ヒープにバッファ作成
    CreateBuffer(allocator, sizeInBytes, D3D12_RESOURCE_STATE_COMMON, D3D12_HEAP_TYPE_DEFAULT);

    // UPLOAD ヒープに一時バッファ作成
    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Alignment          = 0;
    uploadDesc.Width              = sizeInBytes;
    uploadDesc.Height             = 1;
    uploadDesc.DepthOrArraySize   = 1;
    uploadDesc.MipLevels          = 1;
    uploadDesc.Format             = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count   = 1;
    uploadDesc.SampleDesc.Quality = 0;
    uploadDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES uploadHeapProps{};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    ThrowIfFailed(d3dDevice->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_uploadBuffer)));

    // Upload バッファにデータをコピー
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    ThrowIfFailed(m_uploadBuffer->Map(0, &readRange, &mapped));
    std::memcpy(mapped, data, sizeInBytes);
    m_uploadBuffer->Unmap(0, nullptr);

    // 一時的な CommandQueue + Allocator + List を作成してコピー
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>     tempQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>  tempAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> tempCmdList;
    Microsoft::WRL::ComPtr<ID3D12Fence>             tempFence;

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&tempQueue)));
    ThrowIfFailed(d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tempAllocator)));
    ThrowIfFailed(d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tempAllocator.Get(), nullptr, IID_PPV_ARGS(&tempCmdList)));

    tempCmdList->CopyBufferRegion(m_resource.Get(), 0, m_uploadBuffer.Get(), 0, sizeInBytes);
    ThrowIfFailed(tempCmdList->Close());

    ID3D12CommandList* lists[] = { tempCmdList.Get() };
    tempQueue->ExecuteCommandLists(1, lists);

    // フェンスで完了待ち
    ThrowIfFailed(d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tempFence)));
    ThrowIfFailed(tempQueue->Signal(tempFence.Get(), 1));

    if (tempFence->GetCompletedValue() < 1)
    {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DX_ASSERT(event != nullptr, "Failed to create fence event");
        ThrowIfFailed(tempFence->SetEventOnCompletion(1, event));
        WaitForSingleObject(event, INFINITE);
        CloseHandle(event);
    }

    m_currentState = D3D12_RESOURCE_STATE_COMMON;

    // VBV 設定
    m_view.BufferLocation = m_resource->GetGPUVirtualAddress();
    m_view.SizeInBytes    = sizeInBytes;
    m_view.StrideInBytes  = strideInBytes;

    Logger::Info("VertexBuffer created: {} bytes, stride {}", sizeInBytes, strideInBytes);
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

    auto* d3dDevice = device.GetDevice();
    auto* allocator = device.GetAllocator();

    u32 sizeInBytes = indexCount * sizeof(u32);
    m_indexCount    = indexCount;

    // DEFAULT ヒープにバッファ作成
    CreateBuffer(allocator, sizeInBytes, D3D12_RESOURCE_STATE_COMMON, D3D12_HEAP_TYPE_DEFAULT);

    // UPLOAD ヒープに一時バッファ作成
    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Alignment          = 0;
    uploadDesc.Width              = sizeInBytes;
    uploadDesc.Height             = 1;
    uploadDesc.DepthOrArraySize   = 1;
    uploadDesc.MipLevels          = 1;
    uploadDesc.Format             = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count   = 1;
    uploadDesc.SampleDesc.Quality = 0;
    uploadDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES uploadHeapProps{};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    ThrowIfFailed(d3dDevice->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_uploadBuffer)));

    // Upload バッファにデータをコピー
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    ThrowIfFailed(m_uploadBuffer->Map(0, &readRange, &mapped));
    std::memcpy(mapped, indices, sizeInBytes);
    m_uploadBuffer->Unmap(0, nullptr);

    // 一時的な CommandQueue + Allocator + List を作成してコピー
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        tempQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>     tempAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>  tempCmdList;
    Microsoft::WRL::ComPtr<ID3D12Fence>                tempFence;

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&tempQueue)));
    ThrowIfFailed(d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tempAllocator)));
    ThrowIfFailed(d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tempAllocator.Get(), nullptr, IID_PPV_ARGS(&tempCmdList)));

    tempCmdList->CopyBufferRegion(m_resource.Get(), 0, m_uploadBuffer.Get(), 0, sizeInBytes);
    ThrowIfFailed(tempCmdList->Close());

    ID3D12CommandList* lists[] = { tempCmdList.Get() };
    tempQueue->ExecuteCommandLists(1, lists);

    // フェンスで完了待ち
    ThrowIfFailed(d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tempFence)));
    ThrowIfFailed(tempQueue->Signal(tempFence.Get(), 1));

    if (tempFence->GetCompletedValue() < 1)
    {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DX_ASSERT(event != nullptr, "Failed to create fence event");
        ThrowIfFailed(tempFence->SetEventOnCompletion(1, event));
        WaitForSingleObject(event, INFINITE);
        CloseHandle(event);
    }

    m_currentState = D3D12_RESOURCE_STATE_COMMON;

    // IBV 設定
    m_view.BufferLocation = m_resource->GetGPUVirtualAddress();
    m_view.SizeInBytes    = sizeInBytes;
    m_view.Format         = DXGI_FORMAT_R32_UINT;

    Logger::Info("IndexBuffer created: {} indices ({} bytes)", indexCount, sizeInBytes);
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
