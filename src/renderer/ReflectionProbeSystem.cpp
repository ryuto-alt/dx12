#include "renderer/ReflectionProbeSystem.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "graphics/Buffer.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <chrono>
#include <cstring>

namespace dx12e
{

namespace
{
constexpr DXGI_FORMAT kProbeColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;   // kSceneColorFormat と同じ

void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, UINT sub,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.Subresource = sub;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}
}

double ReflectionProbeSystem::NowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int ReflectionProbeSystem::FindSlot(entt::entity e) const
{
    for (u32 i = 0; i < kMaxProbes; ++i)
        if (m_slots[i].used && m_slots[i].entity == e) return static_cast<int>(i);
    return -1;
}

int ReflectionProbeSystem::AcquireSlot(entt::entity e)
{
    const int f = FindSlot(e);
    if (f >= 0) return f;
    for (u32 i = 0; i < kMaxProbes; ++i)
        if (!m_slots[i].used)
        {
            m_slots[i] = Slot{};
            m_slots[i].used = true;
            m_slots[i].entity = e;
            return static_cast<int>(i);
        }
    return -1;
}

void ReflectionProbeSystem::ResetAll()
{
    for (auto& s : m_slots) s = Slot{};
}

void ReflectionProbeSystem::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, DescriptorHeap* rtvHeap,
                                       DescriptorHeap* dsvHeap, const std::wstring& shaderDirW,
                                       u32 frameConstantsBytes)
{
    if (m_ready) return;
    m_srvHeap = srvHeap;
    m_shaderDir = shaderDirW;
    auto* dev = device.GetDevice();

    // ---- 撮影用: HDR の RT + 深度 + 面ごとの b1 ----
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_captureRT = std::make_unique<RenderTarget>();
    m_captureRT->Initialize(device, rtvHeap, srvHeap, kCaptureSize, kCaptureSize, kProbeColorFormat, clear);
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = kCaptureSize; d.Height = kCaptureSize;
        d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc = {1, 0};
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil = {1.0f, 0};
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&m_depth)));
        m_dsv = dsvHeap->Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(m_depth.Get(), &dv, m_dsv);
    }
    for (auto& cb : m_faceCB)
    {
        cb = std::make_unique<ConstantBuffer>();
        cb->Initialize(device, frameConstantsBytes, kFrameCount);
    }

    // ---- compute ルートシグネチャ: b0(4 DWORD) + t0(SRV table) + u0(UAV table) + s0(linear clamp) ----
    {
        D3D12_DESCRIPTOR_RANGE srvRange{}; srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srvRange.NumDescriptors = 1;
        D3D12_DESCRIPTOR_RANGE uavRange{}; uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uavRange.NumDescriptors = 1;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = 4;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1; p[1].DescriptorTable.pDescriptorRanges = &srvRange;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1; p[2].DescriptorTable.pDescriptorRanges = &uavRange;
        D3D12_STATIC_SAMPLER_DESC s{};
        s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 3; rd.pParameters = p; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &s;
        Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr))
        {
            if (err) Logger::Error("ReflectionProbeSystem: ルートシグネチャ作成に失敗: {}", static_cast<const char*>(err->GetBufferPointer()));
            ThrowIfFailed(hr);
        }
        ThrowIfFailed(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rs)));
    }
    RecreatePipelines(device);
    CreateResources(device);
    m_ready = true;
    Logger::Info("ReflectionProbeSystem initialized (max {} probes / capture {}px / filtered {}px x {} mips)",
                 kMaxProbes, kCaptureSize, kFilteredSize, kFilteredMips);
}

void ReflectionProbeSystem::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto make = [&](const wchar_t* cso, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + cso);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rs.Get();
        pso.CS = { bc.GetData(), bc.GetSize() };
        ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    make(L"ProbeCopyFace_CS.cso", m_psoCopy);
    make(L"ProbeDownsample_CS.cso", m_psoDown);
    make(L"PrefilterEnv_CS.cso", m_psoPrefilter);
}

