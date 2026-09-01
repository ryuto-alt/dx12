#include "renderer/ScreenShaderPass.h"

#include "graphics/GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{

void ScreenShaderPass::Initialize(GraphicsDevice& device, DXGI_FORMAT outFormat)
{
    auto* dev = device.GetDevice();
    m_outFormat = outFormat;

    // p0: t0 画面カラー / p1: t1 深度 / p2: b0 ルート定数 20 DWORD
    D3D12_DESCRIPTOR_RANGE colorRange{};
    colorRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    colorRange.NumDescriptors     = 1;
    colorRange.BaseShaderRegister = 0;  // t0

    D3D12_DESCRIPTOR_RANGE depthRange = colorRange;
    depthRange.BaseShaderRegister = 1;  // t1

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &colorRange;
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &depthRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // CB リソースを持たない（ルート定数）＝Apply ごとのスロット多重化が要らない。
    // PostProcess の CB は「同じフレームで複数回 Apply すると後勝ちで上書きされる」問題を
    // スロット分割で解いているが、ルート定数ならコマンドリストに直接乗るのでその心配がない。
    params[2].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;  // b0
    params[2].Constants.Num32BitValues = 20;
    params[2].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister   = 0;  // s0
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1] = samplers[0];
    samplers[1].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].ShaderRegister = 1;    // s1

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = 3;
    desc.pParameters       = params;
    desc.NumStaticSamplers = 2;
    desc.pStaticSamplers   = samplers;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &serialized, &error));
    ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));

    Logger::Info("ScreenShaderPass initialized");
}

ID3D12PipelineState* ScreenShaderPass::FindPso(const std::string& key) const
{
    auto it = m_psos.find(key);
    return (it != m_psos.end()) ? it->second.pso.Get() : nullptr;
}

void ScreenShaderPass::InvalidatePso(const std::string& key)
{
    if (key.empty()) m_psos.clear();
    else             m_psos.erase(key);
}

ID3D12PipelineState* ScreenShaderPass::GetOrCreatePso(GraphicsDevice& device,
                                                      const std::string& key,
                                                      const std::vector<u8>& vsBytes,
                                                      const std::vector<u8>& psBytes,
                                                      std::string* outError)
{
    auto it = m_psos.find(key);
    if (it != m_psos.end())
        return it->second.pso.Get();   // 失敗済み（pso=null）もここで打ち切る

    Entry entry;
    entry.tried = true;
    if (!m_rootSig || vsBytes.empty() || psBytes.empty())
    {
        if (outError) *outError = "シェーダーのバイトコードが取得できません";
        m_psos[key] = std::move(entry);
        return nullptr;
    }

    // 頂点バッファ無しのフルスクリーン描画。ユーザーの VSMain が SV_VertexID から
    // 三角形を組む（雛形がそうなっている）。深度もカリングも使わない。
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature        = m_rootSig.Get();
    pso.VS                    = {vsBytes.data(), vsBytes.size()};
    pso.PS                    = {psBytes.data(), psBytes.size()};
    pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable   = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask            = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = m_outFormat;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc            = {1, 0};

    HRESULT hr = device.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&entry.pso));
    if (FAILED(hr))
    {
        entry.pso.Reset();
        // ★ここで失敗する典型は「雛形と違うルートシグネチャ前提で書かれている」ケース
        //   （b1 を宣言している / t2 を使っている等）。素通しに倒して絵は出し続ける。
        Logger::Error("スクリーンシェーダーの PSO 生成に失敗しました: {} (hr=0x{:08X})", key,
                      static_cast<unsigned>(hr));
        if (outError) *outError = "PSO の生成に失敗しました（b0/t0/t1/s0 以外のスロットを使っていませんか）";
    }

    ID3D12PipelineState* raw = entry.pso.Get();
    m_psos[key] = std::move(entry);
    return raw;
}

void ScreenShaderPass::Apply(ID3D12GraphicsCommandList* cmd,
                             ID3D12PipelineState* pso,
                             D3D12_GPU_DESCRIPTOR_HANDLE colorSrv,
                             D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
                             const Constants& cb)
{
    if (!pso || !m_rootSig) return;

    cmd->SetPipelineState(pso);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRootDescriptorTable(0, colorSrv);
    cmd->SetGraphicsRootDescriptorTable(1, depthSrv);
    cmd->SetGraphicsRoot32BitConstants(2, 20, &cb, 0);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace dx12e
