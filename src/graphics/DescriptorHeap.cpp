#include "DescriptorHeap.h"

#include "GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{

void DescriptorHeap::Initialize(GraphicsDevice& device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                u32 numDescriptors, bool shaderVisible)
{
    m_type           = type;
    m_numDescriptors = numDescriptors;
    m_numAllocated   = 0;
    m_shaderVisible  = shaderVisible;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = type;
    heapDesc.NumDescriptors = numDescriptors;
    heapDesc.Flags          = shaderVisible
        ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
        : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heapDesc.NodeMask = 0;

    ThrowIfFailed(device.GetDevice()->CreateDescriptorHeap(
        &heapDesc, IID_PPV_ARGS(&m_heap)));

    m_descriptorSize = device.GetDevice()->GetDescriptorHandleIncrementSize(type);
    m_cpuStart       = m_heap->GetCPUDescriptorHandleForHeapStart();

    if (shaderVisible)
    {
        m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
    }

    Logger::Info("DescriptorHeap initialized (type: {}, count: {}, shaderVisible: {})",
        static_cast<int>(type), numDescriptors, shaderVisible);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::Allocate()
{
    DX_ASSERT(m_numAllocated < m_numDescriptors, "DescriptorHeap is full");

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_cpuStart;
    handle.ptr += static_cast<SIZE_T>(m_descriptorSize) * m_numAllocated;
    ++m_numAllocated;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::GetCpuHandle(u32 index) const
{
    DX_ASSERT(index < m_numDescriptors, "DescriptorHeap CPU handle index out of range");

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_cpuStart;
    handle.ptr += static_cast<SIZE_T>(m_descriptorSize) * index;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GetGpuHandle(u32 index) const
{
    DX_ASSERT(m_shaderVisible, "DescriptorHeap is not shader visible");
    DX_ASSERT(index < m_numDescriptors, "DescriptorHeap GPU handle index out of range");

    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_gpuStart;
    handle.ptr += static_cast<UINT64>(m_descriptorSize) * index;
    return handle;
}

} // namespace dx12e
