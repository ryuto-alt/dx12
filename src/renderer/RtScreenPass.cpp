#include "renderer/RtScreenPass.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "graphics/Buffer.h"
#include "graphics/FrameResources.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>

namespace dx12e
{
using namespace DirectX;

// shaders/raytracing/RtCommon.hlsli の cbuffer RtParams とバイト単位で一致させること。
struct RtParamsCB
{
    XMFLOAT4X4 invViewProj;   //  64B（転置済み: mul(row, M)）
    XMFLOAT4X4 invProj;       //  64B
    XMFLOAT4   p[8];          // 128B
};
static_assert(sizeof(RtParamsCB) == 256, "layout mismatch with RtCommon.hlsli");

void RtScreenPass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                             u32 width, u32 height, const std::wstring& shaderDir)
{
    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;

    CreateRootSignature(device);
    m_shaderDir = shaderDir;
    RecreatePipelines(device);

    // 影 / AO は「1 = 遮蔽なし」なのでクリアは白。デバッグはミス(-1)で埋める。
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float miss[4]  = {-1.0f, -1.0f, 0.0f, 0.0f};
    for (u32 i = 0; i < PassCount; ++i)
    {
        m_rt[i] = std::make_unique<RenderTarget>();
        m_rt[i]->Initialize(device, rtvHeap, srvHeap, m_width, m_height,
                            (i == PassDebug) ? kDebugFormat : kMaskFormat,
                            (i == PassDebug) ? miss : white);
        m_rtState[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    m_paramCB = std::make_unique<ConstantBuffer>();
    m_paramCB->Initialize(device, sizeof(RtParamsCB), FrameResources::kFrameCount * PassCount);

    Logger::Info("RtScreenPass initialized ({}x{}) — RT影 / RT-AO / RTデバッグ", m_width, m_height);
}

void RtScreenPass::CreateRootSignature(GraphicsDevice& device)
{
    // t0 = 深度（ディスクリプタテーブル）/ t1 = TLAS（★ルート SRV）/ b0 = パラメータ。
    // TLAS をルート SRV にすると、毎フレーム TLAS バッファが作り直されても
    // ディスクリプタを書き直さずに済む（Microsoft の DXR サンプルと同じ方式）。
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 1;
    srvRange.BaseShaderRegister = 0;   // t0

    D3D12_DESCRIPTOR_RANGE ssaoRange{};
    ssaoRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ssaoRange.NumDescriptors     = 1;
    ssaoRange.BaseShaderRegister = 2;  // t2 = SSAO（RT-AO と min 合成するとき用）

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 1;   // t1 = TLAS
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 0;   // b0
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &ssaoRange;
    params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 4;
    desc.pParameters   = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ThrowIfFailed(device.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
}

void RtScreenPass::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"Rt_VS.cso");

    auto make = [&](const wchar_t* psName, DXGI_FORMAT rtFormat,
                    Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + psName);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.VS = { vs.GetData(), vs.GetSize() };
        pso.PS = { ps.GetData(), ps.GetSize() };
        pso.InputLayout = { nullptr, 0 };
        pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.DepthStencilState.DepthEnable   = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.SampleMask            = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets      = 1;
        pso.RTVFormats[0]         = rtFormat;
        pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc            = { 1, 0 };
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out)));
    };

    make(L"RtShadow_PS.cso", kMaskFormat,  m_psoShadow);
    make(L"RtAo_PS.cso",     kMaskFormat,  m_psoAo);
    make(L"RtDebug_PS.cso",  kDebugFormat, m_psoDebug);
}