void ReflectionProbeSystem::CreateResources(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto tex = [&](u32 size, u32 slices, u32 mips, D3D12_RESOURCE_STATES st, Microsoft::WRL::ComPtr<ID3D12Resource>& out)
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = size; d.Height = size;
        d.DepthOrArraySize = static_cast<u16>(slices); d.MipLevels = static_cast<u16>(mips);
        d.Format = kProbeColorFormat; d.SampleDesc = {1, 0};
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&out)));
    };
    tex(kCaptureSize, 6, kSourceMips, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_srcCube);
    tex(kFilteredSize, 6 * kMaxProbes, kFilteredMips, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_filtered);
    m_srcCube->SetName(L"ReflectionProbe.SourceCube");
    m_filtered->SetName(L"ReflectionProbe.Filtered");

    // プローブ情報（UPLOAD。フレームごとに別の領域）
    {
        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<u64>(sizeof(GpuProbe)) * kMaxProbes * kFrameCount;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc = {1, 0}; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_info)));
        void* p = nullptr;
        ThrowIfFailed(m_info->Map(0, nullptr, &p));
        m_infoMapped = static_cast<u8*>(p);
        std::memset(m_infoMapped, 0, static_cast<size_t>(bd.Width));
    }

    // 永続ディスクリプタ
    m_block = m_srvHeap->AllocateBlock(kDescCount);
    DX_ASSERT(m_block != DescriptorHeap::kInvalidIndex, "ReflectionProbeSystem descriptor allocation failed");
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = kProbeColorFormat;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MipLevels = kSourceMips;
        dev->CreateShaderResourceView(m_srcCube.Get(), &sd, m_srvHeap->GetCpuHandle(m_block + kDescCubeSrv));
        for (u32 m = 0; m < kSourceMips; ++m)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC a{};
            a.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            a.Format = kProbeColorFormat;
            a.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            a.Texture2DArray.MostDetailedMip = m;
            a.Texture2DArray.MipLevels = 1;
            a.Texture2DArray.FirstArraySlice = 0;
            a.Texture2DArray.ArraySize = 6;
            dev->CreateShaderResourceView(m_srcCube.Get(), &a, m_srvHeap->GetCpuHandle(m_block + kDescMipSrv + m));
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = kProbeColorFormat;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            u.Texture2DArray.MipSlice = m; u.Texture2DArray.FirstArraySlice = 0; u.Texture2DArray.ArraySize = 6;
            dev->CreateUnorderedAccessView(m_srcCube.Get(), nullptr, &u, m_srvHeap->GetCpuHandle(m_block + kDescMipUav + m));
        }
        for (u32 s = 0; s < kMaxProbes; ++s)
            for (u32 m = 0; m < kFilteredMips; ++m)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
                u.Format = kProbeColorFormat;
                u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                u.Texture2DArray.MipSlice = m; u.Texture2DArray.FirstArraySlice = s * 6; u.Texture2DArray.ArraySize = 6;
                dev->CreateUnorderedAccessView(m_filtered.Get(), nullptr, &u,
                    m_srvHeap->GetCpuHandle(m_block + kDescFilteredUav + s * kFilteredMips + m));
            }
    }
}

