#include "graphics/Texture.h"
#include "graphics/GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <cstring>

namespace dx12e
{

void Texture::Initialize(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const D3D12_RESOURCE_DESC& desc,
    const D3D12_SUBRESOURCE_DATA* subresources,
    u32 subresourceCount)
{
    DX_ASSERT(cmdList, "CommandList must not be null");
    DX_ASSERT(subresources, "Subresource data must not be null");
    DX_ASSERT(subresourceCount > 0, "SubresourceCount must be > 0");

    m_width  = static_cast<u32>(desc.Width);
    m_height = desc.Height;
    m_format = desc.Format;

    // 1. D3D12MA で DEFAULT ヒープにリソース確保 (COPY_DEST state)
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    CreateResource(
        device.GetAllocator(),
        desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        allocDesc);

    // 2. フットプリント取得でアップロードバッファサイズ計算
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;

    device.GetDevice()->GetCopyableFootprints(
        &desc, 0, subresourceCount, 0,
        &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    // 3. UPLOAD ヒープにアップロードバッファ作成（D3D12MAは使わず直接作成）
    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Alignment        = 0;
    uploadDesc.Width            = totalBytes;
    uploadDesc.Height           = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels        = 1;
    uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc       = {1, 0};
    uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES uploadHeapProps{};
    uploadHeapProps.Type                 = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeapProps.CreationNodeMask     = 1;
    uploadHeapProps.VisibleNodeMask      = 1;

    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_uploadBuffer)));

    // 4. Upload buffer に Map → row-by-row コピー → Unmap
    void* mapped = nullptr;
    ThrowIfFailed(m_uploadBuffer->Map(0, nullptr, &mapped));

    auto* dstBase = static_cast<u8*>(mapped) + footprint.Offset;
    const auto& srcData = subresources[0];

    for (UINT row = 0; row < numRows; ++row)
    {
        auto* dstRow = dstBase + static_cast<size_t>(row) * footprint.Footprint.RowPitch;
        auto* srcRow = static_cast<const u8*>(srcData.pData)
                       + static_cast<size_t>(row) * srcData.RowPitch;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(rowSizeInBytes));
    }

    m_uploadBuffer->Unmap(0, nullptr);

    // 5. CopyTextureRegion でテクスチャへコピー
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource        = m_resource.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource       = m_uploadBuffer.Get();
    src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // 6. バリア遷移: COPY_DEST → SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    cmdList->ResourceBarrier(1, &barrier);
    m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    Logger::Info("Texture created ({}x{}, format={})", m_width, m_height, static_cast<u32>(m_format));
}

void Texture::CreateSRV(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle)
{
    DX_ASSERT(m_resource.Get(), "Resource must be initialized before creating SRV");

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                        = m_format;
    srvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip     = 0;
    srvDesc.Texture2D.MipLevels           = 1;
    srvDesc.Texture2D.PlaneSlice          = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    device.GetDevice()->CreateShaderResourceView(m_resource.Get(), &srvDesc, cpuHandle);
}

void Texture::FinishUpload()
{
    m_uploadBuffer.Reset();
}

} // namespace dx12e
