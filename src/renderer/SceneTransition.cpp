#include "renderer/SceneTransition.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>

namespace dx12e
{
void SceneTransition::Initialize(GraphicsDevice& device, DXGI_FORMAT outFormat, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();

    // --- Root Signature: b0(progress/type/aspect/pad = 4 DWORD, PIXEL) ---
    {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = 0;
        param.Constants.Num32BitValues = 4;
        param.ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 1;
        desc.pParameters   = &param;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    // --- PSO: VB なしフルスクリーン三角形 / AlphaBlend ON / Depth OFF ---
    m_outFormat = outFormat;
    m_shaderDir = shaderDir;
    RecreatePipelines(device);

    Logger::Info("SceneTransition initialized");
}

void SceneTransition::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"Transition_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"Transition_PS.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.Get();
    pso.VS = { vs.GetData(), vs.GetSize() };
    pso.PS = { ps.GetData(), ps.GetSize() };
    pso.InputLayout = { nullptr, 0 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    auto& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable           = TRUE;
    rt.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp               = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rt.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.DepthStencilState.DepthEnable   = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask       = UINT_MAX;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0]    = m_outFormat;
    pso.DSVFormat        = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc       = { 1, 0 };

    ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

void SceneTransition::Start(TransitionType type, float totalDuration)
{
    m_type          = type;
    m_dur           = (totalDuration > 0.05f) ? totalDuration : 0.05f;
    m_t             = 0.0f;
    m_active        = true;
    m_halfwayPending = false;
    m_halfwayFired   = false;
}

void SceneTransition::Update(float dt)
{
    if (!m_active) return;
    float prev = m_t;
    m_t += dt;
    const float half = m_dur * 0.5f;
    if (prev < half && m_t >= half)
        m_halfwayPending = true;     // 画面が隠れた瞬間
    if (m_t >= m_dur)
        m_active = false;
}

bool SceneTransition::ConsumeHalfway()
{
    if (m_halfwayPending && !m_halfwayFired)
    {
        m_halfwayFired   = true;
        m_halfwayPending = false;
        return true;
    }
    return false;
}

float SceneTransition::Progress() const
{
    const float half = m_dur * 0.5f;
    if (m_t < half)  return std::clamp(m_t / half, 0.0f, 1.0f);            // 閉じる 0→1
    return std::clamp(1.0f - (m_t - half) / half, 0.0f, 1.0f);            // 開く 1→0
}

void SceneTransition::Render(ID3D12GraphicsCommandList* cmd, float aspect)
{
    if (!m_active || !m_pso) return;

    struct TransCB { float progress; int type; float aspect; float total; } cb{};
    cb.progress = Progress();
    cb.type     = static_cast<int>(m_type);
    cb.aspect   = aspect;
    cb.total    = std::clamp(m_t / m_dur, 0.0f, 1.0f);   // 全体進行 0..1（閉じ→開きを跨ぐ。Seek のシークバー用）

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRoot32BitConstants(0, 4, &cb, 0);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace dx12e
