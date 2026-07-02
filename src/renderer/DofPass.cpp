#include "renderer/DofPass.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "graphics/CommandList.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>

namespace dx12e
{
// シーンと同フォーマット（後段がこの RT を「シーン」として扱うため）
static constexpr DXGI_FORMAT kDofFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// HLSL の DofCB と一致（3 x float4 = 12 DWORD、ルート定数）
struct DofCB
{
    float rectP[4];
    float focus[4];
    float texelP[4];
};
static_assert(sizeof(DofCB) == 12 * sizeof(float), "DofCB must be 12 DWORDs");
static constexpr UINT kDofCBNum32 = 12;

void DofPass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                         u32 width, u32 height, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;

    // --- Root Signature: t0/t1/t2(SRV table ×3) + b0(12 DWORD) + s0 ---
    {
        D3D12_DESCRIPTOR_RANGE r0{}, r1{}, r2{};
        r0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r0.NumDescriptors = 1; r0.BaseShaderRegister = 0;
        r1 = r0; r1.BaseShaderRegister = 1;
        r2 = r0; r2.BaseShaderRegister = 2;

        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {1, &r0};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &r1};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &r2};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[3].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[3].Constants.ShaderRegister = 0;
        params[3].Constants.Num32BitValues = kDofCBNum32;
        params[3].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 4;
        desc.pParameters       = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    // --- PSO ×3（VS は FSTriVS 流用）---
    {
        auto vs    = ShaderCompiler::LoadFromFile(shaderDir + L"PostProcess_VS.cso");
        auto psCoc = ShaderCompiler::LoadFromFile(shaderDir + L"DofCoc_PS.cso");
        auto psGat = ShaderCompiler::LoadFromFile(shaderDir + L"DofGather_PS.cso");
        auto psCmp = ShaderCompiler::LoadFromFile(shaderDir + L"DofComposite_PS.cso");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.VS = { vs.GetData(), vs.GetSize() };
        pso.PS = { psCoc.GetData(), psCoc.GetSize() };
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
        pso.RTVFormats[0]         = kDofFormat;
        pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc            = { 1, 0 };
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoCoc)));

        pso.PS = { psGat.GetData(), psGat.GetSize() };
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoGather)));

        pso.PS = { psCmp.GetData(), psCmp.GetSize() };
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoComposite)));
    }

    const float clearBlack[4] = {0, 0, 0, 1};
    const u32 hw = (std::max)(1u, m_width / 2), hh = (std::max)(1u, m_height / 2);
    m_halfCoc = std::make_unique<RenderTarget>();
    m_halfCoc->Initialize(device, rtvHeap, srvHeap, hw, hh, kDofFormat, clearBlack);
    m_halfBlur = std::make_unique<RenderTarget>();
    m_halfBlur->Initialize(device, rtvHeap, srvHeap, hw, hh, kDofFormat, clearBlack);
    m_outRT = std::make_unique<RenderTarget>();
    m_outRT->Initialize(device, rtvHeap, srvHeap, m_width, m_height, kDofFormat, clearBlack);

    Logger::Info("DofPass initialized");
}

void DofPass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width  = width;
    m_height = height;
    const u32 hw = (std::max)(1u, width / 2), hh = (std::max)(1u, height / 2);
    if (m_halfCoc)  m_halfCoc->Resize(device, hw, hh);
    if (m_halfBlur) m_halfBlur->Resize(device, hw, hh);
    if (m_outRT)    m_outRT->Resize(device, width, height);
}

u32 DofPass::Apply(CommandList& cmd, DescriptorHeap* srvHeap,
                   D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
                   D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
                   float uvOfsX, float uvOfsY, float uvScaleX, float uvScaleY,
                   u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH,
                   float projA, float projB,
                   const PostProcessSettings& s)
{
    if (!m_psoComposite || !m_outRT) return DescriptorHeap::kInvalidIndex;
    auto* native = cmd.GetNative();

    const u32 halfW = (std::max)(1u, m_width / 2), halfH = (std::max)(1u, m_height / 2);

    DofCB cb{};
    cb.rectP[0] = uvOfsX;   cb.rectP[1] = uvOfsY;
    cb.rectP[2] = uvScaleX; cb.rectP[3] = uvScaleY;
    cb.focus[0] = (std::max)(s.dofFocusDist, 0.01f);
    cb.focus[1] = 1.0f / (std::max)(s.dofFocusRange, 0.05f);
    cb.focus[2] = (std::max)(s.dofBlurSize, 1.0f) * 0.5f;   // 半解像度基準の px
    cb.texelP[0] = 1.0f / static_cast<float>(halfW);
    cb.texelP[1] = 1.0f / static_cast<float>(halfH);
    cb.texelP[2] = projA;
    cb.texelP[3] = projB;

    native->SetGraphicsRootSignature(m_rootSig.Get());
    native->SetGraphicsRoot32BitConstants(3, kDofCBNum32, &cb, 0);
    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->IASetVertexBuffers(0, 0, nullptr);
    native->IASetIndexBuffer(nullptr);

    const u32 hl = vpLeft / 2, ht = vpTop / 2;
    const u32 hw = (std::max)(1u, vpW / 2), hh = (std::max)(1u, vpH / 2);

    // ---- パス1: 半解像度へ 色+CoC ----
    m_halfCoc->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_halfCoc->GetRtv();
        native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd.SetViewportAndScissor(hl, ht, hw, hh);
        native->SetPipelineState(m_psoCoc.Get());
        native->SetGraphicsRootDescriptorTable(0, sceneSrvGpu);
        native->SetGraphicsRootDescriptorTable(1, sceneSrvGpu);   // 未使用スロットもバインド
        native->SetGraphicsRootDescriptorTable(2, depthSrvGpu);
        native->DrawInstanced(3, 1, 0, 0);
    }

    // ---- パス2: gather ----
    m_halfCoc->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_halfBlur->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_halfBlur->GetRtv();
        native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd.SetViewportAndScissor(hl, ht, hw, hh);
        native->SetPipelineState(m_psoGather.Get());
        native->SetGraphicsRootDescriptorTable(0, srvHeap->GetGpuHandle(m_halfCoc->GetSrvIndex()));
        native->SetGraphicsRootDescriptorTable(1, sceneSrvGpu);
        native->SetGraphicsRootDescriptorTable(2, depthSrvGpu);
        native->DrawInstanced(3, 1, 0, 0);
    }

    // ---- パス3: フル解像度合成 ----
    m_halfBlur->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_outRT->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_outRT->GetRtv();
        native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd.SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);
        native->SetPipelineState(m_psoComposite.Get());
        native->SetGraphicsRootDescriptorTable(0, sceneSrvGpu);
        native->SetGraphicsRootDescriptorTable(1, srvHeap->GetGpuHandle(m_halfBlur->GetSrvIndex()));
        native->SetGraphicsRootDescriptorTable(2, depthSrvGpu);
        native->DrawInstanced(3, 1, 0, 0);
    }

    // 後段（自動露出CS/ブルーム/uber）がシーンとして読むので複合読取状態へ
    m_outRT->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                           | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return m_outRT->GetSrvIndex();
}

} // namespace dx12e
