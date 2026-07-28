#include "renderer/DdgiVolume.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>

using namespace DirectX;

namespace dx12e
{
namespace
{

// shaders/ddgi/DdgiProbeUpdate.hlsl の cbuffer DdgiCB と完全一致させること。
struct DdgiCB
{
    // DdgiConstants
    XMFLOAT3 originWS;   float rayLength;
    XMFLOAT3 spacing;    float hysteresis;
    u32      probeCountX, probeCountY, probeCountZ, frameIndex;
    float    intensity, normalBias, pad0, pad1;
    // 光源
    XMFLOAT3 sunDir;     float sunIntensity;
    XMFLOAT3 sunColor;   float pad2;
    XMFLOAT3 skyColor;   u32   skyCubeIndex;
};
static_assert(sizeof(DdgiCB) % 16 == 0, "DdgiCB は 16B 境界に揃えること");
constexpr u32 kDdgiCBNum32 = sizeof(DdgiCB) / sizeof(u32);

// irradiance アトラスのサイズ。プローブは (X*Y) 列 × Z 行 に並べる。
void AtlasSize(u32 px, u32 py, u32 pz, u32& w, u32& h)
{
    w = px * py * DdgiVolume::kProbeTile;
    h = pz * DdgiVolume::kProbeTile;
}

} // namespace

bool DdgiVolume::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap,
                            const std::wstring& shaderDir)
{
    m_srvHeap   = srvHeap;
    m_shaderDir = shaderDir;
    // ★DDGI は inline RayQuery とバインドレスの両方を使う。どちらか欠けたら諦める。
    if (!device.SupportsInlineRaytracing() || !device.SupportsDynamicResources())
    {
        Logger::Info("DDGI: 非対応（inline RT か Dynamic Resources が無い）。無効のまま続行します");
        return false;
    }
    CreateRootSignature(device);
    RecreatePipelines(device);
    return IsReady();
}

void DdgiVolume::Shutdown()
{
    if (m_srvHeap)
    {
        if (m_rayDataUav    != 0xFFFFFFFFu) m_srvHeap->FreeBlock(m_rayDataUav, 2);
        if (m_irradianceSrv != 0xFFFFFFFFu) m_srvHeap->Free(m_irradianceSrv);
    }
    m_rayDataUav = m_irradianceUav = m_irradianceSrv = 0xFFFFFFFFu;
    m_rayData.Reset();
    m_irradiance.Reset();
    m_psoTrace.Reset();
    m_psoBlend.Reset();
    m_rootSig.Reset();
    m_probesX = m_probesY = m_probesZ = 0;
    m_historyValid = false;
}

void DdgiVolume::CreateRootSignature(GraphicsDevice& device)
{
    // b0(32bit定数) + t0(TLAS root SRV) + t1(GeometryInfo root SRV) + u0/u1(root UAV)
    // DWORD: kDdgiCBNum32 + 2 + 2 + 2 + 2。専用ルートシグネチャなので PBR の 61/64 は無関係。
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = kDdgiCBNum32;

    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t0 = TLAS
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t1 = GeometryInfo
    params[2].Descriptor.ShaderRegister = 1;

    // ★u0/u1 は Texture2D なのでルート UAV にできない（ルート記述子はバッファ専用）。
    //   連続 2 スロットのディスクリプタテーブルにする。
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors     = 2;   // u0 = RayData / u1 = Irradiance
    uavRange.BaseShaderRegister = 0;
    params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &uavRange;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxLOD         = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = _countof(params);
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &samp;
    // ★バインドレス（ResourceDescriptorHeap[]）を使うので必須。
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
    const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   &serialized, &error);
    if (FAILED(hr))
    {
        if (error) Logger::Error("DDGI のルートシグネチャ: {}",
                                 static_cast<const char*>(error->GetBufferPointer()));
        return;
    }
    ThrowIfFailed(device.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
}

void DdgiVolume::RecreatePipelines(GraphicsDevice& device)
{
    if (!m_rootSig) return;
    auto make = [&](const wchar_t* name, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + name);
        if (bc.GetSize() == 0) { out.Reset(); return; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.CS = { bc.GetData(), bc.GetSize() };
        if (FAILED(device.GetDevice()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out))))
        {
            Logger::Warn("DDGI: compute の PSO を作れませんでした");
            out.Reset();
        }
    };
    make(L"DdgiTrace_CS.cso", m_psoTrace);
    make(L"DdgiBlend_CS.cso", m_psoBlend);
}

