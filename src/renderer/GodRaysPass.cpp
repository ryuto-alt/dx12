#include "renderer/GodRaysPass.h"
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
static constexpr DXGI_FORMAT kGRFormat = DXGI_FORMAT_R11G11B10_FLOAT;

// HLSL の GRCB と一致（4 x float4 = 16 DWORD、ルート定数）
struct GRCB
{
    float rectP[4];
    float sunP[4];
    float sunCol[4];
    float marchP[4];
};
static_assert(sizeof(GRCB) == 16 * sizeof(float), "GRCB must be 16 DWORDs");
static constexpr UINT kGRCBNum32 = 16;

void GodRaysPass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                             u32 width, u32 height, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;

    // --- Root Signature: t0(SRV table) + b0(16 DWORD) + s0(linear clamp) ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.Num32BitValues = kGRCBNum32;
        params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 2;
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

    // --- PSO ×2（VS は PostProcess の FSTriVS を流用）---
    m_shaderDir = shaderDir;
    RecreatePipelines(device);

    // --- 半解像度 RT ×2 ---
    const float clearBlack[4] = {0, 0, 0, 1};
    const u32 hw = (std::max)(1u, m_width / 2), hh = (std::max)(1u, m_height / 2);
    m_maskRT = std::make_unique<RenderTarget>();
    m_maskRT->Initialize(device, rtvHeap, srvHeap, hw, hh, kGRFormat, clearBlack);
    m_shaftRT = std::make_unique<RenderTarget>();
    m_shaftRT->Initialize(device, rtvHeap, srvHeap, hw, hh, kGRFormat, clearBlack);

    Logger::Info("GodRaysPass initialized");
}

void GodRaysPass::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto vs     = ShaderCompiler::LoadFromFile(m_shaderDir + L"PostProcess_VS.cso");
    auto psMask = ShaderCompiler::LoadFromFile(m_shaderDir + L"GodRaysMask_PS.cso");
    auto psBlur = ShaderCompiler::LoadFromFile(m_shaderDir + L"GodRaysBlur_PS.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.Get();
    pso.VS = { vs.GetData(), vs.GetSize() };
    pso.PS = { psMask.GetData(), psMask.GetSize() };
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
    pso.RTVFormats[0]         = kGRFormat;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc            = { 1, 0 };
    ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoMask)));

    pso.PS = { psBlur.GetData(), psBlur.GetSize() };
    ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoBlur)));
}

void GodRaysPass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width  = width;
    m_height = height;
    const u32 hw = (std::max)(1u, width / 2), hh = (std::max)(1u, height / 2);
    if (m_maskRT)  m_maskRT->Resize(device, hw, hh);
    if (m_shaftRT) m_shaftRT->Resize(device, hw, hh);
}

u32 GodRaysPass::Generate(CommandList& cmd, DescriptorHeap* srvHeap,
                          D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
                          u32 sceneW, u32 sceneH,
                          float sunUvX, float sunUvY, float fade,
                          const DirectX::XMFLOAT3& sunColor,
                          const PostProcessSettings& s)
{
    if (!m_psoMask || !m_maskRT) return DescriptorHeap::kInvalidIndex;
    auto* native = cmd.GetNative();

    GRCB cb{};
    cb.rectP[0] = 0.0f; cb.rectP[1] = 0.0f;   // ★#16: シーンは RT 全面
    cb.rectP[2] = 1.0f; cb.rectP[3] = 1.0f;
    cb.sunP[0]  = sunUvX;   cb.sunP[1]  = sunUvY;
    cb.sunP[2]  = fade;     cb.sunP[3]  = s.grIntensity;
    cb.sunCol[0] = sunColor.x; cb.sunCol[1] = sunColor.y; cb.sunCol[2] = sunColor.z;
    cb.sunCol[3] = (std::min)((std::max)(s.grDensity, 0.05f), 1.0f);
    cb.marchP[0] = (std::min)((std::max)(s.grDecay, 0.5f), 0.999f);
    cb.marchP[1] = (sceneH > 0) ? static_cast<float>(sceneW) / static_cast<float>(sceneH) : 1.0f;

    native->SetGraphicsRootSignature(m_rootSig.Get());
    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->IASetVertexBuffers(0, 0, nullptr);
    native->IASetIndexBuffer(nullptr);

    // ★半解像度 RT の実サイズをそのまま使う（Resize と同じ式なので必ず一致する）。
    //   かつては vpLeft/2, vpW/2 と整数半減していてオフバイワンの温床だった（#16 で解消）。
    const u32 hw = (std::max)(1u, m_width / 2), hh = (std::max)(1u, m_height / 2);

    // ---- パス1: 空マスク ----
    m_maskRT->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_maskRT->GetRtv();
        native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd.SetViewportAndScissor(hw, hh);
        native->SetPipelineState(m_psoMask.Get());
        native->SetGraphicsRootDescriptorTable(0, depthSrvGpu);
        native->SetGraphicsRoot32BitConstants(1, kGRCBNum32, &cb, 0);
        native->DrawInstanced(3, 1, 0, 0);
    }

    // ---- パス2: 放射ブラー ----
    m_maskRT->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_shaftRT->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_shaftRT->GetRtv();
        native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd.SetViewportAndScissor(hw, hh);
        native->SetPipelineState(m_psoBlur.Get());
        native->SetGraphicsRootDescriptorTable(0, srvHeap->GetGpuHandle(m_maskRT->GetSrvIndex()));
        native->SetGraphicsRoot32BitConstants(1, kGRCBNum32, &cb, 0);
        native->DrawInstanced(3, 1, 0, 0);
    }

    m_shaftRT->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return m_shaftRT->GetSrvIndex();
}

} // namespace dx12e
