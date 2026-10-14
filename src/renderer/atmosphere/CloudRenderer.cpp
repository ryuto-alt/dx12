#include "renderer/atmosphere/CloudRenderer.h"

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

namespace
{
constexpr DXGI_FORMAT kNoiseFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kDistFormat  = DXGI_FORMAT_R16_FLOAT;
constexpr D3D12_RESOURCE_STATES kSrvState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr u32 kNoiseCbSlot = FrameResources::kFrameCount * CloudRenderer::kSubFrames;

Microsoft::WRL::ComPtr<ID3D12Resource> MakeTex(ID3D12Device* dev, D3D12_RESOURCE_DIMENSION dim, u32 w, u32 h, u32 d, DXGI_FORMAT fmt,
                                               const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = dim;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = static_cast<UINT16>(d);
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> r;
    ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&r)));
    r->SetName(name);
    return r;
}
} // namespace

bool CloudRenderer::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, const std::wstring& shaderDir)
{
    m_dev = device.GetDevice();
    m_srvHeap = srvHeap;
    m_shaderDir = shaderDir;
    if (!m_dev || !srvHeap) return false;

    // ---- ルートシグネチャ ----
    //   [0] CBV b0(AtmosphereCB)[1] CBV b1(CloudCB)[2] SRV t0..t4(大気 LUT)[3] SRV t5..t9(ノイズ + 粗い結果)[4] SRV t10..t11(履歴)
    //   [5] SRV t12(深度)[6] UAV u0..u1(粗い結果)[7] UAV u2..u3(解決の出力)[8] UAV u4..u5(ノイズ生成)。s0 = LINEAR CLAMP / s1 = LINEAR WRAP。
    //   DWORD: 2 + 2 + 6 = 10 / 64
    {
        D3D12_DESCRIPTOR_RANGE r[7]{};
        auto range = [&](int i, D3D12_DESCRIPTOR_RANGE_TYPE type, u32 n, u32 base)
        {
            r[i].RangeType = type; r[i].NumDescriptors = n; r[i].BaseShaderRegister = base;
            r[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        };
        range(0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0);
        range(1, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 5);
        range(2, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 10);
        range(3, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 12);
        range(4, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);
        range(5, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 2);
        range(6, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 4);
        D3D12_ROOT_PARAMETER p[9]{};
        for (int i = 0; i < 2; ++i)
        {
            p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[i].Descriptor.ShaderRegister = i;
            p[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        for (int i = 0; i < 7; ++i)
        {
            p[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[2 + i].DescriptorTable.NumDescriptorRanges = 1;
            p[2 + i].DescriptorTable.pDescriptorRanges = &r[i];
            p[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_STATIC_SAMPLER_DESC samp[2]{};
        for (int i = 0; i < 2; ++i)
        {
            samp[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            const auto mode = (i == 0) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samp[i].AddressU = samp[i].AddressV = samp[i].AddressW = mode;
            samp[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            samp[i].MaxLOD = D3D12_FLOAT32_MAX;
            samp[i].ShaderRegister = i;
            samp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = _countof(p);
        desc.pParameters = p;
        desc.NumStaticSamplers = 2;
        desc.pStaticSamplers = samp;
        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(m_dev->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_rs)));
    }

    // ---- ノイズ(固定サイズ)----
    m_shape.res   = MakeTex(m_dev, D3D12_RESOURCE_DIMENSION_TEXTURE3D, kShapeSize, kShapeSize, kShapeSize, kNoiseFormat, L"CloudShapeNoise");
    m_detail.res  = MakeTex(m_dev, D3D12_RESOURCE_DIMENSION_TEXTURE3D, kDetailSize, kDetailSize, kDetailSize, kNoiseFormat, L"CloudDetailNoise");
    m_weather.res = MakeTex(m_dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, kWeatherSize, kWeatherSize, 1, kNoiseFormat, L"CloudWeatherMap");

    m_srvBlock = srvHeap->AllocateBlock(kSrvCount);
    m_uavBlock = srvHeap->AllocateBlock(kUavCount);
    if (m_srvBlock == DescriptorHeap::kInvalidIndex || m_uavBlock == DescriptorHeap::kInvalidIndex)
    {
        Logger::Error("CloudRenderer: ディスクリプタの確保に失敗");
        return false;
    }
    auto srv3d = [&](ID3D12Resource* res, u32 idx)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = kNoiseFormat; s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture3D.MipLevels = 1;
        m_dev->CreateShaderResourceView(res, &s, srvHeap->GetCpuHandle(m_srvBlock + idx));
    };
    srv3d(m_shape.res.Get(), 0);
    srv3d(m_detail.res.Get(), 1);
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = kNoiseFormat; s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MipLevels = 1;
        m_dev->CreateShaderResourceView(m_weather.res.Get(), &s, srvHeap->GetCpuHandle(m_srvBlock + 2));
    }
    auto uav3d = [&](ID3D12Resource* res, u32 idx, u32 depth)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = kNoiseFormat; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        u.Texture3D.WSize = depth;
        m_dev->CreateUnorderedAccessView(res, nullptr, &u, srvHeap->GetCpuHandle(m_uavBlock + idx));
    };
    auto uav2dNoise = [&](u32 idx)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = kNoiseFormat; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_dev->CreateUnorderedAccessView(m_weather.res.Get(), nullptr, &u, srvHeap->GetCpuHandle(m_uavBlock + idx));
    };
    uav3d(m_shape.res.Get(), 6, kShapeSize);
    uav2dNoise(7);
    uav3d(m_detail.res.Get(), 8, kDetailSize);
    uav2dNoise(9);

    m_cb = std::make_unique<ConstantBuffer>();
    m_cb->Initialize(device, sizeof(Cb), kNoiseCbSlot + 2);

    RecreatePipelines(device);
    m_ready = true;
    Logger::Info("CloudRenderer initialized (shape {}^3 / detail {}^3 / weather {}^2)", kShapeSize, kDetailSize, kWeatherSize);
    return true;
}

bool CloudRenderer::CreateSized(GraphicsDevice& /*device*/, u32 w, u32 h)
{
    m_w = w; m_h = h;
    m_cw = (w + 3) / 4; m_ch = (h + 3) / 4;
    m_traceC.res = MakeTex(m_dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, m_cw, m_ch, 1, kColorFormat, L"CloudTraceColor");
    m_traceD.res = MakeTex(m_dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, m_cw, m_ch, 1, kDistFormat, L"CloudTraceDist");
    m_traceC.state = m_traceD.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    for (int i = 0; i < 2; ++i)
    {
        m_histC[i].res = MakeTex(m_dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, w, h, 1, kColorFormat, i ? L"CloudHistColor1" : L"CloudHistColor0");
        m_histD[i].res = MakeTex(m_dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, w, h, 1, kDistFormat, i ? L"CloudHistDist1" : L"CloudHistDist0");
        m_histC[i].state = m_histD[i].state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    WriteSizedDescriptors();
    m_histValid = false;
    m_cur = 0;
    return true;
}

void CloudRenderer::WriteSizedDescriptors()
{
    auto srv = [&](ID3D12Resource* r, DXGI_FORMAT f, u32 idx)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = f; s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MipLevels = 1;
        m_dev->CreateShaderResourceView(r, &s, m_srvHeap->GetCpuHandle(m_srvBlock + idx));
    };
    auto uav = [&](ID3D12Resource* r, DXGI_FORMAT f, u32 idx)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = f; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_dev->CreateUnorderedAccessView(r, nullptr, &u, m_srvHeap->GetCpuHandle(m_uavBlock + idx));
    };
    srv(m_traceC.res.Get(), kColorFormat, 3);
    srv(m_traceD.res.Get(), kDistFormat, 4);
    uav(m_traceC.res.Get(), kColorFormat, 0);
    uav(m_traceD.res.Get(), kDistFormat, 1);
    for (u32 i = 0; i < 2; ++i)
    {
        srv(m_histC[i].res.Get(), kColorFormat, 5 + i * 2);
        srv(m_histD[i].res.Get(), kDistFormat, 6 + i * 2);
        uav(m_histC[i].res.Get(), kColorFormat, 2 + i * 2);
        uav(m_histD[i].res.Get(), kDistFormat, 3 + i * 2);
    }
}

void CloudRenderer::Shutdown()
{
    if (m_srvHeap)
    {
        if (m_srvBlock != DescriptorHeap::kInvalidIndex) m_srvHeap->FreeBlock(m_srvBlock, kSrvCount);
        if (m_uavBlock != DescriptorHeap::kInvalidIndex) m_srvHeap->FreeBlock(m_uavBlock, kUavCount);
    }
    m_srvBlock = m_uavBlock = DescriptorHeap::kInvalidIndex;
    m_cb.reset();
    m_shape.res.Reset(); m_detail.res.Reset(); m_weather.res.Reset();
    m_traceC.res.Reset(); m_traceD.res.Reset();
    for (int i = 0; i < 2; ++i) { m_histC[i].res.Reset(); m_histD[i].res.Reset(); }
    m_psoShape.Reset(); m_psoDetail.Reset(); m_psoWeather.Reset(); m_psoTrace.Reset(); m_psoResolve.Reset(); m_psoComposite.Reset();
    m_rs.Reset();
    m_srvHeap = nullptr;
    m_dev = nullptr;
    m_ready = false;
    m_noiseReady = false;
    m_histValid = false;
    m_w = m_h = 0;
}

void CloudRenderer::RecreatePipelines(GraphicsDevice& device)
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
    makeCS(L"CloudShape_CS.cso",   m_psoShape);
    makeCS(L"CloudDetail_CS.cso",  m_psoDetail);
    makeCS(L"CloudWeather_CS.cso", m_psoWeather);
    makeCS(L"CloudTrace_CS.cso",   m_psoTrace);
    makeCS(L"CloudResolve_CS.cso", m_psoResolve);

    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"CloudComposite_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"CloudComposite_PS.cso");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rs.Get();
    pso.VS = {vs.GetData(), vs.GetSize()};
    pso.PS = {ps.GetData(), ps.GetSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    auto& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;                       // dst = src.rgb + dst * src.a(a = 雲の透過率)
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;   // kSceneColorFormat
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc = {1, 0};
    ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoComposite)));
}

