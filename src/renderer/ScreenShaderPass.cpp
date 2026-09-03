#include "renderer/ScreenShaderPass.h"

#include "graphics/GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"
#include "resource/ShaderDiagnostics.h"

#include <iterator>

namespace dx12e
{
namespace
{

// スクリーンシェーダーの契約。レジスタの「範囲」はルートシグネチャ実体から自動で出るので、
// ここに書くのは人間向けの意味づけだけ（書き忘れても嘘にはならない）。
constexpr shaderdiag::SlotNote kScreenSlotNotes[] = {
    {'b', 0, "cbuffer ScreenShaderCB … resolution / timeParams / params / cameraParams / uvOffsetScale（20 DWORD）"},
    {'t', 0, "画面カラー（ポストプロセス適用後の LDR・ガンマ空間）"},
    {'t', 1, "シーン深度（R32_FLOAT。0=near, 1=far の非線形深度）"},
    {'s', 0, "linear clamp サンプラー"},
    {'s', 1, "point clamp サンプラー"},
};

shaderdiag::Contract MakeScreenContract(ID3DBlob* rsBlob)
{
    shaderdiag::Contract c;
    c.title      = "スクリーンシェーダー（カメラの「画面シェーダー」）";
    c.entryNote  = "VSMain / PSMain（vs_6_0 / ps_6_0）";
    c.rsBlob     = rsBlob ? rsBlob->GetBufferPointer() : nullptr;
    c.rsBlobSize = rsBlob ? rsBlob->GetBufferSize() : 0;
    c.notes      = kScreenSlotNotes;
    c.noteCount  = std::size(kScreenSlotNotes);
    c.extra =
        "  そのほかの約束事\n"
        "    ・頂点バッファは使いません。VSMain は uint vid : SV_VertexID から\n"
        "      フルスクリーン三角形を組んでください（雛形がそうなっています）。\n"
        "    ・PSMain は float4 ... : SV_TARGET を返します。深度もステンシルも書けません。\n"
        "    ・画面の読み取りは必ず uvOffsetScale を掛けること（雛形の SampleScreen /\n"
        "      SampleDepth がやっています）。中間 RT はウィンドウ全面で、絵はその中の\n"
        "      表示矩形にしか入っていないため、直接 Sample すると位置がずれます。\n"
        "  雛形: ツールバーの「新規シェーダー」→ 種類「画面全体用」で生成できます。";
    return c;
}

} // namespace

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

    Microsoft::WRL::ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &m_rsBlob, &error));
    ThrowIfFailed(dev->CreateRootSignature(0, m_rsBlob->GetBufferPointer(),
        m_rsBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));

    // 「書式」をエディタ UI から引けるように登録する（デバイスに触らずテキストだけ読める）。
    shaderdiag::RegisterHelp(shaderdiag::kIdScreen,
                             shaderdiag::DescribeContract(MakeScreenContract(m_rsBlob.Get())));

    Logger::Info("ScreenShaderPass initialized");
}

ID3D12PipelineState* ScreenShaderPass::FindPso(const std::string& key) const
{
    auto it = m_psos.find(key);
    return (it != m_psos.end()) ? it->second.pso.Get() : nullptr;
}

bool ScreenShaderPass::HasTriedPso(const std::string& key) const
{
    auto it = m_psos.find(key);
    return it != m_psos.end() && it->second.tried;
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
        std::string msg =
            "スクリーンシェーダーのバイトコードが取得できません: " + key + "\n"
            "  ・HLSL がコンパイルできていない（ログの「コンパイルに失敗しました」を確認）\n"
            "  ・パスが違う（assets/shaders/ からの相対で指定します）\n"
            "  ・VSMain / PSMain のどちらかが無い（両方必須です）\n\n"
            + shaderdiag::GetHelp(shaderdiag::kIdScreen);
        if (outError) *outError = msg;
        shaderdiag::SetIssue(key, std::move(msg));
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
        //   D3D12 はその詳細をデバッグレイヤーにしか出さないので、シェーダーの
        //   リフレクションとルートシグネチャを自前で突き合わせて理由を特定する。
        std::string msg = shaderdiag::ExplainPsoFailure(
            MakeScreenContract(m_rsBlob.Get()), hr,
            vsBytes.data(), vsBytes.size(), psBytes.data(), psBytes.size(),
            "対象: " + key);
        Logger::Error("{}", msg);
        if (outError) *outError = msg;
        shaderdiag::SetIssue(key, std::move(msg));
    }
    else
    {
        shaderdiag::ClearIssue(key);
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
