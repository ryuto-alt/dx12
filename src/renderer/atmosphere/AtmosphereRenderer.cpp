#include "renderer/atmosphere/AtmosphereRenderer.h"

#include <algorithm>
#include <cstring>

#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/Buffer.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/FrameResources.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"

namespace dx12e
{
using namespace atmosphere;

namespace
{
// LUT は 32bit float。★fp16 だと、太陽が地平線の下 6°〜18° の薄明(照度 1 で正規化した放射輝度が 1e-5 前後 = fp16 の非正規数)で
//   量子化が粗くなり、空に段々の帯が出る。合計 1.7 MiB。環境キューブ(IBL の元)だけは IBLBaker と同じ RGBA16F。
constexpr DXGI_FORMAT kLutFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr DXGI_FORMAT kCubeFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr D3D12_RESOURCE_STATES kSrvState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

bool AtmosphereRenderer::SvKey::operator==(const SvKey& o) const
{
    return camAlt == o.camAlt && lightDir[0] == o.lightDir[0] && lightDir[1] == o.lightDir[1]
        && lightDir[2] == o.lightDir[2] && msFactor == o.msFactor && media == o.media;
}

const char* AtmosphereRenderer::ScopeName(u32 s)
{
    switch (s)
    {
        case ScopeTransMs:     return "transmittanceMultiScattering";
        case ScopeSkyView:     return "skyViewLut";
        case ScopeAp:          return "aerialPerspectiveLut";
        case ScopeSkyCube:     return "skyCube";
        case ScopeSkyDraw:     return "skyDraw";
        case ScopeApComposite: return "apComposite";
        case ScopeIblIrr:      return "iblIrradiance";
        case ScopeIblPre:      return "iblPrefilter";
        default:               return "?";
    }
}

bool AtmosphereRenderer::Initialize(GraphicsDevice& device, ID3D12CommandQueue* queue, DescriptorHeap* srvHeap,
                                    const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_srvHeap = srvHeap;
    m_shaderDir = shaderDir;
    if (!dev || !srvHeap) return false;

    // ---- ルートシグネチャ ----
    //   [0] CBV b0(AtmosphereCB) / [1] SRV t0..t4(LUT 5 枚)/ [2] SRV t5(深度)/ [3] UAV u0..u5 / [4] 定数 b1(4 DWORD)/ [5] UAV u6(キューブ mip 1..)
    //   s0 = LINEAR CLAMP。DWORD: 2 + 1 + 1 + 1 + 4 + 1 = 10 / 64
    {
        D3D12_DESCRIPTOR_RANGE r1{}, r2{}, r3{};
        r1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r1.NumDescriptors = 5; r1.BaseShaderRegister = 0;
        r1.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        r2.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r2.NumDescriptors = 1; r2.BaseShaderRegister = 5;
        r2.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        r3.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r3.NumDescriptors = 6; r3.BaseShaderRegister = 0;
        r3.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE r4{};   // u6: 環境キューブの mip 1.. の書き込み先(ディスパッチごとに 1 つ差し替える)
        r4.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r4.NumDescriptors = 1; r4.BaseShaderRegister = 6;
        r4.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER p[6]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].Descriptor.ShaderRegister = 0;
        p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_DESCRIPTOR_RANGE* ranges[3] = {&r1, &r2, &r3};
        for (int i = 0; i < 3; ++i)
        {
            p[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[1 + i].DescriptorTable.NumDescriptorRanges = 1;
            p[1 + i].DescriptorTable.pDescriptorRanges = ranges[i];
            p[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[4].Constants.ShaderRegister = 1;
        p[4].Constants.Num32BitValues = 4;
        p[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[5].DescriptorTable.NumDescriptorRanges = 1;
        p[5].DescriptorTable.pDescriptorRanges = &r4;
        p[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = _countof(p);
        desc.pParameters = p;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers = &samp;
        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                               IID_PPV_ARGS(&m_rs)));
    }

    // ---- LUT ----
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto makeTex = [&](Tex& t, D3D12_RESOURCE_DIMENSION dim, u32 w, u32 h, u32 d, const wchar_t* name, DXGI_FORMAT fmt = kLutFormat, u32 mips = 1)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = dim;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = static_cast<UINT16>(d);
        desc.MipLevels = static_cast<UINT16>(mips);
        desc.Format = fmt;
        desc.SampleDesc = {1, 0};
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&t.res)));
        t.res->SetName(name);
        t.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };
    makeTex(m_trans, D3D12_RESOURCE_DIMENSION_TEXTURE2D, kTransmittanceW, kTransmittanceH, 1, L"AtmosTransmittance");
    makeTex(m_ms,    D3D12_RESOURCE_DIMENSION_TEXTURE2D, kMultiScatW, kMultiScatH, 1, L"AtmosMultiScattering");
    makeTex(m_sky,   D3D12_RESOURCE_DIMENSION_TEXTURE2D, kSkyViewW, kSkyViewH, 1, L"AtmosSkyView");
    makeTex(m_apL,   D3D12_RESOURCE_DIMENSION_TEXTURE3D, kApW, kApH, kApD, L"AtmosApL");
    makeTex(m_apT,   D3D12_RESOURCE_DIMENSION_TEXTURE3D, kApW, kApH, kApD, L"AtmosApT");
    makeTex(m_cubeTex, D3D12_RESOURCE_DIMENSION_TEXTURE2D, kSkyCubeSize, kSkyCubeSize, 6, L"AtmosSkyCube", kCubeFormat, kSkyCubeMips);
    m_cube = m_cubeTex.res;

