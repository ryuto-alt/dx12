#include "renderer/LensFlarePass.h"
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
static constexpr DXGI_FORMAT kLFFormat = DXGI_FORMAT_R11G11B10_FLOAT;

// HLSL の LFCB と一致（2 x float4 = 8 DWORD、ルート定数）
struct LFCB
{
    float p0[4];
    float p1[4];
};
static_assert(sizeof(LFCB) == 8 * sizeof(float), "LFCB must be 8 DWORDs");
static constexpr UINT kLFCBNum32 = 8;

void LensFlarePass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                               u32 width, u32 height, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;

    // --- Root Signature: t0(SRV table) + b0(8 DWORD) + s0(linear wrap = frac巻き込み対応) ---
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
        params[1].Constants.Num32BitValues = kLFCBNum32;
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

    // --- PSO（VS は FSTriVS 流用）---
    {
        auto vs = ShaderCompiler::LoadFromFile(shaderDir + L"PostProcess_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(shaderDir + L"LensFlare_PS.cso");

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
        pso.RTVFormats[0]         = kLFFormat;
        pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc            = { 1, 0 };
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
    }

    const float clearBlack[4] = {0, 0, 0, 1};
    m_flareRT = std::make_unique<RenderTarget>();
    m_flareRT->Initialize(device, rtvHeap, srvHeap,
        (std::max)(1u, m_width / 4), (std::max)(1u, m_height / 4), kLFFormat, clearBlack);

    Logger::Info("LensFlarePass initialized");
}

void LensFlarePass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width  = width;
    m_height = height;
    if (m_flareRT)
        m_flareRT->Resize(device, (std::max)(1u, width / 4), (std::max)(1u, height / 4));
}

u32 LensFlarePass::Generate(CommandList& cmd, DescriptorHeap* /*srvHeap*/,
                            D3D12_GPU_DESCRIPTOR_HANDLE bloomMipSrvGpu,
                            const PostProcessSettings& s)
{
    if (!m_pso || !m_flareRT) return DescriptorHeap::kInvalidIndex;
    auto* native = cmd.GetNative();

    LFCB cb{};
    cb.p0[0] = static_cast<float>((std::min)((std::max)(s.lfGhosts, 1), 8));
    cb.p0[1] = s.lfDispersal;
    cb.p0[2] = s.lfHalo;
    cb.p0[3] = s.lfIntensity;
    cb.p1[0] = s.lfChroma;
    cb.p1[1] = 8.0f;   // 中心減衰 pow

    m_flareRT->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_flareRT->GetRtv();
    native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd.SetViewportAndScissor(m_flareRT->GetWidth(), m_flareRT->GetHeight());

    native->SetGraphicsRootSignature(m_rootSig.Get());
    native->SetPipelineState(m_pso.Get());
    native->SetGraphicsRootDescriptorTable(0, bloomMipSrvGpu);
    native->SetGraphicsRoot32BitConstants(1, kLFCBNum32, &cb, 0);
    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->IASetVertexBuffers(0, 0, nullptr);
    native->IASetIndexBuffer(nullptr);
    native->DrawInstanced(3, 1, 0, 0);

    m_flareRT->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return m_flareRT->GetSrvIndex();
}

} // namespace dx12e
