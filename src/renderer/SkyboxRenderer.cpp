#include "renderer/SkyboxRenderer.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{

// shaders/ibl/Skybox.hlsl の cbuffer SkyboxConstants と一致（20 DWORD = 16(invVP) + 1(intensity) + 3(pad)）
struct SkyboxCB
{
    DirectX::XMFLOAT4X4 invViewProj;
    float intensity;
    float _pad[3];
};
static_assert(sizeof(SkyboxCB) == 20 * sizeof(float), "SkyboxCB must be 20 DWORDs");
static constexpr UINT kSkyboxCBNum32 = 20;

void SkyboxRenderer::Initialize(GraphicsDevice& device, DXGI_FORMAT rtvFormat, const std::wstring& shaderDirW)
{
    auto* dev = device.GetDevice();

    // --- Root Signature: b0(20 DWORD) + t0(SRV table) + s0(linear clamp mip有) ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;  // t0

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;  // b0
        params[0].Constants.Num32BitValues = kSkyboxCBNum32;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderRegister   = 0;  // s0
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 2;
        desc.pParameters       = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    // --- PSO: VB なし全画面三角形 / Depth OFF / Cull NONE ---
    m_rtvFormat = rtvFormat;
    m_shaderDir = shaderDirW;
    RecreatePipelines(device);

    Logger::Info("SkyboxRenderer initialized");
}

void SkyboxRenderer::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"Skybox_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"Skybox_PS.cso");

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
    pso.RTVFormats[0]         = m_rtvFormat;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc            = { 1, 0 };

    ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

void SkyboxRenderer::Render(ID3D12GraphicsCommandList* cmd,
                            D3D12_GPU_DESCRIPTOR_HANDLE envCubeSrvGpu,
                            const DirectX::XMFLOAT4X4& invViewProj,
                            float intensity)
{
    if (!m_pso) return;

    SkyboxCB cb{};
    cb.invViewProj = invViewProj;
    cb.intensity   = intensity;

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRoot32BitConstants(0, kSkyboxCBNum32, &cb, 0);
    cmd->SetGraphicsRootDescriptorTable(1, envCubeSrvGpu);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

void SkyboxRenderer::Reset()
{
    m_pso.Reset();
    m_rootSig.Reset();
}

} // namespace dx12e