    // PT が引くパラメータ(UPLOAD・永続マップ・raw SRV)
    {
        D3D12_HEAP_PROPERTIES up{};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 512;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc = {1, 0};
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_paramsBuf)));
        ThrowIfFailed(m_paramsBuf->Map(0, nullptr, reinterpret_cast<void**>(&m_paramsMapped)));
        std::memset(m_paramsMapped, 0, 512);
    }

    // ---- ディスクリプタ ----
    m_srvBlock = srvHeap->AllocateBlock(kSrvCount);
    m_uavBlock = srvHeap->AllocateBlock(kUavCount);
    if (m_srvBlock == DescriptorHeap::kInvalidIndex || m_uavBlock == DescriptorHeap::kInvalidIndex)
    {
        Logger::Error("AtmosphereRenderer: ディスクリプタの確保に失敗");
        return false;
    }
    {
        auto srv2d = [&](ID3D12Resource* r, u32 idx)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = kLutFormat; s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(r, &s, srvHeap->GetCpuHandle(m_srvBlock + idx));
        };
        auto srv3d = [&](ID3D12Resource* r, u32 idx)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = kLutFormat; s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.Texture3D.MipLevels = 1;
            dev->CreateShaderResourceView(r, &s, srvHeap->GetCpuHandle(m_srvBlock + idx));
        };
        srv2d(m_trans.res.Get(), 0);
        srv2d(m_ms.res.Get(), 1);
        srv2d(m_sky.res.Get(), 2);
        srv3d(m_apL.res.Get(), 3);
        srv3d(m_apT.res.Get(), 4);
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = kCubeFormat; s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.TextureCube.MipLevels = kSkyCubeMips;
            dev->CreateShaderResourceView(m_cube.Get(), &s, srvHeap->GetCpuHandle(m_srvBlock + 5));
        }
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = DXGI_FORMAT_R32_TYPELESS; s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.Buffer.FirstElement = 0; s.Buffer.NumElements = 512 / 4;
            s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            dev->CreateShaderResourceView(m_paramsBuf.Get(), &s, srvHeap->GetCpuHandle(m_srvBlock + 6));
        }
        auto uav2d = [&](ID3D12Resource* r, u32 idx)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = kLutFormat; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            dev->CreateUnorderedAccessView(r, nullptr, &u, srvHeap->GetCpuHandle(m_uavBlock + idx));
        };
        auto uav3d = [&](ID3D12Resource* r, u32 idx, u32 depth)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = kLutFormat; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            u.Texture3D.WSize = depth;
            dev->CreateUnorderedAccessView(r, nullptr, &u, srvHeap->GetCpuHandle(m_uavBlock + idx));
        };
        uav2d(m_trans.res.Get(), 0);
        uav2d(m_ms.res.Get(), 1);
        uav2d(m_sky.res.Get(), 2);
        uav3d(m_apL.res.Get(), 3, kApD);
        uav3d(m_apT.res.Get(), 4, kApD);
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = kCubeFormat; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            u.Texture2DArray.ArraySize = 6;
            for (u32 m = 0; m < kSkyCubeMips; ++m)
            {
                u.Texture2DArray.MipSlice = m;
                dev->CreateUnorderedAccessView(m_cube.Get(), nullptr, &u, srvHeap->GetCpuHandle(m_uavBlock + 5 + m));
            }
        }
    }

    m_cb = std::make_unique<ConstantBuffer>();
    m_cb->Initialize(device, sizeof(AtmosphereGpuParams), FrameResources::kFrameCount * 2);   // 主ビュー 3 + 副ビュー 3

    // ---- GPU タイマー ----
    if (queue && SUCCEEDED(queue->GetTimestampFrequency(&m_freq)) && m_freq != 0)
    {
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = kFrames * ScopeCount * 2;
        if (SUCCEEDED(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_qheap))))
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = static_cast<u64>(qh.Count) * sizeof(u64);
            rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_qreadback))))
                m_qheap.Reset();
        }
    }

    RecreatePipelines(device);
    m_ready = true;
    Logger::Info("AtmosphereRenderer initialized (T {}x{} / MS {}x{} / SkyView {}x{} / AP {}x{}x{} / cube {}^2x6)",
                 kTransmittanceW, kTransmittanceH, kMultiScatW, kMultiScatH, kSkyViewW, kSkyViewH, kApW, kApH, kApD, kSkyCubeSize);
    return true;
}