bool DdgiVolume::EnsureResources(GraphicsDevice& device, const DdgiSettings& s)
{
    const u32 px = static_cast<u32>(std::clamp(s.probeCountX, 1, 32));
    const u32 py = static_cast<u32>(std::clamp(s.probeCountY, 1, 32));
    const u32 pz = static_cast<u32>(std::clamp(s.probeCountZ, 1, 32));
    const u32 total = px * py * pz;
    if (total == 0 || total > kMaxProbes) return false;
    if (m_rayData && m_probesX == px && m_probesY == py && m_probesZ == pz) return true;

    auto* dev = device.GetDevice();
    auto makeTex = [&](u32 w, u32 h, Microsoft::WRL::ComPtr<ID3D12Resource>& out) -> bool
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = w;
        desc.Height           = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc       = {1, 0};
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    };

    u32 aw = 0, ah = 0;
    AtlasSize(px, py, pz, aw, ah);
    Microsoft::WRL::ComPtr<ID3D12Resource> rayData, irradiance;
    if (!makeTex(kRaysPerProbe, total, rayData) || !makeTex(aw, ah, irradiance))
    {
        Logger::Warn("DDGI: プローブ用テクスチャを確保できません（{} プローブ）", total);
        return false;
    }
    m_rayData    = std::move(rayData);
    m_irradiance = std::move(irradiance);
    // 作りたてはどちらも UNORDERED_ACCESS。作り直したら追跡中のステートも戻す。
    m_irradianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // ディスクリプタは初回だけ取る（格子が変わっても同じ index へ張り直す）。
    // ★u0/u1 はディスクリプタテーブルなので**連続**でなければならない。
    if (m_rayDataUav == 0xFFFFFFFFu)
    {
        m_rayDataUav    = m_srvHeap->AllocateBlock(2);
        m_irradianceUav = m_rayDataUav + 1;
        m_irradianceSrv = m_srvHeap->AllocateIndex();
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(m_rayData.Get(), nullptr, &uav,
                                   m_srvHeap->GetCpuHandle(m_rayDataUav));
    dev->CreateUnorderedAccessView(m_irradiance.Get(), nullptr, &uav,
                                   m_srvHeap->GetCpuHandle(m_irradianceUav));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    dev->CreateShaderResourceView(m_irradiance.Get(), &srv,
                                  m_srvHeap->GetCpuHandle(m_irradianceSrv));

    m_probesX = px; m_probesY = py; m_probesZ = pz;
    m_historyValid = false;   // 格子が変わった＝履歴の意味が変わる
    m_stats.probes = total;
    m_stats.bytes  = static_cast<u64>(kRaysPerProbe) * total * 8
                   + static_cast<u64>(aw) * ah * 8;
    Logger::Info("DDGI: プローブ {}x{}x{} = {} 個 / アトラス {}x{} / {:.2f} MB",
                 px, py, pz, total, aw, ah, static_cast<double>(m_stats.bytes) / (1024.0 * 1024.0));
    return true;
}

u32 DdgiVolume::GetIrradianceSrvIndex() const
{
    return m_irradiance ? m_irradianceSrv : 0xFFFFFFFFu;
}

bool DdgiVolume::WriteIrradianceSrv(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    if (!m_irradiance || dst.ptr == 0) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    device.GetDevice()->CreateShaderResourceView(m_irradiance.Get(), &srv, dst);
    return true;
}

void DdgiVolume::Update(ID3D12GraphicsCommandList* cmd, GraphicsDevice& device,
                        const DdgiSettings& s, const UpdateDesc& d)
{
    m_stats.raysCast = 0;
    if (!cmd || !IsReady() || !s.enabled || d.tlas == 0 || d.geometryInfo == 0)
        return;
    // ★GeometryInfo が無いフレームは走らせない。ヒット点のアルベドが引けず真っ黒になる。

    // 遅延確保: DDGI を実際に ON にしたフレームで初めてテクスチャを取る
    //（OFF のプロジェクトでは VRAM もディスクリプタも 1 バイトも消費しない）。
    if (!EnsureResources(device, s))
        return;

    const u32 total = m_probesX * m_probesY * m_probesZ;
    if (total == 0) return;

    DdgiCB cb{};
    cb.originWS   = {s.originX, s.originY, s.originZ};
    cb.rayLength  = std::max(s.rayLength, 0.1f);
    cb.spacing    = {s.spacing, s.spacing, s.spacing};
    cb.hysteresis = m_historyValid ? std::clamp(s.hysteresis, 0.0f, 0.995f) : 0.0f;
    cb.probeCountX = m_probesX; cb.probeCountY = m_probesY; cb.probeCountZ = m_probesZ;
    cb.frameIndex  = d.frameIndex;
    cb.intensity   = std::max(s.intensity, 0.0f);
    cb.normalBias  = std::max(s.normalBias, 0.0f);
    cb.sunDir      = d.sunDir;
    cb.sunIntensity = d.sunIntensity;
    cb.sunColor    = d.sunColor;
    cb.skyColor    = d.skyColor;
    cb.skyCubeIndex = d.skyCubeSrvIndex;

    // 段階1 でフォワード PS が t22 から読むようになったので、書く前に UAV へ戻す。
    if (m_irradianceState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_irradiance.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = m_irradianceState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cmd->ResourceBarrier(1, &b);
        m_irradianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRoot32BitConstants(0, kDdgiCBNum32, &cb, 0);
    cmd->SetComputeRootShaderResourceView(1, d.tlas);
    cmd->SetComputeRootShaderResourceView(2, d.geometryInfo);
    cmd->SetComputeRootDescriptorTable(3, m_srvHeap->GetGpuHandle(m_rayDataUav));

    // ---- レイトレ（1 スレッド = 1 レイ）----
    cmd->SetPipelineState(m_psoTrace.Get());
    cmd->Dispatch(1, total, 1);   // x は numthreads が kRaysPerProbe なので 1 グループ

    // RayData の書き込みを Blend が読む前に直列化する。
    D3D12_RESOURCE_BARRIER uavB{};
    uavB.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavB.UAV.pResource = m_rayData.Get();
    cmd->ResourceBarrier(1, &uavB);

    // ---- ブレンド（1 グループ = 1 プローブ / 1 スレッド = 1 テクセル）----
    cmd->SetPipelineState(m_psoBlend.Get());
    cmd->Dispatch(total, 1, 1);

    // フォワードパスが PS から読めるようにしておく（段階1）。
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_irradiance.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(1, &b);
        m_irradianceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    m_stats.raysCast = total * kRaysPerProbe;
    m_historyValid   = true;
}

} // namespace dx12e