void ReflectionProbeSystem::BeginCapture(ID3D12GraphicsCommandList* cmd)
{
    Transition(cmd, m_srcCube.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void ReflectionProbeSystem::CopyFace(ID3D12GraphicsCommandList* cmd, u32 face)
{
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap->GetHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_rs.Get());
    cmd->SetPipelineState(m_psoCopy.Get());
    const u32 c[4] = { kCaptureSize, face, 0u, 0u };
    cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
    cmd->SetComputeRootDescriptorTable(1, m_srvHeap->GetGpuHandle(m_captureRT->GetSrvIndex()));
    cmd->SetComputeRootDescriptorTable(2, m_srvHeap->GetGpuHandle(m_block + kDescMipUav + 0));
    cmd->Dispatch(kCaptureSize / 8, kCaptureSize / 8, 1);
}

void ReflectionProbeSystem::EndCapture(ID3D12GraphicsCommandList* cmd, u32 slot)
{
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap->GetHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_rs.Get());

    // 1) ソースのミップ列（mip m を読んで m+1 を書く）
    cmd->SetPipelineState(m_psoDown.Get());
    for (u32 m = 0; m + 1 < kSourceMips; ++m)
    {
        for (u32 s = 0; s < 6; ++s)
            Transition(cmd, m_srcCube.Get(), m + s * kSourceMips,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const u32 dst = kCaptureSize >> (m + 1);
        const u32 c[4] = { dst, 0u, 0u, 0u };
        cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
        cmd->SetComputeRootDescriptorTable(1, m_srvHeap->GetGpuHandle(m_block + kDescMipSrv + m));
        cmd->SetComputeRootDescriptorTable(2, m_srvHeap->GetGpuHandle(m_block + kDescMipUav + m + 1));
        const u32 g = (dst + 7) / 8;
        cmd->Dispatch(g, g, 6);
    }
    for (u32 s = 0; s < 6; ++s)   // 最後のミップ
        Transition(cmd, m_srcCube.Get(), (kSourceMips - 1) + s * kSourceMips,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 2) GGX 畳み込み（既存の PrefilterEnv_CS。mip m の粗さ = m / (N-1)）→ 配列の slot
    Transition(cmd, m_filtered.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->SetPipelineState(m_psoPrefilter.Get());
    for (u32 m = 0; m < kFilteredMips; ++m)
    {
        const u32 sz = kFilteredSize >> m;
        const float rough = static_cast<float>(m) / static_cast<float>(kFilteredMips - 1);
        u32 c[4] = { sz, 0u, kCaptureSize, 0u };
        std::memcpy(&c[1], &rough, sizeof(float));
        cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
        cmd->SetComputeRootDescriptorTable(1, m_srvHeap->GetGpuHandle(m_block + kDescCubeSrv));
        cmd->SetComputeRootDescriptorTable(2, m_srvHeap->GetGpuHandle(m_block + kDescFilteredUav + slot * kFilteredMips + m));
        const u32 g = (sz + 7) / 8;
        cmd->Dispatch(g, g, 6);
    }
    Transition(cmd, m_filtered.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void ReflectionProbeSystem::WriteFrame(GraphicsDevice& device, DescriptorHeap& srvHeap, u32 tableBase, u32 frameIndex,
                                       const GpuProbe* probes, u32 count)
{
    if (!m_ready) return;
    auto* dev = device.GetDevice();
    const u32 f = frameIndex % kFrameCount;
    if (count > kMaxProbes) count = kMaxProbes;
    u8* dst = m_infoMapped + static_cast<size_t>(f) * kMaxProbes * sizeof(GpuProbe);
    if (count > 0) std::memcpy(dst, probes, static_cast<size_t>(count) * sizeof(GpuProbe));

    D3D12_SHADER_RESOURCE_VIEW_DESC cs{};
    cs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    cs.Format = kProbeColorFormat;
    cs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    cs.TextureCubeArray.MostDetailedMip = 0;
    cs.TextureCubeArray.MipLevels = kFilteredMips;
    cs.TextureCubeArray.First2DArrayFace = 0;
    cs.TextureCubeArray.NumCubes = kMaxProbes;
    dev->CreateShaderResourceView(m_filtered.Get(), &cs, srvHeap.GetCpuHandle(tableBase + 12));

    D3D12_SHADER_RESOURCE_VIEW_DESC bs{};
    bs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bs.Format = DXGI_FORMAT_UNKNOWN;
    bs.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    bs.Buffer.FirstElement = static_cast<u64>(f) * kMaxProbes;
    bs.Buffer.NumElements = kMaxProbes;
    bs.Buffer.StructureByteStride = sizeof(GpuProbe);
    dev->CreateShaderResourceView(m_info.Get(), &bs, srvHeap.GetCpuHandle(tableBase + 13));
}

void ReflectionProbeSystem::WriteTableDummy(GraphicsDevice& device, DescriptorHeap& srvHeap, u32 tableBase, u32 frameIndex)
{
    WriteFrame(device, srvHeap, tableBase, frameIndex, nullptr, 0);
}

u32 ReflectionProbeSystem::GpuBytes() const
{
    const u64 filtered = static_cast<u64>(kFilteredSize) * kFilteredSize * 8 * 6 * kMaxProbes * 4 / 3;
    const u64 src      = static_cast<u64>(kCaptureSize) * kCaptureSize * 8 * 6 * 4 / 3;
    const u64 rt       = static_cast<u64>(kCaptureSize) * kCaptureSize * (8 + 4);
    return static_cast<u32>(filtered + src + rt);
}

void ReflectionProbeSystem::Shutdown()
{
    if (m_info && m_infoMapped) { m_info->Unmap(0, nullptr); m_infoMapped = nullptr; }
    if (m_srvHeap && m_block != DescriptorHeap::kInvalidIndex)
    {
        m_srvHeap->FreeBlock(m_block, kDescCount);
        m_block = DescriptorHeap::kInvalidIndex;
    }
    for (auto& cb : m_faceCB) cb.reset();
    m_captureRT.reset();
    m_depth.Reset(); m_srcCube.Reset(); m_filtered.Reset(); m_info.Reset();
    m_psoCopy.Reset(); m_psoDown.Reset(); m_psoPrefilter.Reset(); m_rs.Reset();
    m_ready = false;
}

} // namespace dx12e