void AtmosphereRenderer::Shutdown()
{
    if (m_srvHeap)
    {
        if (m_srvBlock != DescriptorHeap::kInvalidIndex) m_srvHeap->FreeBlock(m_srvBlock, kSrvCount);
        if (m_uavBlock != DescriptorHeap::kInvalidIndex) m_srvHeap->FreeBlock(m_uavBlock, kUavCount);
    }
    m_srvBlock = m_uavBlock = DescriptorHeap::kInvalidIndex;
    if (m_paramsBuf && m_paramsMapped) m_paramsBuf->Unmap(0, nullptr);
    m_paramsMapped = nullptr;
    m_paramsBuf.Reset();
    m_cb.reset();
    m_trans.res.Reset(); m_ms.res.Reset(); m_sky.res.Reset(); m_apL.res.Reset(); m_apT.res.Reset();
    m_cubeTex.res.Reset(); m_cube.Reset();
    m_psoT.Reset(); m_psoMs.Reset(); m_psoSv.Reset(); m_psoAp.Reset(); m_psoCube.Reset(); m_psoCubeMip.Reset();
    m_psoSky.Reset(); m_psoApComp.Reset();
    m_rs.Reset();
    m_qheap.Reset(); m_qreadback.Reset();
    m_srvHeap = nullptr;
    m_ready = false;
}

