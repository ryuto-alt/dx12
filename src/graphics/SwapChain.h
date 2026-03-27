#pragma once

#include <Windows.h>

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>

#include "core/Types.h"

namespace dx12e
{

class Window;
class GraphicsDevice;
class CommandQueue;
class DescriptorHeap;

class SwapChain
{
public:
    static constexpr u32 kFrameCount = 3;

    SwapChain() = default;
    ~SwapChain() = default;

    SwapChain(const SwapChain&) = delete;
    SwapChain& operator=(const SwapChain&) = delete;

    void Initialize(Window& window, GraphicsDevice& device,
                    CommandQueue& directQueue, DescriptorHeap& rtvHeap);

    void Present(bool vsync);
    void Resize(u32 width, u32 height, DescriptorHeap& rtvHeap);

    bool IsTearingSupported() const { return m_tearingSupported; }

    ID3D12Resource*             GetCurrentBackBuffer()      const { return m_backBuffers[m_currentFrameIndex].Get(); }
    u32                         GetCurrentBackBufferIndex()  const { return m_currentFrameIndex; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV()             const { return m_rtvHandles[m_currentFrameIndex]; }
    DXGI_FORMAT                 GetFormat()                 const { return m_format; }

private:
    void CreateRenderTargetViews(ID3D12Device* device, DescriptorHeap& rtvHeap);

    Microsoft::WRL::ComPtr<IDXGISwapChain4>                              m_swapChain;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount>      m_backBuffers;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kFrameCount>                 m_rtvHandles{};
    u32                                                                  m_currentFrameIndex = 0;
    DXGI_FORMAT                                                          m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool                                                                 m_tearingSupported = false;
};

} // namespace dx12e
