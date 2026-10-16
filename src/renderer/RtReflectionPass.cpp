#include "renderer/RtReflectionPass.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "graphics/Buffer.h"
#include "graphics/FrameResources.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>
#include <cstring>

namespace dx12e
{
using namespace DirectX;

namespace
{
// shaders/raytracing/RtReflection.hlsl の cbuffer ReflParams とバイト単位で一致させること。
struct ReflParamsCB
{
    XMFLOAT4X4 invViewProj;   // 転置済み
    XMFLOAT4X4 invProj;       // 転置済み
    XMFLOAT4   r[6];          // gR0..gR5
    u32        i[3][4];       // gI0..gI2
};
static_assert(sizeof(ReflParamsCB) == 128 + 96 + 48, "layout mismatch with RtReflection.hlsl");
constexpr u32 kHitCbBytes = 176;   // DdgiVolume::kHitCbBytes
constexpr u32 kNoSrv = 0xFFFFFFFFu;
constexpr u32 kNone = 99u;         // draw() の「この入力は使わない」
}

void RtReflectionPass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                                  u32 width, u32 height, const std::wstring& shaderDir)
{
    m_device = &device;
    m_rtvHeap = rtvHeap;
    m_srvHeap = srvHeap;
    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;
    m_shaderDir = shaderDir;
    CreateRootSignature(device);
    RecreatePipelines(device);
    m_hitCB = std::make_unique<ConstantBuffer>();
    m_hitCB->Initialize(device, kHitCbBytes, FrameResources::kFrameCount);
    m_paramCB = std::make_unique<ConstantBuffer>();
    m_paramCB->Initialize(device, sizeof(ReflParamsCB), FrameResources::kFrameCount * PCount);
    Logger::Info("RtReflectionPass initialized（RT 反射。テクスチャは初回使用時に確保）");
}

void RtReflectionPass::CreateRootSignature(GraphicsDevice& device)
{
    if (!device.SupportsDynamicResources()) return;   // バインドレスが無ければ作らない（IsReady が false のまま）
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;   // b0 = DdgiCB
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;   // b1 = ReflParams
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t0 = TLAS
    params[2].Descriptor.ShaderRegister = 0;
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t1 = GeometryInfo
    params[3].Descriptor.ShaderRegister = 1;
    params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp[2]{};
    samp[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;     // s0 = LINEAR / WRAP（アルベド / α）
    samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp[0].MaxLOD           = D3D12_FLOAT32_MAX;
    samp[0].ShaderRegister   = 0;
    samp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samp[1].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;     // s1 = LINEAR / CLAMP（履歴の再投影）
    samp[1].AddressU = samp[1].AddressV = samp[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp[1].MaxLOD           = D3D12_FLOAT32_MAX;
    samp[1].ShaderRegister   = 1;
    samp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = _countof(params);
    desc.pParameters       = params;
    desc.NumStaticSamplers = 2;
    desc.pStaticSamplers   = samp;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
    const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (FAILED(hr))
    {
        if (error) Logger::Error("RT 反射のルートシグネチャ: {}", static_cast<const char*>(error->GetBufferPointer()));
        return;
    }
    ThrowIfFailed(device.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
}

void RtReflectionPass::RecreatePipelines(GraphicsDevice& device)
{
    if (!m_rootSig) return;
    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"Rt_VS.cso");
    auto make = [&](const wchar_t* psName, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + psName);
        if (ps.GetSize() == 0 || vs.GetSize() == 0) { out.Reset(); return; }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.VS = { vs.GetData(), vs.GetSize() };
        pso.PS = { ps.GetData(), ps.GetSize() };
        pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.DepthStencilState.DepthEnable   = FALSE;
        pso.SampleMask            = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets      = 1;
        pso.RTVFormats[0]         = kFormat;
        pso.SampleDesc            = { 1, 0 };
        if (FAILED(device.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out))))
        {
            Logger::Warn("RT 反射: PSO を作れませんでした");
            out.Reset();
        }
    };
    make(L"RtReflTrace_PS.cso",     m_psoTrace);
    make(L"RtReflTemporal_PS.cso",  m_psoTemporal);
    make(L"RtReflSpatial_PS.cso",   m_psoSpatial);
    make(L"RtReflComposite_PS.cso", m_psoComposite);
}