void AtmosphereRenderer::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto makeCS = [&](const wchar_t* cso, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + cso);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rs.Get();
        pso.CS = {bc.GetData(), bc.GetSize()};
        ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    makeCS(L"AtmosphereT_CS.cso",    m_psoT);
    makeCS(L"AtmosphereMs_CS.cso",   m_psoMs);
    makeCS(L"AtmosphereSv_CS.cso",   m_psoSv);
    makeCS(L"AtmosphereAp_CS.cso",   m_psoAp);
    makeCS(L"AtmosphereCube_CS.cso", m_psoCube);
    makeCS(L"AtmosphereCubeMip_CS.cso", m_psoCubeMip);

    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"AtmosphereSky_VS.cso");
    auto makeGfx = [&](const wchar_t* psName, bool dual, DXGI_FORMAT dsv, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + psName);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rs.Get();
        pso.VS = {vs.GetData(), vs.GetSize()};
        pso.PS = {ps.GetData(), ps.GetSize()};
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        auto& rt = pso.BlendState.RenderTarget[0];
        if (dual)
        {
            // dst = src0 + dst * src1(RGB の透過率をそのまま掛ける。FogComposite の dst = src.rgb + dst*src.a のカラー版)
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_ONE;
            rt.DestBlend = D3D12_BLEND_SRC1_COLOR;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
            rt.DestBlendAlpha = D3D12_BLEND_ONE;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;   // kSceneColorFormat
        pso.DSVFormat = dsv;
        pso.SampleDesc = {1, 0};
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    makeGfx(L"AtmosphereSky_PS.cso", false, DXGI_FORMAT_D32_FLOAT, m_psoSky);
    makeGfx(L"AtmosphereAp_PS.cso",  true,  DXGI_FORMAT_UNKNOWN,   m_psoApComp);
}

void AtmosphereRenderer::Transition(ID3D12GraphicsCommandList* cmd, Tex* const* texes, u32 n, D3D12_RESOURCE_STATES next)
{
    D3D12_RESOURCE_BARRIER b[6]{};
    u32 k = 0;
    for (u32 i = 0; i < n && k < 6; ++i)
    {
        Tex* t = texes[i];
        if (!t->res || t->state == next) continue;
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.pResource = t->res.Get();
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[k].Transition.StateBefore = t->state;
        b[k].Transition.StateAfter = next;
        t->state = next;
        ++k;
    }
    if (k) cmd->ResourceBarrier(k, b);
}

void AtmosphereRenderer::SetCommon(ID3D12GraphicsCommandList* cmd, u32 frameIndex, bool compute,
                                   D3D12_GPU_DESCRIPTOR_HANDLE depthSrv)
{
    ID3D12DescriptorHeap* heaps[] = {m_srvHeap->GetHeap()};
    cmd->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_VIRTUAL_ADDRESS cb = m_cb->GetGpuAddress(frameIndex);
    const auto srv = m_srvHeap->GetGpuHandle(m_srvBlock);
    const auto uav = m_srvHeap->GetGpuHandle(m_uavBlock);
    const u32 consts[4] = {kSkyCubeSize, 1, 0, 0};
    if (compute)
    {
        cmd->SetComputeRootSignature(m_rs.Get());
        cmd->SetComputeRootConstantBufferView(0, cb);
        cmd->SetComputeRootDescriptorTable(1, srv);
        cmd->SetComputeRootDescriptorTable(2, depthSrv.ptr ? depthSrv : srv);
        cmd->SetComputeRootDescriptorTable(3, uav);
        cmd->SetComputeRoot32BitConstants(4, 4, consts, 0);
        cmd->SetComputeRootDescriptorTable(5, m_srvHeap->GetGpuHandle(m_uavBlock + 6));
    }
    else
    {
        cmd->SetGraphicsRootSignature(m_rs.Get());
        cmd->SetGraphicsRootConstantBufferView(0, cb);
        cmd->SetGraphicsRootDescriptorTable(1, srv);
        cmd->SetGraphicsRootDescriptorTable(2, depthSrv.ptr ? depthSrv : srv);
        cmd->SetGraphicsRootDescriptorTable(3, uav);
        cmd->SetGraphicsRoot32BitConstants(4, 4, consts, 0);
        cmd->SetGraphicsRootDescriptorTable(5, m_srvHeap->GetGpuHandle(m_uavBlock + 6));
    }
}

