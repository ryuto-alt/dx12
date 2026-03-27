#include "SwapChain.h"

#include "GraphicsDevice.h"
#include "CommandQueue.h"
#include "DescriptorHeap.h"
#include "core/Assert.h"
#include "core/Logger.h"
#include "core/Window.h"

using Microsoft::WRL::ComPtr;

namespace dx12e
{

void SwapChain::Initialize(Window& window, GraphicsDevice& device,
                           CommandQueue& directQueue, DescriptorHeap& rtvHeap)
{
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width       = window.GetWidth();
    swapChainDesc.Height      = window.GetHeight();
    swapChainDesc.Format      = m_format;
    swapChainDesc.Stereo      = FALSE;
    swapChainDesc.SampleDesc  = { 1, 0 };
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.Scaling     = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags       = 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(device.GetFactory()->CreateSwapChainForHwnd(
        directQueue.GetQueue(),
        window.GetHwnd(),
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain1));

    // ALT+ENTER によるフルスクリーン切り替えを無効化
    ThrowIfFailed(device.GetFactory()->MakeWindowAssociation(
        window.GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

    ThrowIfFailed(swapChain1.As(&m_swapChain));

    m_currentFrameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // バックバッファ取得 + RTV作成
    CreateRenderTargetViews(device.GetDevice(), rtvHeap);

    Logger::Info("SwapChain initialized ({}x{}, {} buffers, FLIP_DISCARD)",
        window.GetWidth(), window.GetHeight(), kFrameCount);
}

void SwapChain::Present(bool vsync)
{
    ThrowIfFailed(m_swapChain->Present(vsync ? 1 : 0, 0));
    m_currentFrameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void SwapChain::Resize(u32 width, u32 height, DescriptorHeap& rtvHeap)
{
    // バックバッファの参照を解放
    for (auto& buffer : m_backBuffers)
    {
        buffer.Reset();
    }

    ThrowIfFailed(m_swapChain->ResizeBuffers(
        kFrameCount,
        width,
        height,
        m_format,
        0));

    m_currentFrameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // ID3D12Deviceを取得してRTV再作成
    ComPtr<ID3D12Device> device;
    ThrowIfFailed(m_swapChain->GetDevice(IID_PPV_ARGS(&device)));
    CreateRenderTargetViews(device.Get(), rtvHeap);

    Logger::Info("SwapChain resized ({}x{})", width, height);
}

void SwapChain::CreateRenderTargetViews(ID3D12Device* device, DescriptorHeap& rtvHeap)
{
    for (u32 i = 0; i < kFrameCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));

        m_rtvHandles[i] = rtvHeap.Allocate();
        device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, m_rtvHandles[i]);
    }
}

} // namespace dx12e
