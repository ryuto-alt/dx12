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
    m_allocator.Initialize(numDescriptors);
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

// 枯渇はデバッグ/リリース問わず例外で fail-fast させる。
// kInvalidIndex(0xFFFFFFFF) を黙って返すと、呼び出し側の GetCpuHandle で
// オーバーフローしたポインタが CreateShaderResourceView に渡り、
// Release ではヒープ破壊/GPUクラッシュとして離れた場所で発症する。
D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::Allocate()
{
    u32 index = m_allocator.Allocate();
    if (index == kInvalidIndex)
    {
        Logger::Error("DescriptorHeap が満杯です（type: {}, capacity: {}）",
            static_cast<int>(m_type), m_numDescriptors);
        throw std::runtime_error("DescriptorHeap is full (capacity " +
            std::to_string(m_numDescriptors) + ")");
    }
    return GetCpuHandle(index);
}

u32 DescriptorHeap::AllocateIndex()
{
    u32 index = m_allocator.Allocate();
    if (index == kInvalidIndex)
    {
        Logger::Error("DescriptorHeap が満杯です（type: {}, capacity: {}）",
            static_cast<int>(m_type), m_numDescriptors);
        throw std::runtime_error("DescriptorHeap is full (capacity " +
            std::to_string(m_numDescriptors) + ")");
    }
    return index;
}

u32 DescriptorHeap::AllocateBlock(u32 count)
{
    u32 start = m_allocator.AllocateBlock(count);
    if (start == kInvalidIndex)
    {
        Logger::Error("DescriptorHeap に {} 個の連続空きがありません（type: {}, free: {}/{}）",
            count, static_cast<int>(m_type), m_allocator.FreeCount(), m_numDescriptors);
        throw std::runtime_error("DescriptorHeap block allocation failed (need " +
            std::to_string(count) + ", free " + std::to_string(m_allocator.FreeCount()) +
            "/" + std::to_string(m_numDescriptors) + ")");
    }
    return start;
}

void DescriptorHeap::Free(u32 index)
{
    if (index == kInvalidIndex)
    {
        return;
    }
    DX_ASSERT(index < m_numDescriptors, "DescriptorHeap::Free index out of range");
    m_allocator.Free(index);
}

void DescriptorHeap::FreeBlock(u32 start, u32 count)
{
    if (start == kInvalidIndex || count == 0)
    {
        return;
    }
    DX_ASSERT(start + count <= m_numDescriptors, "DescriptorHeap::FreeBlock range out of bounds");
    m_allocator.FreeBlock(start, count);
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