void AtmosphereRenderer::TimerBegin(ID3D12GraphicsCommandList* cmd, u32 frameIndex, Scope s)
{
    if (!m_qheap) return;
    const u32 slot = frameIndex % kFrames;
    cmd->EndQuery(m_qheap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (slot * ScopeCount + s) * 2);
}

void AtmosphereRenderer::TimerEnd(ID3D12GraphicsCommandList* cmd, u32 frameIndex, Scope s)
{
    if (!m_qheap) return;
    const u32 slot = frameIndex % kFrames;
    const u32 q = (slot * ScopeCount + s) * 2;
    cmd->EndQuery(m_qheap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
    cmd->ResolveQueryData(m_qheap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q, 2, m_qreadback.Get(), static_cast<u64>(q) * sizeof(u64));
    m_wrote[slot] |= (1u << s);
}

void AtmosphereRenderer::BeginFrameTimers(u32 frameIndex)
{
    if (!m_qheap) return;
    const u32 slot = frameIndex % kFrames;
    const u32 mask = m_wrote[slot];
    for (u32 s = 0; s < ScopeCount; ++s) m_ranLast[s] = (mask >> s) & 1u;
    if (mask)
    {
        const u32 first = slot * ScopeCount * 2;
        const D3D12_RANGE range{first * sizeof(u64), (first + ScopeCount * 2) * sizeof(u64)};
        const u64* data = nullptr;
        if (SUCCEEDED(m_qreadback->Map(0, &range, reinterpret_cast<void**>(const_cast<u64**>(&data)))))
        {
            for (u32 s = 0; s < ScopeCount; ++s)
            {
                if (!((mask >> s) & 1u)) continue;
                const u64 a = data[first + s * 2], b = data[first + s * 2 + 1];
                if (b >= a) m_scopeMs[s] = static_cast<float>(static_cast<double>(b - a) * 1000.0 / static_cast<double>(m_freq));
            }
            const D3D12_RANGE none{0, 0};
            m_qreadback->Unmap(0, &none);
        }
    }
    m_wrote[slot] = 0;
}

void AtmosphereRenderer::Update(ID3D12GraphicsCommandList* cmd, u32 frameIndex, const AtmosphereGpuParams& g, bool forceAll)
{
    if (!m_ready) return;
    m_cb->Update(&g, sizeof(g), frameIndex);
    std::memcpy(m_paramsMapped, &g, sizeof(g));

    const u64 media = HashMediumParams(g);
    const bool doTMs = forceAll || !m_haveLut || media != m_mediaHash;
    SvKey key;
    key.camAlt = g.camAlt;
    key.lightDir[0] = g.lightDir[0]; key.lightDir[1] = g.lightDir[1]; key.lightDir[2] = g.lightDir[2];
    key.msFactor = g.msFactor;
    key.media = media;
    const bool doSv = forceAll || doTMs || !m_svValid || !(key == m_svKey);
    const bool doAp = (g.flags & kFlagAp) != 0u;

    SetCommon(cmd, frameIndex, true, {});

    if (doTMs)
    {
        TimerBegin(cmd, frameIndex, ScopeTransMs);
        Tex* both[2] = {&m_trans, &m_ms};
        Transition(cmd, both, 2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(m_psoT.Get());
        cmd->Dispatch(kTransmittanceW / 8, kTransmittanceH / 8, 1);
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = m_trans.res.Get();
        cmd->ResourceBarrier(1, &ub);
        Tex* t1[1] = {&m_trans};
        Transition(cmd, t1, 1, kSrvState);       // MS が T を読む
        cmd->SetPipelineState(m_psoMs.Get());
        cmd->Dispatch(kMultiScatW, kMultiScatH, 1);
        ub.UAV.pResource = m_ms.res.Get();
        cmd->ResourceBarrier(1, &ub);
        Tex* t2[1] = {&m_ms};
        Transition(cmd, t2, 1, kSrvState);
        TimerEnd(cmd, frameIndex, ScopeTransMs);
        m_mediaHash = media;
        m_haveLut = true;
        ++m_updateCount[0];
    }
    {
        // T / MS は SRV 状態で読む(初回以外も。前フレームの状態のまま)
        Tex* ro[2] = {&m_trans, &m_ms};
        Transition(cmd, ro, 2, kSrvState);
    }
    if (doSv)
    {
        TimerBegin(cmd, frameIndex, ScopeSkyView);
        Tex* t[1] = {&m_sky};
        Transition(cmd, t, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(m_psoSv.Get());
        cmd->Dispatch((kSkyViewW + 7) / 8, (kSkyViewH + 7) / 8, 1);
        Transition(cmd, t, 1, kSrvState);
        TimerEnd(cmd, frameIndex, ScopeSkyView);
        m_svKey = key;
        m_svValid = true;
        ++m_updateCount[1];
    }
    if (doAp)
    {
        TimerBegin(cmd, frameIndex, ScopeAp);
        Tex* t[2] = {&m_apL, &m_apT};
        Transition(cmd, t, 2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(m_psoAp.Get());
        cmd->Dispatch(kApW / 8, kApH / 8, 1);
        Transition(cmd, t, 2, kSrvState);
        TimerEnd(cmd, frameIndex, ScopeAp);
        ++m_updateCount[2];
    }
}

void AtmosphereRenderer::UploadParams(u32 slot, const AtmosphereGpuParams& g)
{
    if (!m_ready) return;
    m_cb->Update(&g, sizeof(g), slot);
}

void AtmosphereRenderer::RecordSkyCube(ID3D12GraphicsCommandList* cmd, u32 frameIndex)
{
    if (!m_ready) return;
    TimerBegin(cmd, frameIndex, ScopeSkyCube);
    SetCommon(cmd, frameIndex, true, {});
    Tex* t[1] = {&m_cubeTex};
    Transition(cmd, t, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->SetPipelineState(m_psoCube.Get());
    cmd->Dispatch(kSkyCubeSize / 8, kSkyCubeSize / 8, 6);
    // mip 1.. は各テクセルを 2x2 でスーパーサンプルして直接評価（mip 間の依存なし＝バリア不要）。
    cmd->SetPipelineState(m_psoCubeMip.Get());
    for (u32 m = 1; m < kSkyCubeMips; ++m)
    {
        const u32 sz = kSkyCubeSize >> m;
        const u32 consts[4] = {sz, 2, 0, 0};
        cmd->SetComputeRoot32BitConstants(4, 4, consts, 0);
        cmd->SetComputeRootDescriptorTable(5, m_srvHeap->GetGpuHandle(m_uavBlock + 5 + m));
        cmd->Dispatch((sz + 7) / 8, (sz + 7) / 8, 6);
    }
    Transition(cmd, t, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    TimerEnd(cmd, frameIndex, ScopeSkyCube);
    ++m_updateCount[3];
}

void AtmosphereRenderer::DrawSky(ID3D12GraphicsCommandList* cmd, u32 frameIndex, D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu)
{
    if (!m_ready) return;
    TimerBegin(cmd, frameIndex, ScopeSkyDraw);
    SetCommon(cmd, frameIndex, false, depthSrvGpu);
    cmd->SetPipelineState(m_psoSky.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
    TimerEnd(cmd, frameIndex, ScopeSkyDraw);
}

void AtmosphereRenderer::DrawAerialPerspective(ID3D12GraphicsCommandList* cmd, u32 frameIndex,
                                               D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu)
{
    if (!m_ready) return;
    TimerBegin(cmd, frameIndex, ScopeApComposite);
    SetCommon(cmd, frameIndex, false, depthSrvGpu);
    cmd->SetPipelineState(m_psoApComp.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
    TimerEnd(cmd, frameIndex, ScopeApComposite);
}

} // namespace dx12e
