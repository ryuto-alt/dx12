#include "renderer/IBLBaker.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <cstring>

namespace dx12e
{

// shaders/ibl/*.hlsl の cbuffer IBLConstants と一致させる（16B）
struct IBLConstants
{
    u32   faceSize;
    float roughness;
    u32   envSize;   // src env cube の 1 面サイズ(mip0)。PrefilterEnv の PDF mip 選択用
    u32   _pad1;
};

static constexpr u32 kCBSlotStride = 256;  // CBV は 256B アライン

void IBLBaker::Initialize(GraphicsDevice& device, const std::wstring& shaderDirW)
{
    if (m_initialized) return;
    auto* dev = device.GetDevice();

    // --- Compute Root Signature: b0(CBV) + t0(SRV table) + u0(UAV table) + s0(linear clamp) ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;  // t0

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors     = 1;
        uavRange.BaseShaderRegister = 0;  // u0

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;  // b0
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &uavRange;
        params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderRegister   = 0;  // s0
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 3;
        desc.pParameters       = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
        if (FAILED(hr))
        {
            if (error)
                Logger::Error("IBLBaker のルートシグネチャ作成に失敗しました: {}",
                    static_cast<const char*>(error->GetBufferPointer()));
            ThrowIfFailed(hr);
        }
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_computeRS)));
    }

    // --- Compute PSOs ---
    m_shaderDir = shaderDirW;
    RecreatePipelines(device);

    CreateDerivedResources(device);

    m_initialized = true;
    Logger::Info("IBLBaker initialized");
}

void IBLBaker::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto makeCS = [&](const std::wstring& cso, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + cso);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_computeRS.Get();
        pso.CS = { bc.GetData(), bc.GetSize() };
        ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    makeCS(L"IrradianceConvolution_CS.cso", m_psoIrradiance);
    makeCS(L"PrefilterEnv_CS.cso",          m_psoPrefilter);
    makeCS(L"IntegrateBRDF_CS.cso",         m_psoBrdf);
}

void IBLBaker::CreateDerivedResources(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto makeArrayTex = [&](u32 size, u32 arr, u32 mips, DXGI_FORMAT fmt,
                            Microsoft::WRL::ComPtr<ID3D12Resource>& out)
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width            = size;
        d.Height           = size;
        d.DepthOrArraySize = static_cast<u16>(arr);
        d.MipLevels        = static_cast<u16>(mips);
        d.Format           = fmt;
        d.SampleDesc       = {1, 0};
        d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(dev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
            &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    };

    makeArrayTex(kIrradianceSize, 6, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, m_irradianceCube);
    makeArrayTex(kPrefilterSize, 6, kPrefilterMips, DXGI_FORMAT_R16G16B16A16_FLOAT, m_prefilteredCube);

    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width            = kBrdfSize;
        d.Height           = kBrdfSize;
        d.DepthOrArraySize = 1;
        d.MipLevels        = 1;
        d.Format           = DXGI_FORMAT_R16G16_FLOAT;
        d.SampleDesc       = {1, 0};
        d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(dev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
            &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_brdfLut)));
    }

    // constants upload バッファ（dispatch ぶんの 256B スロット）
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = kCBSlotStride * 8;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc       = {1, 0};
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES up{};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        ThrowIfFailed(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE,
            &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cbUpload)));
    }
}

