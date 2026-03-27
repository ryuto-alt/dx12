#include "CommandQueue.h"

#include "GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"

using Microsoft::WRL::ComPtr;

namespace dx12e
{

CommandQueue::~CommandQueue()
{
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

void CommandQueue::Initialize(GraphicsDevice& device, D3D12_COMMAND_LIST_TYPE type)
{
    m_type = type;

    // --- Command Queue ---
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type     = type;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    ThrowIfFailed(device.GetDevice()->CreateCommandQueue(
        &queueDesc, IID_PPV_ARGS(&m_queue)));

    // --- Fence ---
    ThrowIfFailed(device.GetDevice()->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));

    // --- Fence Event ---
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    DX_ASSERT(m_fenceEvent != nullptr, "Failed to create fence event");

    Logger::Info("CommandQueue initialized (type: {})", static_cast<int>(type));
}

void CommandQueue::ExecuteCommandList(ID3D12GraphicsCommandList* cmdList)
{
    ID3D12CommandList* lists[] = { cmdList };
    m_queue->ExecuteCommandLists(1, lists);
}

u64 CommandQueue::Signal()
{
    ++m_fenceValue;
    ThrowIfFailed(m_queue->Signal(m_fence.Get(), m_fenceValue));
    return m_fenceValue;
}

void CommandQueue::WaitForValue(u64 value)
{
    if (m_fence->GetCompletedValue() < value)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(value, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void CommandQueue::WaitIdle()
{
    u64 value = Signal();
    WaitForValue(value);
}

} // namespace dx12e
