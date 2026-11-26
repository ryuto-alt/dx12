#include "DescriptorHeap.h"

#include "GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"
#include "DeferredRelease.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace dx12e
{

// 遅延返却のコールバックはヒープより長生きし得る(FlushAll がヒープ破棄の後になる、
// 同じアドレスに別のヒープが再構築される、Initialize し直される等)。
// コールバックはポインタではなく世代 id を捕まえ、mutex の下で表を引く。
// 破棄・再初期化で id を表から消すので、古いコールバックは何もしない。
namespace
{
std::mutex& RegistryMutex()
{
    static std::mutex m;
    return m;
}
std::unordered_map<u64, DescriptorHeap*>& Registry()
{
    static std::unordered_map<u64, DescriptorHeap*> r;
    return r;
}
u64 NextHeapId()
{
    static std::atomic<u64> next{1};
    return next.fetch_add(1);
}
} // namespace

// 呼び出し側が m_heapId を更新する(登録解除 + 新規登録)。
static u64 ReRegister(u64 oldId, DescriptorHeap* heap)
{
    std::lock_guard<std::mutex> lock(RegistryMutex());
    if (oldId != 0)
        Registry().erase(oldId);
    u64 id = NextHeapId();
    Registry()[id] = heap;
    return id;
}

DescriptorHeap::~DescriptorHeap()
{
    if (m_heapId != 0)
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        Registry().erase(m_heapId);
    }
}

void DescriptorHeap::InitializeBookkeepingOnly(u32 numDescriptors, bool shaderVisible)
{
    m_numDescriptors = numDescriptors;
    m_descriptorSize = 1;
    m_allocator.Initialize(numDescriptors);
    m_shaderVisible  = shaderVisible;
    m_heapId         = ReRegister(m_heapId, this);
}

void DescriptorHeap::Initialize(GraphicsDevice& device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                u32 numDescriptors, bool shaderVisible)
{
    m_type           = type;
    m_numDescriptors = numDescriptors;
    m_allocator.Initialize(numDescriptors);
    m_shaderVisible  = shaderVisible;
    m_heapId         = ReRegister(m_heapId, this);

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
    // 未登録、またはシェーダから見えないヒープは遅延不要。
    if (m_heapId == 0 || !m_shaderVisible)
    {
        FreeNow(index);
        return;
    }
    const u64 id = m_heapId;
    DeferredRelease::Defer(
        [](u64 hid, u32 idx, u32)
        {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            auto it = Registry().find(hid);
            if (it != Registry().end())
                it->second->FreeNow(idx);
        },
        id, index, 0);
}

void DescriptorHeap::FreeBlock(u32 start, u32 count)
{
    if (start == kInvalidIndex || count == 0)
    {
        return;
    }
    DX_ASSERT(start + count <= m_numDescriptors, "DescriptorHeap::FreeBlock range out of bounds");
    if (m_heapId == 0 || !m_shaderVisible)
    {
        FreeBlockNow(start, count);
        return;
    }
    const u64 id = m_heapId;
    DeferredRelease::Defer(
        [](u64 hid, u32 st, u32 cnt)
        {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            auto it = Registry().find(hid);
            if (it != Registry().end())
                it->second->FreeBlockNow(st, cnt);
        },
        id, start, count);
}

void DescriptorHeap::FreeNow(u32 index)
{
    m_allocator.Free(index);
}

void DescriptorHeap::FreeBlockNow(u32 start, u32 count)
{
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
