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
    // ★Debug ビルド限定にしない。このリポジトリでは Debug ビルドが実際には作られておらず
    //   （build/debug が存在しない）、デバッグレイヤの指摘が「誰にも見えない」状態が続いていた。
    //   環境変数で release からも入れられるようにする:
    //     DX12_D3D_DEBUG=1  … デバッグレイヤ + メッセージをエンジンログへ流す
    //     DX12_D3D_GBV=1    … 加えて GPU-based validation（重い。単体では入れない）
    const auto envOn = [](const char* name) {
        size_t len = 0; char buf[8]{};
        return getenv_s(&len, buf, sizeof(buf), name) == 0 && len > 0 && buf[0] == '1';
    };
#ifdef _DEBUG
    const bool wantDebugLayer = true;
    const bool wantGbv        = !envOn("DX12_D3D_NO_GBV");
#else
    const bool wantDebugLayer = envOn("DX12_D3D_DEBUG") || envOn("DX12_D3D_GBV");
    const bool wantGbv        = envOn("DX12_D3D_GBV");
#endif
    if (wantDebugLayer)
    {
        ComPtr<ID3D12Debug3> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            debugController->SetEnableGPUBasedValidation(wantGbv ? TRUE : FALSE);
            Logger::Info("D3D12 Debug Layer enabled (GBV={})", wantGbv ? "on" : "off");
        }
    }

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
    // ★指摘を dx12_engine.log へ流す。従来は OutputDebugString へ出るだけで、
    //   デバッガを繋がない限り誰も読めなかった（＝実在するエラーが何ヶ月も放置されていた）。
    //   ログに出れば dx12_get_log からも読める＝AI もエンジンの不正を検出できる。
    if (wantDebugLayer)
    {
        ComPtr<ID3D12InfoQueue1> iq1;
        if (SUCCEEDED(m_device.As(&iq1)))
        {
            DWORD cookie = 0;
            iq1->RegisterMessageCallback(
                [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id,
                   LPCSTR desc, void*)
                {
                    // CORRUPTION/ERROR は error、WARNING は warn。INFO 以下は捨てる（大量に出る）。
                    if (sev <= D3D12_MESSAGE_SEVERITY_ERROR)
                        Logger::Error("[D3D12 id={}] {}", static_cast<int>(id), desc ? desc : "");
                    else if (sev == D3D12_MESSAGE_SEVERITY_WARNING)
                        Logger::Warn("[D3D12 id={}] {}", static_cast<int>(id), desc ? desc : "");
                },
                D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
            Logger::Info("D3D12 InfoQueue: メッセージをエンジンログへ流します");
        }

        // ★break-on-severity は既定で切る。起動時に実在のエラーが出ている状態で
        //   これを立てるとデバッガ無しでは即死するだけで、原因が何も分からない
        //   （実際 2026-07-30 に exit 2170 で落ちて、ログへ流して初めて中身が読めた）。
        //   デバッガで止めたいときだけ環境変数で。
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (envOn("DX12_D3D_BREAK") && SUCCEEDED(m_device.As(&infoQueue)))
        {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            Logger::Info("D3D12 InfoQueue: ERROR で break します");
        }
    }

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

    // --- ①' DXR 非対応 GPU のシミュレーション（環境変数 DX12_DISABLE_DXR=1）---
    // ★フォールバック経路（計画09 §5.3）を対応 GPU 上で実際に踏むための逃げ道。
    //   Tier を丸ごと NOT_SUPPORTED に落とすので、ログ・エディタ UI・診断・
    //   レンダリングの全経路が「本当に非対応な GPU」とまったく同じ挙動になる。
    {
        char buf[8]{};
        size_t len = 0;
        if (::getenv_s(&len, buf, sizeof(buf), "DX12_DISABLE_DXR") == 0 && len > 0 && buf[0] == '1')
        {
            m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
            m_dxrSupported   = false;
            Logger::Warn("DX12_DISABLE_DXR=1 のため DXR を非対応として扱います（フォールバック検証用）");
        }
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

    // --- ③.5 リソースバインディング Tier（SM 6.6 Dynamic Resources の必要条件）---
    // 仕様が要求するのは「SM 6.6」かつ「Tier 3」の 2 つだけ。FL 12_2 の GPU は Tier 3 が必須。
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))))
            m_resourceBindingTier = opts.ResourceBindingTier;
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
        Logger::Info("DXR: Tier {}.{} / ShaderModel {}.{} / InlineRT={} / DynamicResources(SM6.6+Tier3)={}"
                     " (bindingTier={}) / DXR1.2={} / SER={}",
            tierMajor, tierMinor, smMajor, smMinor,
            SupportsInlineRaytracing() ? "yes" : "no",
            SupportsDynamicResources() ? "yes" : "no",
            static_cast<int>(m_resourceBindingTier),
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
