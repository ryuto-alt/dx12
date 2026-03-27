#pragma once

#include <Windows.h>

#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;

class DescriptorHeap
{
public:
    DescriptorHeap() = default;
    ~DescriptorHeap() = default;

    DescriptorHeap(const DescriptorHeap&) = delete;
    DescriptorHeap& operator=(const DescriptorHeap&) = delete;

    void Initialize(GraphicsDevice& device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                    u32 numDescriptors, bool shaderVisible = false);

    D3D12_CPU_DESCRIPTOR_HANDLE Allocate();

    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(u32 index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(u32 index) const;

    ID3D12DescriptorHeap* GetHeap()           const { return m_heap.Get(); }
    u32                   GetDescriptorSize() const { return m_descriptorSize; }

private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    u32                          m_descriptorSize   = 0;
    u32                          m_numDescriptors   = 0;
    u32                          m_numAllocated     = 0;
    D3D12_DESCRIPTOR_HEAP_TYPE   m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    D3D12_CPU_DESCRIPTOR_HANDLE  m_cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE  m_gpuStart{};
    bool                         m_shaderVisible = false;
};

} // namespace dx12e
