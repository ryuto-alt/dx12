#pragma once

#include <Windows.h>

#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;

class CommandQueue
{
public:
    CommandQueue() = default;
    ~CommandQueue();

    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    void Initialize(GraphicsDevice& device, D3D12_COMMAND_LIST_TYPE type);

    void ExecuteCommandList(ID3D12GraphicsCommandList* cmdList);
    u64  Signal();
    void WaitForValue(u64 value);
    void WaitIdle();

    ID3D12CommandQueue*    GetQueue() const { return m_queue.Get(); }
    D3D12_COMMAND_LIST_TYPE GetType()  const { return m_type; }

private:
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence>        m_fence;
    u64                                        m_fenceValue = 0;
    HANDLE                                     m_fenceEvent = nullptr;
    D3D12_COMMAND_LIST_TYPE                    m_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
};

} // namespace dx12e
