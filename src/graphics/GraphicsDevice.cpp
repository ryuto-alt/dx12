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
        std::string adapterName(static_cast<size_t>(nameSize), '\0');
        WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, adapterName.data(), nameSize, nullptr, nullptr);
        adapterName.pop_back();
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

    QueryCapabilities();
}

// DXR ケーパビリティの一括問い合わせ（計画09 Step 0）。
// 「この GPU で何ができるか」をここ 1 箇所に集約し、以後の全パスが
// SupportsInlineRaytracing() だけを見て安全に分岐できるようにする。
void GraphicsDevice::QueryCapabilities()
{
    // --- ① レイトレーシング Tier の実値 ---
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        HRESULT hr = m_device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
        if (SUCCEEDED(hr))
            m_raytracingTier = options5.RaytracingTier;
        m_dxrSupported = (m_raytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED);
    }

    // --- ② 最高シェーダモデル ---
    // CheckFeatureSupport は「ランタイムが知らない SM 値」を渡すと E_INVALIDARG を返す。
    // 公式ドキュメントの推奨どおり、高い方から 1 段ずつ下げて再試行する
    // （成功時は「渡した値以下で、かつデバイスが対応する最高値」が書き戻される）。
    {
        static constexpr D3D_SHADER_MODEL kProbe[] = {
            D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7,
            D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4,
            D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1,
            D3D_SHADER_MODEL_6_0,
        };
        for (D3D_SHADER_MODEL sm : kProbe)
        {
            D3D12_FEATURE_DATA_SHADER_MODEL data{};
            data.HighestShaderModel = sm;
            if (SUCCEEDED(m_device->CheckFeatureSupport(
                    D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data))))
            {
                m_highestShaderModel = data.HighestShaderModel;
                break;
            }
        }
    }

    // --- ③ DXR 1.2（SER）。今は使わないがログに出しておく（計画09 §3.3 / R18）---
    if (m_raytracingTier >= D3D12_RAYTRACING_TIER_1_2)
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS22 options22{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS22, &options22, sizeof(options22))))
        {
            m_serActuallyReorders = (options22.ShaderExecutionReorderingActuallyReorders != FALSE);
        }
    }

    // --- ④ 起動ログ 1 行（dx12_get_log / DeepDiagnostics `dxr` の一次情報）---
    const int tierMajor = (m_raytracingTier >= D3D12_RAYTRACING_TIER_1_0)
                        ? static_cast<int>(m_raytracingTier) / 10 : 0;
    const int tierMinor = (m_raytracingTier >= D3D12_RAYTRACING_TIER_1_0)
                        ? static_cast<int>(m_raytracingTier) % 10 : 0;
    const int smMajor = static_cast<int>(m_highestShaderModel) >> 4;
    const int smMinor = static_cast<int>(m_highestShaderModel) & 0xF;

    if (m_dxrSupported)
    {
        Logger::Info("DXR: Tier {}.{} / ShaderModel {}.{} / InlineRT={} / Bindless(SM6.6)={} / DXR1.2={} / SER={}",
            tierMajor, tierMinor, smMajor, smMinor,
            SupportsInlineRaytracing() ? "yes" : "no",
            SupportsBindlessSm66()     ? "yes" : "no",
            SupportsDxr12()            ? "yes" : "no",
            m_serActuallyReorders      ? "yes" : "no");
    }
    else
    {
        Logger::Info("DXR: Tier none / ShaderModel {}.{} / InlineRT=no", smMajor, smMinor);
    }

    if (!SupportsInlineRaytracing())
    {
        // ★Error にしない。エラーではなく「機能の不在」（計画09 §5.3）。
        Logger::Warn("この GPU では inline raytracing (RayQuery) が使えません "
                     "（要 DXR Tier 1.1 かつ Shader Model 6.5。RTX 20 系 / RX 6000 系以降）。"
                     "レイトレーシング機能は無効のまま従来の描画で動作します");
    }
}

} // namespace dx12e
