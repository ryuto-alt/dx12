#pragma once

#include <Windows.h>

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <D3D12MemAlloc.h>

#include "core/Types.h"

namespace dx12e
{

class Window;

class GraphicsDevice
{
public:
    GraphicsDevice() = default;
    ~GraphicsDevice();

    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    void Initialize(Window& window);

    ID3D12Device5*      GetDevice()      const { return m_device.Get(); }
    IDXGIFactory6*      GetFactory()     const { return m_factory.Get(); }
    D3D12MA::Allocator* GetAllocator()   const { return m_allocator; }
    bool                IsDxrSupported() const { return m_dxrSupported; }

private:
    Microsoft::WRL::ComPtr<ID3D12Device5>  m_device;
    Microsoft::WRL::ComPtr<IDXGIFactory6>  m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>  m_adapter;
    D3D12MA::Allocator*                    m_allocator = nullptr;
    bool                                   m_dxrSupported = false;
};

} // namespace dx12e
