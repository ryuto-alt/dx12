#include "GraphicsDevice.h"

#include "core/Assert.h"
#include "core/Logger.h"
#include "core/Window.h"

using Microsoft::WRL::ComPtr;

namespace dx12e
{

GraphicsDevice::~GraphicsDevice()
{
    if (m_allocator)
    {
        m_allocator->Release();
        m_allocator = nullptr;
    }
}

void GraphicsDevice::Initialize(Window& /*window*/)
{
    // --- Debug Layer ---
#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug3> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            debugController->SetEnableGPUBasedValidation(TRUE);
            Logger::Info("D3D12 Debug Layer enabled with GPU-based validation");
        }
    }
#endif

    // --- DXGI Factory ---
    {
        UINT factoryFlags = 0;
#ifdef _DEBUG
        factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
        ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)));
        Logger::Info("DXGI Factory created");
    }

    // --- Adapter Selection ---
    {
        HRESULT hr = m_factory->EnumAdapterByGpuPreference(
            0,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&m_adapter));

        if (FAILED(hr))
        {
            // Fallback: enumerate adapters manually
            ThrowIfFailed(m_factory->EnumAdapters1(0, &m_adapter));
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        m_adapter->GetDesc1(&adapterDesc);
        // wchar_t → char 安全変換
        int nameSize = WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, nullptr, 0, nullptr, nullptr);
        std::string adapterName(static_cast<size_t>(nameSize - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, adapterName.data(), nameSize, nullptr, nullptr);
        Logger::Info("GPU selected: {}", adapterName);
    }

    // --- Device Creation ---
    {
        ThrowIfFailed(D3D12CreateDevice(
            m_adapter.Get(),
            D3D_FEATURE_LEVEL_12_1,
            IID_PPV_ARGS(&m_device)));
        Logger::Info("D3D12 Device created (Feature Level 12_1)");
    }

    // --- Info Queue (Debug) ---
#ifdef _DEBUG
    {
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(m_device.As(&infoQueue)))
        {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            Logger::Info("D3D12 InfoQueue break-on-severity configured");
        }
    }
#endif

    // --- D3D12 Memory Allocator ---
    {
        D3D12MA::ALLOCATOR_DESC allocatorDesc{};
        allocatorDesc.pDevice  = m_device.Get();
        allocatorDesc.pAdapter = m_adapter.Get();
        allocatorDesc.Flags    = D3D12MA::ALLOCATOR_FLAG_NONE;

        ThrowIfFailed(D3D12MA::CreateAllocator(&allocatorDesc, &m_allocator));
        Logger::Info("D3D12 Memory Allocator initialized");
    }

    // --- DXR Support Check ---
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        HRESULT hr = m_device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS5,
            &options5,
            sizeof(options5));

        if (SUCCEEDED(hr) && options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        {
            m_dxrSupported = true;
            Logger::Info("DXR supported (Tier {})",
                static_cast<int>(options5.RaytracingTier));
        }
        else
        {
            m_dxrSupported = false;
            Logger::Warn("DXR not supported on this device");
        }
    }
}

} // namespace dx12e
