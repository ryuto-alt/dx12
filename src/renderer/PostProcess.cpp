#include "renderer/PostProcess.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{
// HLSL の cbuffer PostCB と一致させる（5 つの float4 = 20 DWORD）。
struct PostCB
{
    float uvOffset[2];
    float uvScale[2];
    float texel[2];
    float exposure;
    float contrast;
    float saturation;
    float tint[3];
    float vignette;
    float bloom;
    float bloomThreshold;
    float _pad0;
    int   fxaa;
    int   grayscale;
    int   enabled;
    int   _pad1;
};
static_assert(sizeof(PostCB) == 20 * sizeof(float), "PostCB must be 20 DWORDs");

void PostProcess::Initialize(GraphicsDevice& device, DXGI_FORMAT outFormat, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();

    // --- Root Signature: t0(SRV table) + b0(20 DWORD constants) + s0(linear clamp) ---
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
        params[1].Constants.Num32BitValues = 20;
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

    // --- PSO: VB なしフルスクリーン三角形 / Depth OFF / Cull NONE ---
    {
        auto vs = ShaderCompiler::LoadFromFile(shaderDir + L"PostProcess_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(shaderDir + L"PostProcess_PS.cso");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.VS = { vs.GetData(), vs.GetSize() };
        pso.PS = { ps.GetData(), ps.GetSize() };
        pso.InputLayout = { nullptr, 0 };  // SV_VertexID のみ

        pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;

        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        pso.DepthStencilState.DepthEnable   = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;

        pso.SampleMask            = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets      = 1;
        pso.RTVFormats[0]         = outFormat;
        pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc            = { 1, 0 };

        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
    }

    Logger::Info("PostProcess initialized");
}

void PostProcess::Apply(ID3D12GraphicsCommandList* cmd,
                        D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
                        const PostProcessSettings& s,
                        float uvOffsetX, float uvOffsetY,
                        float uvScaleX, float uvScaleY,
                        float texelW, float texelH)
{
    if (!m_pso) return;

    PostCB cb{};
    cb.uvOffset[0] = uvOffsetX; cb.uvOffset[1] = uvOffsetY;
    cb.uvScale[0]  = uvScaleX;  cb.uvScale[1]  = uvScaleY;
    cb.texel[0]    = texelW;    cb.texel[1]    = texelH;
    cb.exposure    = s.exposure;
    cb.contrast    = s.contrast;
    cb.saturation  = s.saturation;
    cb.tint[0]     = s.tint.x; cb.tint[1] = s.tint.y; cb.tint[2] = s.tint.z;
    cb.vignette       = s.vignette;
    cb.bloom          = s.bloom;
    cb.bloomThreshold = s.bloomThreshold;
    cb.fxaa        = s.fxaa ? 1 : 0;
    cb.grayscale   = s.grayscale ? 1 : 0;
    cb.enabled     = s.enabled ? 1 : 0;

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRootDescriptorTable(0, sceneSrvGpu);
    cmd->SetGraphicsRoot32BitConstants(1, 20, &cb, 0);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace dx12e