void RtScreenPass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width  = width;
    m_height = height;
    for (u32 i = 0; i < PassCount; ++i)
    {
        if (m_rt[i]) m_rt[i]->Resize(device, width, height);
        m_rtState[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
}

u32 RtScreenPass::Run(ID3D12GraphicsCommandList* cmd, const GenerateDesc& d, const RtSettings& s,
                      PassIndex pass)
{
    if (!m_rootSig || !m_rt[pass] || d.tlas == 0) return DescriptorHeap::kInvalidIndex;

    ID3D12PipelineState* pso = (pass == PassShadow) ? m_psoShadow.Get()
                             : (pass == PassAo)     ? m_psoAo.Get()
                                                    : m_psoDebug.Get();
    if (!pso) return DescriptorHeap::kInvalidIndex;

    const XMMATRIX viewProj = d.view * d.proj;

    RtParamsCB cb{};
    // HLSL は mul(row, M) なので転置して渡す（SSAO / ContactShadow と同じ運用）。
    XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProj)));
    XMStoreFloat4x4(&cb.invProj,     XMMatrixTranspose(XMMatrixInverse(nullptr, d.proj)));

    cb.p[0] = {d.cameraPos.x, d.cameraPos.y, d.cameraPos.z, d.zNear};
    {
        XMFLOAT3 l{};
        XMStoreFloat3(&l, XMVector3Normalize(XMLoadFloat3(&d.lightDir)));
        // 太陽の「角半径」の tan。設定は角直径(度)なので半分にしてラジアンへ。
        const float halfRad = std::max(0.0f, s.shadowSunAngle) * 0.5f * 3.14159265f / 180.0f;
        cb.p[1] = {l.x, l.y, l.z, std::tan(halfRad)};
    }
    cb.p[2] = {static_cast<float>(d.vpLeft), static_cast<float>(d.vpTop),
               static_cast<float>(d.vpW),    static_cast<float>(d.vpH)};
    cb.p[3] = {static_cast<float>(m_width), static_cast<float>(m_height),
               1.0f / static_cast<float>(m_width), 1.0f / static_cast<float>(m_height)};
    cb.p[4] = {std::max(s.shadowNormalBias, 0.0f),
               (pass == PassAo) ? 0.0f : std::max(s.shadowMaxDistance, 0.0f),
               (pass == PassAo) ? std::clamp(s.aoIntensity, 0.0f, 1.0f)
                                : std::clamp(s.shadowIntensity, 0.0f, 1.0f),
               d.frameJitter};
    cb.p[5] = {std::max(s.aoRadius, 0.01f), static_cast<float>(std::clamp(s.aoRayCount, 1, 8)),
               std::max(s.aoPower, 0.01f), d.debugRange};
    cb.p[6] = {d.zFar, (s.aoCombineWithSsao && d.ssaoValid) ? 1.0f : 0.0f, 0.0f, 0.0f};
    cb.p[7] = {0.0f, 0.0f, 0.0f, 0.0f};

    const u32 cbSlot = d.frameIndex % FrameResources::kFrameCount * PassCount + pass;
    m_paramCB->Update(&cb, sizeof(cb), cbSlot);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    D3D12_RECT     sc{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};

    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    auto transition = [&](D3D12_RESOURCE_STATES next)
    {
        if (m_rtState[pass] == next) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_rt[pass]->GetResource();
        b.Transition.StateBefore = m_rtState[pass];
        b.Transition.StateAfter   = next;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        m_rtState[pass] = next;
    };

    transition(D3D12_RESOURCE_STATE_RENDER_TARGET);

    auto rtv = m_rt[pass]->GetRtv();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd->SetPipelineState(pso);
    cmd->SetGraphicsRootDescriptorTable(0, d.depthSrv);
    cmd->SetGraphicsRootShaderResourceView(1, d.tlas);   // TLAS はルート SRV（VA 直指定）
    cmd->SetGraphicsRootConstantBufferView(2, m_paramCB->GetGpuAddress(cbSlot));
    cmd->SetGraphicsRootDescriptorTable(3, d.ssaoSrv);
    cmd->DrawInstanced(3, 1, 0, 0);

    transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    return m_rt[pass]->GetSrvIndex();
}

u32 RtScreenPass::GenerateShadow(ID3D12GraphicsCommandList* cmd, const GenerateDesc& d, const RtSettings& s)
{
    return Run(cmd, d, s, PassShadow);
}

u32 RtScreenPass::GenerateAo(ID3D12GraphicsCommandList* cmd, const GenerateDesc& d, const RtSettings& s)
{
    return Run(cmd, d, s, PassAo);
}

u32 RtScreenPass::GenerateDebug(ID3D12GraphicsCommandList* cmd, const GenerateDesc& d)
{
    static const RtSettings kDefault{};
    return Run(cmd, d, kDefault, PassDebug);
}

} // namespace dx12e
