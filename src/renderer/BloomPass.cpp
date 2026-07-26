#include "renderer/BloomPass.h"
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
// チェーンの中間フォーマット。HDR で全成分正なので R11G11B10 で十分（帯域半分）。
static constexpr DXGI_FORMAT kBloomFormat = DXGI_FORMAT_R11G11B10_FLOAT;

// HLSL の BloomCB と一致（3 x float4 = 12 DWORD、ルート定数）
struct BloomCB
{
    float srcRect[4];  // xy=UVオフセット, zw=UVスケール
    float texelP[4];   // xy=ソーステクセル, z=初段フラグ, w=未使用
    float filt[4];     // x=しきい値, y=ニー, zw=未使用
};
static_assert(sizeof(BloomCB) == 12 * sizeof(float), "BloomCB must be 12 DWORDs");
static constexpr UINT kBloomCBNum32 = 12;

void BloomPass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                           u32 width, u32 height, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;

    // --- Root Signature: t0(SRV table) + b0(12 DWORD constants) + s0(linear clamp) ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;  // t0

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;  // b0
        params[1].Constants.Num32BitValues = kBloomCBNum32;
        params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;  // s0
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

    // --- PSO（ダウン: 上書き / アップ: dest=src*factor+dest*(1-factor) のブレンド合成）---
    m_shaderDir = shaderDir;
    RecreatePipelines(device);

    // --- チェーン RT（1/2, 1/4, ... 1/64）---
    const float clearBlack[4] = {0, 0, 0, 1};
    for (u32 i = 0; i < kMips; ++i)
    {
        const u32 w = (std::max)(1u, m_width  >> (i + 1));
        const u32 h = (std::max)(1u, m_height >> (i + 1));
        m_mips[i] = std::make_unique<RenderTarget>();
        m_mips[i]->Initialize(device, rtvHeap, srvHeap, w, h, kBloomFormat, clearBlack);
    }

    Logger::Info("BloomPass initialized ({} mips)", kMips);
}

void BloomPass::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"Bloom_VS.cso");
    auto psDown = ShaderCompiler::LoadFromFile(m_shaderDir + L"BloomDown_PS.cso");
    auto psUp   = ShaderCompiler::LoadFromFile(m_shaderDir + L"BloomUp_PS.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.Get();
    pso.VS = { vs.GetData(), vs.GetSize() };
    pso.PS = { psDown.GetData(), psDown.GetSize() };
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
    pso.RTVFormats[0]         = kBloomFormat;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc            = { 1, 0 };
    ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoDown)));

    pso.PS = { psUp.GetData(), psUp.GetSize() };
    auto& rt0 = pso.BlendState.RenderTarget[0];
    rt0.BlendEnable    = TRUE;
    rt0.SrcBlend       = D3D12_BLEND_BLEND_FACTOR;
    rt0.DestBlend      = D3D12_BLEND_INV_BLEND_FACTOR;
    rt0.BlendOp        = D3D12_BLEND_OP_ADD;
    rt0.SrcBlendAlpha  = D3D12_BLEND_ONE;
    rt0.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt0.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoUp)));
}

u32 BloomPass::GetMipSrvIndex(u32 mip) const
{
    if (mip >= kMips || !m_mips[mip]) return DescriptorHeap::kInvalidIndex;
    return m_mips[mip]->GetSrvIndex();
}

void BloomPass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width  = width;
    m_height = height;
    for (u32 i = 0; i < kMips; ++i)
    {
        if (!m_mips[i]) continue;
        m_mips[i]->Resize(device,
            (std::max)(1u, m_width >> (i + 1)),
            (std::max)(1u, m_height >> (i + 1)));
    }
}

u32 BloomPass::Generate(CommandList& cmd, DescriptorHeap* srvHeap,
                        D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
                        float sceneTexelW, float sceneTexelH,
                        const PostProcessSettings& s)
{
    if (!m_psoDown || !m_mips[0]) return DescriptorHeap::kInvalidIndex;
    auto* native = cmd.GetNative();

    native->SetGraphicsRootSignature(m_rootSig.Get());
    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->IASetVertexBuffers(0, 0, nullptr);
    native->IASetIndexBuffer(nullptr);

    BloomCB cb{};
    cb.filt[0] = s.bloomThreshold;
    cb.filt[1] = (std::max)(s.bloomKnee, 0.0f);

    // ---- ダウンサンプル: scene → mip0 → mip1 → ... ----
    native->SetPipelineState(m_psoDown.Get());
    for (u32 i = 0; i < kMips; ++i)
    {
        RenderTarget* dst = m_mips[i].get();
        dst->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = dst->GetRtv();
        native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd.SetViewportAndScissor(dst->GetWidth(), dst->GetHeight());

        if (i == 0)
        {
            cb.srcRect[0] = 0.0f; cb.srcRect[1] = 0.0f;   // ★#16: シーンは RT 全面
            cb.srcRect[2] = 1.0f; cb.srcRect[3] = 1.0f;
            cb.texelP[0]  = sceneTexelW; cb.texelP[1] = sceneTexelH;
            cb.texelP[2]  = 1.0f;  // 初段: Karis + しきい値抽出
            native->SetGraphicsRootDescriptorTable(0, sceneSrvGpu);
        }
        else
        {
            RenderTarget* src = m_mips[i - 1].get();
            src->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cb.srcRect[0] = 0; cb.srcRect[1] = 0; cb.srcRect[2] = 1; cb.srcRect[3] = 1;
            cb.texelP[0]  = 1.0f / static_cast<float>(src->GetWidth());
            cb.texelP[1]  = 1.0f / static_cast<float>(src->GetHeight());
            cb.texelP[2]  = 0.0f;
            native->SetGraphicsRootDescriptorTable(0, srvHeap->GetGpuHandle(src->GetSrvIndex()));
        }
        native->SetGraphicsRoot32BitConstants(1, kBloomCBNum32, &cb, 0);
        native->DrawInstanced(3, 1, 0, 0);
    }

    // ---- アップサンプル: mip[i+1] をテントで拡大し mip[i] へ radius 率で合成 ----
    native->SetPipelineState(m_psoUp.Get());
    const float r = (std::min)((std::max)(s.bloomRadius, 0.05f), 0.95f);
    const float bf[4] = {r, r, r, r};
    native->OMSetBlendFactor(bf);

    cb.srcRect[0] = 0; cb.srcRect[1] = 0; cb.srcRect[2] = 1; cb.srcRect[3] = 1;
    for (int i = static_cast<int>(kMips) - 2; i >= 0; --i)
    {
        RenderTarget* src = m_mips[i + 1].get();
        RenderTarget* dst = m_mips[i].get();
        src->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        dst->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = dst->GetRtv();
        native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd.SetViewportAndScissor(dst->GetWidth(), dst->GetHeight());

        cb.texelP[0] = 1.0f / static_cast<float>(src->GetWidth());
        cb.texelP[1] = 1.0f / static_cast<float>(src->GetHeight());
        cb.texelP[2] = 0.0f;
        native->SetGraphicsRootDescriptorTable(0, srvHeap->GetGpuHandle(src->GetSrvIndex()));
        native->SetGraphicsRoot32BitConstants(1, kBloomCBNum32, &cb, 0);
        native->DrawInstanced(3, 1, 0, 0);
    }

    m_mips[0]->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return m_mips[0]->GetSrvIndex();
}

} // namespace dx12e