bool RtReflectionPass::EnsureTargets(GraphicsDevice& device, bool half)
{
    const u32 tw = half ? (m_width + 1) / 2 : m_width;
    const u32 th = half ? (m_height + 1) / 2 : m_height;
    const float zero[4] = {0, 0, 0, 0};
    if (!m_rt[0])
    {
        for (u32 i = 0; i < 5; ++i)
        {
            m_rt[i] = std::make_unique<RenderTarget>();
            const bool full = (i == 4);
            m_rt[i]->Initialize(device, m_rtvHeap, m_srvHeap, full ? m_width : tw, full ? m_height : th, kFormat, zero);
            if (m_rt[i]->GetSrvIndex() == DescriptorHeap::kInvalidIndex) { m_rt[0].reset(); return false; }
            m_state[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        m_tw = tw; m_th = th; m_half = half;
        m_histValid = false;
        return true;
    }
    if (tw != m_tw || th != m_th || m_rt[4]->GetWidth() != m_width || m_rt[4]->GetHeight() != m_height)
    {
        for (u32 i = 0; i < 5; ++i)
        {
            const bool full = (i == 4);
            m_rt[i]->Resize(device, full ? m_width : tw, full ? m_height : th);
            m_state[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        m_tw = tw; m_th = th; m_half = half;
        m_histValid = false;
    }
    return true;
}

void RtReflectionPass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width = width; m_height = height;
    if (m_rt[0]) EnsureTargets(device, m_half);
    m_histValid = false;
}

u64 RtReflectionPass::GetBytes() const
{
    if (!m_rt[0]) return 0;
    return static_cast<u64>(m_tw) * m_th * 8 * 4 + static_cast<u64>(m_width) * m_height * 8;
}

u32 RtReflectionPass::Run(ID3D12GraphicsCommandList* cmd, const Desc& d, const RtSettings& s)
{
    if (!m_device || !IsReady() || !m_rootSig || d.tlas == 0 || d.geometryInfo == 0 || !d.hitCb)
        return DescriptorHeap::kInvalidIndex;
    if (d.depthSrv == kNoSrv || d.gbufferSrv == kNoSrv || d.velocitySrv == kNoSrv || d.ssrSrv == kNoSrv)
        return DescriptorHeap::kInvalidIndex;
    if (!EnsureTargets(*m_device, s.reflectionHalfRes)) return DescriptorHeap::kInvalidIndex;

    const u32 ring = d.frameIndex % FrameResources::kFrameCount;
    m_hitCB->Update(d.hitCb, kHitCbBytes, ring);

    const u32 histRead  = 1u + (1u - m_histWrite);   // rt[1] / rt[2]
    const u32 histWrite = 1u + m_histWrite;
    const u32 scale = m_half ? 2u : 1u;
    const XMMATRIX viewProj = d.view * d.proj;

    ReflParamsCB cb{};
    XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProj)));
    XMStoreFloat4x4(&cb.invProj,     XMMatrixTranspose(XMMatrixInverse(nullptr, d.proj)));
    const float rmax = std::clamp(s.reflectionMaxRoughness, 0.05f, 1.0f);
    cb.r[0] = {d.cameraPos.x, d.cameraPos.y, d.cameraPos.z, d.zNear};
    cb.r[1] = {rmax, std::max(s.reflectionMaxDistance, 1.0f), 0.02f, std::clamp(s.reflectionIntensity, 0.0f, 1.0f)};
    cb.r[2] = {static_cast<float>(d.vpLeft), static_cast<float>(d.vpTop), static_cast<float>(d.vpW), static_cast<float>(d.vpH)};
    cb.r[3] = {static_cast<float>(m_tw), static_cast<float>(m_th), static_cast<float>(m_width), static_cast<float>(m_height)};
    // SSR が 0.98 以上当たっている画素は撃たない（SSR → RT の over 合成で RT の寄与が 2% 未満のため）
    cb.r[4] = {static_cast<float>(d.seq & 0xFFFFu), 0.98f, m_histValid ? 1.0f : 0.0f, d.gbufferValid ? 1.0f : 0.0f};
    cb.r[5] = {d.bounceScale, d.skyScale, d.giNew ? 1.0f : 0.0f, m_half ? 1.5f : 2.5f};   // 空間フィルタの基準半径(px)
    cb.i[0][0] = d.depthSrv; cb.i[0][1] = d.gbufferSrv; cb.i[0][2] = d.velocitySrv; cb.i[0][3] = d.ssrSrv;
    cb.i[1][3] = d.probeDataSrv;
    cb.i[2][0] = d.gridUav;  cb.i[2][1] = scale;
    cb.i[2][2] = d.skyPrefilteredSrv; cb.i[2][3] = d.skyMaxMip;

    if (m_srvHeap && m_srvHeap->GetHeap())
    {
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap->GetHeap() };
        cmd->SetDescriptorHeaps(1, heaps);   // ★ルートシグネチャを張る前に（Dynamic Resources の順序制約）
    }
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->SetGraphicsRootConstantBufferView(0, m_hitCB->GetGpuAddress(ring));
    cmd->SetGraphicsRootShaderResourceView(2, d.tlas);
    cmd->SetGraphicsRootShaderResourceView(3, d.geometryInfo);

    auto transition = [&](u32 idx, D3D12_RESOURCE_STATES next)
    {
        if (m_state[idx] == next) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_rt[idx]->GetResource();
        b.Transition.StateBefore = m_state[idx];
        b.Transition.StateAfter  = next;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        m_state[idx] = next;
    };

    // 1 パスぶん: 出力 outIdx へ PSO で 1 枚描く。b1 の i[1]（trace / history / filtered の SRV）は呼び出しごとに差し替える。
    auto draw = [&](Pass pass, ID3D12PipelineState* pso, u32 outIdx, u32 inTrace, u32 inHist, u32 inFiltered)
    {
        ReflParamsCB c = cb;
        c.i[1][0] = (inTrace    != kNone) ? m_rt[inTrace]->GetSrvIndex()    : kNoSrv;
        c.i[1][1] = (inHist     != kNone) ? m_rt[inHist]->GetSrvIndex()     : kNoSrv;
        c.i[1][2] = (inFiltered != kNone) ? m_rt[inFiltered]->GetSrvIndex() : kNoSrv;
        const u32 slot = ring * PCount + pass;
        m_paramCB->Update(&c, sizeof(c), slot);
        cmd->SetGraphicsRootConstantBufferView(1, m_paramCB->GetGpuAddress(slot));

        transition(outIdx, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const u32 w = m_rt[outIdx]->GetWidth(), h = m_rt[outIdx]->GetHeight();
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        auto rtv = m_rt[outIdx]->GetRtv();
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd->SetPipelineState(pso);
        cmd->DrawInstanced(3, 1, 0, 0);
        transition(outIdx, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };

    // 履歴の読み側は常に SRV にしておく（作りたては RENDER_TARGET）
    transition(histRead, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    draw(PTrace,     m_psoTrace.Get(),     0,         kNone, kNone,    kNone);
    draw(PTemporal,  m_psoTemporal.Get(),  histWrite, 0,     histRead, kNone);
    draw(PSpatial,   m_psoSpatial.Get(),   3,         0,     histWrite, kNone);
    draw(PComposite, m_psoComposite.Get(), 4,         0,     kNone,    3);

    m_histWrite = 1u - m_histWrite;
    m_histValid = true;
    return m_rt[4]->GetSrvIndex();
}

} // namespace dx12e