void IBLBaker::Bake(GraphicsDevice& device, ID3D12GraphicsCommandList* cmdList,
                    DescriptorHeap& srvHeap, ID3D12Resource* envCube)
{
    DX_ASSERT(m_initialized, "IBLBaker::Initialize must be called before Bake");
    auto* dev = device.GetDevice();

    m_hasEnv = (envCube != nullptr);

    // ----- shader-visible 永続 SRV ブロック(3枚): t5=irradiance, t6=prefiltered, t7=brdf -----
    m_blockStart = srvHeap.AllocateBlock(3);
    DX_ASSERT(m_blockStart != DescriptorHeap::kInvalidIndex, "IBL SRV block allocation failed");
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        s.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURECUBE;
        s.TextureCube.MipLevels   = 1;
        dev->CreateShaderResourceView(m_irradianceCube.Get(), &s, srvHeap.GetCpuHandle(m_blockStart));

        s.TextureCube.MipLevels = kPrefilterMips;
        dev->CreateShaderResourceView(m_prefilteredCube.Get(), &s, srvHeap.GetCpuHandle(m_blockStart + 1));

        D3D12_SHADER_RESOURCE_VIEW_DESC l{};
        l.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        l.Format                  = DXGI_FORMAT_R16G16_FLOAT;
        l.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        l.Texture2D.MipLevels     = 1;
        dev->CreateShaderResourceView(m_brdfLut.Get(), &l, srvHeap.GetCpuHandle(m_blockStart + 2));
    }

    // 派生3枚のステート遷移。★Bake は環境マップ差し替え/シーン切替で何度も呼ばれるので、
    //   毎回「今どのステートか」を見て動かす。決め打ちで UNORDERED_ACCESS を StateBefore に
    //   すると 2 回目以降で不整合になり、ドライバが ACCESS_VIOLATION で落ちる。
    auto transitionDerived = [&](D3D12_RESOURCE_STATES after)
    {
        if (m_derivedState == after) return;
        D3D12_RESOURCE_BARRIER b[3]{};
        ID3D12Resource* res[3] = { m_irradianceCube.Get(), m_prefilteredCube.Get(), m_brdfLut.Get() };
        for (int i = 0; i < 3; ++i)
        {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition.pResource   = res[i];
            b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[i].Transition.StateBefore = m_derivedState;
            b[i].Transition.StateAfter  = after;
        }
        cmdList->ResourceBarrier(3, b);
        m_derivedState = after;
    };

    // env が無ければベイクせず、リソースを SRV へ遷移して bind 可能にして終了（hasIBL=false 運用）
    if (!envCube)
    {
        transitionDerived(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_valid = true;   // テーブルは有効（中身ダミー）
        return;
    }

    // 書き込みは UAV として行う（前回 Bake 済みなら SRV から戻す）
    transitionDerived(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ===== compute ベイク =====
    // env cube を読むため NON_PIXEL_SHADER_RESOURCE へ遷移。
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = envCube;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmdList->ResourceBarrier(1, &b);
    }

    // compute のディスクリプタテーブルは shader-visible ヒープが要る。
    // env SRV(1) + UAV(irr1 + prefilter5 + brdf1 = 7) を srvHeap に一時確保し、終了後 Free する。
    const u32 tmpEnvSrv   = srvHeap.AllocateIndex();
    const u32 tmpUavStart = srvHeap.AllocateBlock(7);
    DX_ASSERT(tmpEnvSrv != DescriptorHeap::kInvalidIndex && tmpUavStart != DescriptorHeap::kInvalidIndex,
              "IBL temp descriptor allocation failed");

    // env SRV (TextureCube, 全 mip)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sd.Format                  = envCube->GetDesc().Format;
        sd.TextureCube.MipLevels   = static_cast<UINT>(-1);
        dev->CreateShaderResourceView(envCube, &sd, srvHeap.GetCpuHandle(tmpEnvSrv));
    }

    // UAVs (array)
    auto makeArrayUAV = [&](ID3D12Resource* res, u32 mip, u32 dstIdx)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format                         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        u.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        u.Texture2DArray.MipSlice        = mip;
        u.Texture2DArray.FirstArraySlice = 0;
        u.Texture2DArray.ArraySize       = 6;
        dev->CreateUnorderedAccessView(res, nullptr, &u, srvHeap.GetCpuHandle(dstIdx));
    };
    makeArrayUAV(m_irradianceCube.Get(), 0, tmpUavStart);
    for (u32 m = 0; m < kPrefilterMips; ++m)
        makeArrayUAV(m_prefilteredCube.Get(), m, tmpUavStart + 1 + m);
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC lu{};
        lu.Format        = DXGI_FORMAT_R16G16_FLOAT;
        lu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(m_brdfLut.Get(), nullptr, &lu,
            srvHeap.GetCpuHandle(tmpUavStart + 1 + kPrefilterMips));
    }

    // src env cube の 1 面サイズ(mip0)。PrefilterEnv の PDF ベース mip 選択に渡す。
    const u32 envFaceSize = static_cast<u32>(envCube->GetDesc().Width);

    // constants
    u8* cbBase = nullptr;
    ThrowIfFailed(m_cbUpload->Map(0, nullptr, reinterpret_cast<void**>(&cbBase)));
    auto writeCB = [&](u32 slot, u32 faceSize, float roughness, u32 envSize) -> D3D12_GPU_VIRTUAL_ADDRESS {
        IBLConstants c{ faceSize, roughness, envSize, 0u };
        std::memcpy(cbBase + static_cast<size_t>(slot) * kCBSlotStride, &c, sizeof(c));
        return m_cbUpload->GetGPUVirtualAddress() + static_cast<UINT64>(slot) * kCBSlotStride;
    };
    D3D12_GPU_VIRTUAL_ADDRESS cbIrr = writeCB(0, kIrradianceSize, 0.0f, envFaceSize);
    D3D12_GPU_VIRTUAL_ADDRESS cbPre[kPrefilterMips];
    for (u32 m = 0; m < kPrefilterMips; ++m)
    {
        u32 sz = kPrefilterSize >> m;
        float rough = static_cast<float>(m) / static_cast<float>(kPrefilterMips - 1);
        cbPre[m] = writeCB(1 + m, sz, rough, envFaceSize);
    }
    D3D12_GPU_VIRTUAL_ADDRESS cbBrdf = writeCB(1 + kPrefilterMips, kBrdfSize, 0.0f, envFaceSize);
    m_cbUpload->Unmap(0, nullptr);

    // ★ディスクリプタテーブルを積む前に shader-visible ヒープを必ず bind する。
    //   Bake は「毎フレームのレンダ経路」だけでなく Application::Initialize の専用
    //   コマンドリスト（ヒープ未 bind）からも呼ばれる。bind を忘れると
    //   SetComputeRootDescriptorTable が無効なハンドルを積み、Dispatch でドライバが
    //   ACCESS_VIOLATION する（起動時クラッシュの正体）。同じヒープを張り直すだけなので
    //   レンダ経路から呼ばれた場合も無害。
    {
        ID3D12DescriptorHeap* heaps[] = { srvHeap.GetHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);
    }

    cmdList->SetComputeRootSignature(m_computeRS.Get());

    auto dispatch = [&](ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb,
                        u32 uavIdx, u32 gx, u32 gy, u32 gz)
    {
        cmdList->SetPipelineState(pso);
        cmdList->SetComputeRootConstantBufferView(0, cb);
        cmdList->SetComputeRootDescriptorTable(1, srvHeap.GetGpuHandle(tmpEnvSrv));
        cmdList->SetComputeRootDescriptorTable(2, srvHeap.GetGpuHandle(uavIdx));
        cmdList->Dispatch(gx, gy, gz);
    };

    // irradiance: (32/8, 32/8, 6)
    dispatch(m_psoIrradiance.Get(), cbIrr, tmpUavStart,
             kIrradianceSize / 8, kIrradianceSize / 8, 6);

    // prefiltered: mip ごと
    for (u32 m = 0; m < kPrefilterMips; ++m)
    {
        u32 sz = kPrefilterSize >> m;
        u32 g  = (sz + 7) / 8;
        dispatch(m_psoPrefilter.Get(), cbPre[m], tmpUavStart + 1 + m, g, g, 6);
    }

    // brdf LUT (env table は使われないが bind は維持)
    dispatch(m_psoBrdf.Get(), cbBrdf, tmpUavStart + 1 + kPrefilterMips,
             kBrdfSize / 8, kBrdfSize / 8, 1);

    // UAV → PIXEL_SHADER_RESOURCE（forward が読む）
    transitionDerived(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // env cube を元の PIXEL_SHADER_RESOURCE へ戻す（skybox が読む）
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = envCube;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmdList->ResourceBarrier(1, &b);
    }

    // 一時ディスクリプタを返却。Bake は Execute+WaitIdle 込みで1回呼ばれる運用のため即時 Free で安全。
    srvHeap.Free(tmpEnvSrv);
    srvHeap.FreeBlock(tmpUavStart, 7);

    m_valid = true;
    Logger::Info("IBL baked (irradiance/prefiltered/BRDF LUT)");
}

void IBLBaker::Reset()
{
    m_psoIrradiance.Reset();
    m_psoPrefilter.Reset();
    m_psoBrdf.Reset();
    m_computeRS.Reset();
    m_irradianceCube.Reset();
    m_prefilteredCube.Reset();
    m_brdfLut.Reset();
    m_cbUpload.Reset();
    m_derivedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;  // 作り直したら UAV から
    m_valid = false;
    m_hasEnv = false;
    m_initialized = false;
}

} // namespace dx12e
