#include "renderer/MotionBlurPass.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "graphics/CommandList.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>
#include <cstring>

namespace dx12e
{
static constexpr DXGI_FORMAT kMBFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// HLSL の MBCB と一致（float4x4 ×2 + float4 ×2 = 40 DWORD、ルート定数）
struct MBCB
{
    float invViewProj[16];
    float prevViewProj[16];
    float rectP[4];
    float params[4];
};
static_assert(sizeof(MBCB) == 40 * sizeof(float), "MBCB must be 40 DWORDs");
static constexpr UINT kMBCBNum32 = 40;

void MotionBlurPass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                                u32 width, u32 height, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;

    // --- Root Signature: t0=scene / t1=depth / t2=velocity + b0(40 DWORD) + s0 ---
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
        params[3].Constants.Num32BitValues = kMBCBNum32;
        params[3].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = _countof(params);
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
    m_shaderDir = shaderDir;
    RecreatePipelines(device);

    const float clearBlack[4] = {0, 0, 0, 1};
    m_outRT = std::make_unique<RenderTarget>();
    m_outRT->Initialize(device, rtvHeap, srvHeap, m_width, m_height, kMBFormat, clearBlack);

    Logger::Info("MotionBlurPass initialized");
}

void MotionBlurPass::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"PostProcess_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"MotionBlur_PS.cso");

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
    pso.RTVFormats[0]         = kMBFormat;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc            = { 1, 0 };
    ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

void MotionBlurPass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width  = width;
    m_height = height;
    if (m_outRT) m_outRT->Resize(device, width, height);
}

u32 MotionBlurPass::Apply(CommandList& cmd, DescriptorHeap* /*srvHeap*/,
                          D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
                          D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
                          D3D12_GPU_DESCRIPTOR_HANDLE velocitySrvGpu,
                          bool useVelocityBuffer,
                          const DirectX::XMFLOAT4X4& invViewProjT,
                          const DirectX::XMFLOAT4X4& prevViewProjT,
                          float uvOfsX, float uvOfsY, float uvScaleX, float uvScaleY,
                          u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH,
                          const PostProcessSettings& s)
{
    if (!m_pso || !m_outRT) return DescriptorHeap::kInvalidIndex;
    auto* native = cmd.GetNative();

    MBCB cb{};
    std::memcpy(cb.invViewProj,  &invViewProjT,  sizeof(cb.invViewProj));
    std::memcpy(cb.prevViewProj, &prevViewProjT, sizeof(cb.prevViewProj));
    cb.rectP[0] = uvOfsX;   cb.rectP[1] = uvOfsY;
    cb.rectP[2] = uvScaleX; cb.rectP[3] = uvScaleY;
    cb.params[0] = (std::min)((std::max)(s.mbStrength, 0.0f), 2.0f);
    cb.params[1] = static_cast<float>((std::min)((std::max)(s.mbSamples, 4), 16));
    cb.params[2] = 0.06f;   // 最大ブラー（ローカルUV。画面の約6%）
    // x>0.5 で速度バッファ方式（オブジェクト毎のブラー）。0 なら従来の深度再構成（カメラのみ）。
    cb.params[3] = useVelocityBuffer ? 1.0f : 0.0f;

    m_outRT->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_outRT->GetRtv();
    native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd.SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);

    native->SetGraphicsRootSignature(m_rootSig.Get());
    native->SetPipelineState(m_pso.Get());
    native->SetGraphicsRootDescriptorTable(0, sceneSrvGpu);
    native->SetGraphicsRootDescriptorTable(1, depthSrvGpu);
    native->SetGraphicsRootDescriptorTable(2, velocitySrvGpu);
    native->SetGraphicsRoot32BitConstants(3, kMBCBNum32, &cb, 0);
    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->IASetVertexBuffers(0, 0, nullptr);
    native->IASetIndexBuffer(nullptr);
    native->DrawInstanced(3, 1, 0, 0);

    // 後段（自動露出CS/ブルーム/uber）がシーンとして読むので複合読取状態へ
    m_outRT->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                           | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return m_outRT->GetSrvIndex();
}

} // namespace dx12e
