#include "animation/SkinningBuffer.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "core/Assert.h"

#include <algorithm>
#include <cstring>

namespace dx12e
{

void SkinningBuffer::Initialize(GraphicsDevice& device, DescriptorHeap& srvHeap,
                                u32 maxBones, u32 frameCount)
{
    m_maxBones   = maxBones;
    m_bufferSize = maxBones * static_cast<u32>(sizeof(DirectX::XMFLOAT4X4)); // maxBones * 64

    m_frames.resize(frameCount);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type                  = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty       = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference  = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask      = 1;
    heapProps.VisibleNodeMask       = 1;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Alignment          = 0;
    bufDesc.Width              = m_bufferSize;
    bufDesc.Height             = 1;
    bufDesc.DepthOrArraySize   = 1;
    bufDesc.MipLevels          = 1;
    bufDesc.Format             = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count   = 1;
    bufDesc.SampleDesc.Quality = 0;
    bufDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Device5* d3dDevice = device.GetDevice();

    for (u32 i = 0; i < frameCount; ++i)
    {
        PerFrame& frame = m_frames[i];

        ThrowIfFailed(d3dDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&frame.resource)));

        // 永続Map
        void* mapped = nullptr;
        ThrowIfFailed(frame.resource->Map(0, nullptr, &mapped));
        frame.mappedPtr = static_cast<u8*>(mapped);

        // SRV 作成
        frame.srvIndex = srvHeap.AllocateIndex();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Format                     = DXGI_FORMAT_UNKNOWN;
        srvDesc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement        = 0;
        srvDesc.Buffer.NumElements         = maxBones;
        srvDesc.Buffer.StructureByteStride = static_cast<UINT>(sizeof(DirectX::XMFLOAT4X4)); // 64
        srvDesc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

        d3dDevice->CreateShaderResourceView(
            frame.resource.Get(),
            &srvDesc,
            srvHeap.GetCpuHandle(frame.srvIndex));
    }
}

void SkinningBuffer::Update(const std::vector<DirectX::XMFLOAT4X4>& matrices,
                            u32 frameIndex)
{
    DX_ASSERT(frameIndex < static_cast<u32>(m_frames.size()), "frameIndex out of range");

    u32 copyCount = (std::min)(static_cast<u32>(matrices.size()), m_maxBones);
    u32 copySize  = copyCount * static_cast<u32>(sizeof(DirectX::XMFLOAT4X4));

    std::memcpy(m_frames[frameIndex].mappedPtr, matrices.data(), copySize);
}

} // namespace dx12e