void CloudRenderer::Barrier(ID3D12GraphicsCommandList* cmd, Tex* const* texes, u32 n, D3D12_RESOURCE_STATES next)
{
    D3D12_RESOURCE_BARRIER b[8]{};
    u32 k = 0;
    for (u32 i = 0; i < n && k < 8; ++i)
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

D3D12_GPU_DESCRIPTOR_HANDLE CloudRenderer::Srv(u32 i) const { return m_srvHeap->GetGpuHandle(m_srvBlock + i); }
D3D12_GPU_DESCRIPTOR_HANDLE CloudRenderer::Uav(u32 i) const { return m_srvHeap->GetGpuHandle(m_uavBlock + i); }

void CloudRenderer::BindCommon(ID3D12GraphicsCommandList* cmd, bool compute, D3D12_GPU_VIRTUAL_ADDRESS atmCb,
                               D3D12_GPU_DESCRIPTOR_HANDLE atmSrv, D3D12_GPU_VIRTUAL_ADDRESS cloudCb, u32 histSrvPair, u32 outUav,
                               u32 volUav, D3D12_GPU_DESCRIPTOR_HANDLE depthSrv)
{
    ID3D12DescriptorHeap* heaps[] = {m_srvHeap->GetHeap()};
    cmd->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE depth = depthSrv.ptr ? depthSrv : Srv(0);
    if (compute)
    {
        cmd->SetComputeRootSignature(m_rs.Get());
        cmd->SetComputeRootConstantBufferView(0, atmCb);
        cmd->SetComputeRootConstantBufferView(1, cloudCb);
        cmd->SetComputeRootDescriptorTable(2, atmSrv);
        cmd->SetComputeRootDescriptorTable(3, Srv(0));
        cmd->SetComputeRootDescriptorTable(4, Srv(histSrvPair));
        cmd->SetComputeRootDescriptorTable(5, depth);
        cmd->SetComputeRootDescriptorTable(6, Uav(0));
        cmd->SetComputeRootDescriptorTable(7, Uav(outUav));
        cmd->SetComputeRootDescriptorTable(8, Uav(volUav));
    }
    else
    {
        cmd->SetGraphicsRootSignature(m_rs.Get());
        cmd->SetGraphicsRootConstantBufferView(0, atmCb);
        cmd->SetGraphicsRootConstantBufferView(1, cloudCb);
        cmd->SetGraphicsRootDescriptorTable(2, atmSrv);
        cmd->SetGraphicsRootDescriptorTable(3, Srv(0));
        cmd->SetGraphicsRootDescriptorTable(4, Srv(histSrvPair));
        cmd->SetGraphicsRootDescriptorTable(5, depth);
        cmd->SetGraphicsRootDescriptorTable(6, Uav(0));
        cmd->SetGraphicsRootDescriptorTable(7, Uav(outUav));
        cmd->SetGraphicsRootDescriptorTable(8, Uav(volUav));
    }
}

void CloudRenderer::GenerateNoise(ID3D12GraphicsCommandList* cmd, u32 /*frameIndex*/)
{
    // 先頭 16 バイト(uint4)だけがノイズ用: x = 3D 解像度 y = 2D 解像度。形状/天気と詳細で別スロット(CB は実行時に読まれるので上書きできない)
    Cb cb{};
    cb.resX = kShapeSize; cb.resY = kWeatherSize;
    m_cb->Update(&cb, sizeof(cb), kNoiseCbSlot);
    cb.resX = kDetailSize;
    m_cb->Update(&cb, sizeof(cb), kNoiseCbSlot + 1);
    const D3D12_GPU_DESCRIPTOR_HANDLE none{};

    // 形状 + 天気(UAV u4 = 形状 / u5 = 天気)。大気の CB・LUT は読まないので自前のもので穴埋めする。
    BindCommon(cmd, true, m_cb->GetGpuAddress(kNoiseCbSlot), Srv(0), m_cb->GetGpuAddress(kNoiseCbSlot), 5, 2, 6, none);
    cmd->SetPipelineState(m_psoShape.Get());
    cmd->Dispatch(kShapeSize / 4, kShapeSize / 4, kShapeSize / 4);
    cmd->SetPipelineState(m_psoWeather.Get());
    cmd->Dispatch(kWeatherSize / 8, kWeatherSize / 8, 1);
    // 詳細(u4 = 詳細)
    cmd->SetComputeRootConstantBufferView(1, m_cb->GetGpuAddress(kNoiseCbSlot + 1));
    cmd->SetComputeRootDescriptorTable(8, Uav(8));
    cmd->SetPipelineState(m_psoDetail.Get());
    cmd->Dispatch(kDetailSize / 4, kDetailSize / 4, kDetailSize / 4);

    Tex* all[3] = {&m_shape, &m_detail, &m_weather};
    for (Tex* t : all) t->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Barrier(cmd, all, 3, kSrvState);
    m_noiseReady = true;
}

void CloudRenderer::Record(ID3D12GraphicsCommandList* cmd, u32 frameIndex, GraphicsDevice& device, const CloudSettings& s,
                           const FrameInfo& f, D3D12_GPU_VIRTUAL_ADDRESS atmCb, D3D12_GPU_DESCRIPTOR_HANDLE atmSrv)
{
    if (!m_ready || f.width == 0 || f.height == 0) return;
    if (NeedsResize(f.width, f.height)) CreateSized(device, f.width, f.height);   // 呼び出し側が GPU を待ってある前提
    if (!m_noiseReady) GenerateNoise(cmd, frameIndex);

    const bool reset = f.forceReset || !m_histValid || !f.havePrev;
    const u32 n = reset ? kSubFrames : 1u;
    if (f.settingsChanged) m_fastFrames = kSubFrames;
    const float histBlend = (m_fastFrames > 0) ? 0.35f : 0.04f;
    if (!reset && m_fastFrames > 0) --m_fastFrames;

    const u32 slotBase = (frameIndex % FrameResources::kFrameCount) * kSubFrames;
    const D3D12_GPU_DESCRIPTOR_HANDLE none{};
    for (u32 sub = 0; sub < n; ++sub)
    {
        Cb cb{};
        cb.resX = m_w; cb.resY = m_h; cb.coarseX = m_cw; cb.coarseY = m_ch;
        // 一括生成中は同じカメラ(現在の行列)で再投影する
        std::memcpy(cb.prevViewProj, reset ? f.viewProjT : f.prevViewProjT, sizeof(cb.prevViewProj));
        std::memcpy(cb.windOffsetKm, f.windOffsetKm, sizeof(cb.windOffsetKm));
        cb.coverage = s.coverage;
        cb.cloudType = s.cloudType;
        cb.density = s.density;
        cb.baseKm = s.baseAltitudeKm;
        cb.thickKm = std::max(s.thicknessKm, 0.1f);
        cb.detail = s.detailStrength;
        cb.scale = s.scale;
        cb.maxDistKm = kMaxDistKm;
        cb.histBlend = histBlend;
        cb.steps = static_cast<u32>(std::clamp(s.steps, 16, 128));
        cb.frame = reset ? sub : m_frameCounter;
        cb.flags = (reset ? kFlagBurst : 0u) | ((reset && sub == 0) ? kFlagReset : 0u);
        const u32 slot = slotBase + sub;
        m_cb->Update(&cb, sizeof(cb), slot);
        const D3D12_GPU_VIRTUAL_ADDRESS cloudCb = m_cb->GetGpuAddress(slot);

        const u32 src = m_cur;          // 直近に書いた履歴(一括生成の最初は RESET なので中身は読まれない)
        const u32 cur = 1u - m_cur;
        m_lastSlot = slot;
        Tex* tr[2] = {&m_traceC, &m_traceD};
        Barrier(cmd, tr, 2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        BindCommon(cmd, true, atmCb, atmSrv, cloudCb, 5 + src * 2, 2 + cur * 2, 6, none);
        cmd->SetPipelineState(m_psoTrace.Get());
        cmd->Dispatch((m_cw + 7) / 8, (m_ch + 7) / 8, 1);

        Tex* src2[2] = {&m_histC[src], &m_histD[src]};
        Tex* dst2[2] = {&m_histC[cur], &m_histD[cur]};
        Barrier(cmd, tr, 2, kSrvState);
        Barrier(cmd, src2, 2, kSrvState);
        Barrier(cmd, dst2, 2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(m_psoResolve.Get());
        cmd->Dispatch((m_w + 7) / 8, (m_h + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER ub[2]{};
        ub[0].Type = ub[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub[0].UAV.pResource = m_histC[cur].res.Get();
        ub[1].UAV.pResource = m_histD[cur].res.Get();
        cmd->ResourceBarrier(2, ub);
        Barrier(cmd, dst2, 2, kSrvState);
        m_cur = cur;
        m_histValid = true;
    }
    if (reset) m_frameCounter = kSubFrames; else ++m_frameCounter;
}

void CloudRenderer::DrawComposite(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS atmCb, D3D12_GPU_DESCRIPTOR_HANDLE atmSrv,
                                  D3D12_GPU_DESCRIPTOR_HANDLE depthSrv)
{
    if (!m_ready || !m_histValid) return;
    // CloudCB は合成で使う値(基準高度など)が要る。直近の Record のスロットをそのまま使う(行列以外は同じ)
    BindCommon(cmd, false, atmCb, atmSrv, m_cb->GetGpuAddress(m_lastSlot), 5 + m_cur * 2, 2, 6, depthSrv);
    cmd->SetPipelineState(m_psoComposite.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace dx12e
