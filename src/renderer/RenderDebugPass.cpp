#include "renderer/RenderDebugPass.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/CommandList.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{

// HLSL の cbuffer RenderDebugCB(b0) とバイト単位で一致させること（ルート定数 12 DWORD）。
// ★N25: DXC は「未参照のスカラーメンバ」を消して残りを詰め直す。ここは全部 float4 で、
//   かつ HLSL 側が 3 本とも参照しているのでズレない。
struct RenderDebugCB
{
    float rectP[4];   // xy = UV オフセット, zw = UV スケール
    float modeP[4];   // x = モード, y = 表示倍率, z = projA(_33), w = projB(_43)
    float rangeP[4];  // x = 深度レンジ(m), y = 露出, zw = 予約
};
static_assert(sizeof(RenderDebugCB) == 12 * sizeof(float), "RenderDebugCB must be 12 DWORDs");
static constexpr UINT kRenderDebugCBNum32 = 12;

void RenderDebugPass::CreateRootSignature(GraphicsDevice& device)
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors     = 1;
    ranges[0].BaseShaderRegister = 0;   // t0 : モードごとのソース
    ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors     = 1;
    ranges[1].BaseShaderRegister = 1;   // t1 : 深度

    D3D12_ROOT_PARAMETER params[3]{};
    for (int i = 0; i < 2; ++i)
    {
        params[i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges   = &ranges[i];
        params[i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    params[2].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.Num32BitValues = kRenderDebugCBNum32;
    params[2].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
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
    ThrowIfFailed(device.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
}

void RenderDebugPass::Initialize(GraphicsDevice& device, DXGI_FORMAT sceneRtFormat,
                                 const std::wstring& shaderDir)
{
    m_rtFormat  = sceneRtFormat;
    m_shaderDir = shaderDir;
    CreateRootSignature(device);
    RecreatePipelines(device);
    Logger::Info("RenderDebugPass initialized");
}

void RenderDebugPass::RecreatePipelines(GraphicsDevice& device)
{
    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"PostProcess_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"RenderDebug_PS.cso");
    if (vs.GetSize() == 0 || ps.GetSize() == 0)
    {
        Logger::Warn("RenderDebugPass: シェーダが見つかりません（可視化は無効のまま）");
        return;
    }

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
    pso.RTVFormats[0]         = m_rtFormat;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc            = { 1, 0 };
    ThrowIfFailed(device.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

void RenderDebugPass::Draw(CommandList& cmd, const DrawDesc& d)
{
    if (!m_pso || d.mode == RenderDebugMode::Off) return;
    if (d.sourceSrv.ptr == 0 || d.depthSrv.ptr == 0) return;

    RenderDebugCB cb{};
    cb.rectP[0]  = d.uvOfsX; cb.rectP[1] = d.uvOfsY;
    cb.rectP[2]  = d.uvSclX; cb.rectP[3] = d.uvSclY;
    cb.modeP[0]  = static_cast<float>(static_cast<u32>(d.mode));
    cb.modeP[1]  = d.gain;
    cb.modeP[2]  = d.projA;
    cb.modeP[3]  = d.projB;
    cb.rangeP[0] = d.depthRange;
    cb.rangeP[1] = d.exposure;

    auto* native = cmd.GetNative();
    cmd.SetViewportAndScissor(d.vpLeft, d.vpTop, d.vpW, d.vpH);
    native->SetGraphicsRootSignature(m_rootSig.Get());
    native->SetPipelineState(m_pso.Get());
    native->SetGraphicsRootDescriptorTable(0, d.sourceSrv);
    native->SetGraphicsRootDescriptorTable(1, d.depthSrv);
    native->SetGraphicsRoot32BitConstants(2, kRenderDebugCBNum32, &cb, 0);
    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->IASetVertexBuffers(0, 0, nullptr);
    native->IASetIndexBuffer(nullptr);
    native->DrawInstanced(3, 1, 0, 0);
}

} // namespace dx12e
