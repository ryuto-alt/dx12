#include "renderer/TaaPass.h"
#include "renderer/TaaJitter.h"
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
using namespace DirectX;

// HLSL の cbuffer TaaCB(b0) とバイト単位で一致させること（ルート定数 48 DWORD）。
struct TaaCB
{
    float invViewProj[16];   // offset  0
    float prevViewProj[16];  // offset 16
    float rectP[4];          // offset 32 : xy=UVオフセット, zw=UVスケール
    float texel[4];          // offset 36 : xy=1/RTW,1/RTH （zw は 16B 境界のための詰め物）
    float rtSize[4];         // offset 40 : xy=フルRTのpxサイズ（zw は詰め物）
    float params[4];         // offset 44 : x=historyValid y=feedbackMin z=feedbackMax w=varianceGamma
};
static_assert(sizeof(TaaCB) == 48 * sizeof(float), "TaaCB must be 48 DWORDs");
static constexpr UINT kTaaCBNum32 = 48;

// VelocityDebug.hlsl の cbuffer DbgCB(b0)（8 DWORD）。
struct TaaDebugCB
{
    float rectP[4];
    float scale[4];
};
static constexpr UINT kTaaDebugCBNum32 = 8;

// ---------------------------------------------------------------------------
// ハルトン列（実体は TaaJitter.h の純関数。tests/velocity_math_test.cpp で検証済み）
// ---------------------------------------------------------------------------
float TaaPass::Halton(u32 i, u32 b)
{
    return HaltonRadicalInverse(i, b);
}

XMFLOAT2 TaaPass::NextJitterNdc(u32 sampleCount, u32 vpW, u32 vpH, float jitterScale)
{
    const u32 n = std::clamp(sampleCount, 2u, 64u);
    m_jitterIndex = (m_jitterIndex + 1) % n;
    const TaaJitterNdc j = ComputeTaaJitterNdc(m_jitterIndex, vpW, vpH, jitterScale);
    return XMFLOAT2{j.x, j.y};
}

// ---------------------------------------------------------------------------
// 初期化
// ---------------------------------------------------------------------------
void TaaPass::CreateResolveRootSignature(GraphicsDevice& device)
{
    // t0=scene / t1=history / t2=velocity / t3=depth の 4 テーブル + b0(48 DWORD)
    // + s0(LINEAR CLAMP) / s1(POINT CLAMP)。ルートコスト 4 + 48 = 52 / 64 DWORD。
    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    for (u32 i = 0; i < 4; ++i)
    {
        ranges[i].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors     = 1;
        ranges[i].BaseShaderRegister = i;
    }

    D3D12_ROOT_PARAMETER params[5]{};
    for (u32 i = 0; i < 4; ++i)
    {
        params[i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges   = &ranges[i];
        params[i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    params[4].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants.ShaderRegister = 0;  // b0
    params[4].Constants.Num32BitValues = kTaaCBNum32;
    params[4].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    // ★2 本必要（履歴の Catmull-Rom は LINEAR、シーン/深度/速度の近傍統計は POINT）。
    //   既存パスをコピーすると 1 本だけになりがちなので注意。
    D3D12_STATIC_SAMPLER_DESC samps[2]{};
    samps[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samps[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[0].MaxLOD           = D3D12_FLOAT32_MAX;
    samps[0].ShaderRegister   = 0;
    samps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samps[1]                  = samps[0];
    samps[1].Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samps[1].ShaderRegister   = 1;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = _countof(params);
    desc.pParameters       = params;
    desc.NumStaticSamplers = _countof(samps);
    desc.pStaticSamplers   = samps;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ThrowIfFailed(device.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_resolveRootSig)));
}

void TaaPass::CreateDebugRootSignature(GraphicsDevice& device)
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors     = 1;
    range.BaseShaderRegister = 0;  // t0

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &range;
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = kTaaDebugCBNum32;
    params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

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
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_debugRootSig)));
}

void TaaPass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                         u32 width, u32 height, DXGI_FORMAT historyFormat, DXGI_FORMAT debugRtFormat,
                         const std::wstring& shaderDir)
{
    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;
    m_historyFormat = historyFormat;
    m_debugRtFormat = debugRtFormat;
    m_shaderDir     = shaderDir;
    m_srvHeap       = srvHeap;

    CreateResolveRootSignature(device);
    CreateDebugRootSignature(device);
    RecreatePipelines(device);

    // 速度 RT（クリア = 0 = 速度なし）。m_distortRT が同じ RG16F で既に運用されている。
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_velocityRT = std::make_unique<RenderTarget>();
    m_velocityRT->Initialize(device, rtvHeap, srvHeap, m_width, m_height, kVelocityFormat, zero);

    // 履歴 RT ×2（ping-pong）。シーン RT と同じ HDR フォーマット。
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (u32 i = 0; i < 2; ++i)
    {
        m_history[i] = std::make_unique<RenderTarget>();
        m_history[i]->Initialize(device, rtvHeap, srvHeap, m_width, m_height, m_historyFormat, black);
    }
    m_historyWrite = 0;
    m_historyValid = false;

    Logger::Info("TaaPass initialized ({}x{})", m_width, m_height);
}

void TaaPass::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();

    auto makePso = [&](ID3D12RootSignature* rs, const wchar_t* psName, DXGI_FORMAT rtFormat,
                       Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"PostProcess_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + psName);
        if (vs.GetSize() == 0 || ps.GetSize() == 0)
        {
            Logger::Warn("TaaPass: シェーダが見つかりません（この PSO は作らない）");
            return;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = rs;
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
        pso.RTVFormats[0]         = rtFormat;
        pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc            = { 1, 0 };
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out)));
    };

    makePso(m_resolveRootSig.Get(), L"TaaResolve_PS.cso",   m_historyFormat, m_resolvePso);
    makePso(m_debugRootSig.Get(),   L"VelocityDebug_PS.cso", m_debugRtFormat, m_debugPso);
}

void TaaPass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width  = width;
    m_height = height;
    if (m_velocityRT) m_velocityRT->Resize(device, width, height);
    for (u32 i = 0; i < 2; ++i)
        if (m_history[i]) m_history[i]->Resize(device, width, height);
    m_historyValid = false;   // リサイズで履歴の座標系が変わる＝捨てる
}

// ---------------------------------------------------------------------------
// 速度バッファ
// ---------------------------------------------------------------------------
void TaaPass::BeginVelocity(CommandList& cmd, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                            D3D12_CPU_DESCRIPTOR_HANDLE gbufferRtv,
                            u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH)
{
    if (!m_velocityRT) return;

    m_velocityRT->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // 全面をクリアする（ビューポート矩形の外＝速度なし、空も 0 のまま）。
    constexpr float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    cmd.ClearRenderTarget(m_velocityRT->GetRtv(), zero);

    // RTV0=速度 / RTV1=G-Buffer。PSO は常に 2 本で作られているので本数を分岐させない。
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { m_velocityRT->GetRtv(), gbufferRtv };
    cmd.GetNative()->OMSetRenderTargets(2, rtvs, FALSE, &dsv);
    cmd.SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);
}

void TaaPass::EndVelocity(CommandList& cmd)
{
    if (!m_velocityRT) return;
    m_velocityRT->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

u32 TaaPass::GetVelocitySrvIndex() const
{
    return m_velocityRT ? m_velocityRT->GetSrvIndex() : DescriptorHeap::kInvalidIndex;
}

// ---------------------------------------------------------------------------
// 解決
// ---------------------------------------------------------------------------
u32 TaaPass::Resolve(CommandList& cmd,
                     D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv,
                     D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
                     const XMFLOAT4X4& invViewProjT,
                     const XMFLOAT4X4& prevViewProjT,
                     float uvOfsX, float uvOfsY, float uvSclX, float uvSclY,
                     u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH,
                     const TaaSettings& s)
{
    if (!m_resolvePso || !m_history[0] || !m_history[1] || !m_velocityRT || !m_srvHeap)
        return DescriptorHeap::kInvalidIndex;

    RenderTarget* dst = m_history[m_historyWrite].get();
    RenderTarget* src = m_history[m_historyWrite ^ 1].get();

    TaaCB cb{};
    std::memcpy(cb.invViewProj,  &invViewProjT,  sizeof(cb.invViewProj));
    std::memcpy(cb.prevViewProj, &prevViewProjT, sizeof(cb.prevViewProj));
    cb.rectP[0] = uvOfsX; cb.rectP[1] = uvOfsY; cb.rectP[2] = uvSclX; cb.rectP[3] = uvSclY;
    cb.texel[0] = 1.0f / static_cast<float>(m_width);
    cb.texel[1] = 1.0f / static_cast<float>(m_height);
    cb.rtSize[0] = static_cast<float>(m_width);
    cb.rtSize[1] = static_cast<float>(m_height);
    (void)vpW; (void)vpH;   // 矩形は rectP に入っている（ビューポート設定にのみ使う）
    cb.params[0] = m_historyValid ? 1.0f : 0.0f;
    cb.params[1] = std::clamp(s.feedbackMin, 0.0f, 0.99f);
    cb.params[2] = std::clamp(s.feedbackMax, cb.params[1], 0.995f);
    cb.params[3] = std::clamp(s.varianceGamma, 0.1f, 4.0f);

    // 履歴が無効な初回（起動直後 / リサイズ直後 / シーン切替直後）は、まだ一度も書かれていない
    // 履歴 RT をサンプルすることになる。params.x=0 で寄与は 0 に掛け消されるが、
    // 未初期化メモリが NaN だと 0 * NaN = NaN が伝播しうる。素直にクリアしておく。
    if (!m_historyValid)
    {
        src->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
        constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd.ClearRenderTarget(src->GetRtv(), black);
    }

    src->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    dst->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

    auto* native = cmd.GetNative();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = dst->GetRtv();
    native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd.SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);

    native->SetGraphicsRootSignature(m_resolveRootSig.Get());
    native->SetPipelineState(m_resolvePso.Get());
    native->SetGraphicsRootDescriptorTable(0, sceneSrv);
    native->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGpuHandle(src->GetSrvIndex()));
    native->SetGraphicsRootDescriptorTable(2, m_srvHeap->GetGpuHandle(m_velocityRT->GetSrvIndex()));
    native->SetGraphicsRootDescriptorTable(3, depthSrv);
    native->SetGraphicsRoot32BitConstants(4, kTaaCBNum32, &cb, 0);
    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->IASetVertexBuffers(0, 0, nullptr);
    native->IASetIndexBuffer(nullptr);
    native->DrawInstanced(3, 1, 0, 0);

    // 後段（自動露出CS/ブルーム/uber）がシーンとして読むので複合読取状態へ
    dst->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                       | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const u32 outSrv = dst->GetSrvIndex();
    m_historyWrite ^= 1;      // 次フレームは反対側へ書く（今書いた方が次の履歴）
    m_historyValid = true;
    return outSrv;
}

// ---------------------------------------------------------------------------
// デバッグ可視化
// ---------------------------------------------------------------------------
void TaaPass::DrawVelocityDebug(CommandList& cmd,
                                D3D12_GPU_DESCRIPTOR_HANDLE velocitySrv,
                                float uvOfsX, float uvOfsY, float uvSclX, float uvSclY,
                                u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH, float scale)
{
    if (!m_debugPso) return;

    TaaDebugCB cb{};
    cb.rectP[0] = uvOfsX; cb.rectP[1] = uvOfsY; cb.rectP[2] = uvSclX; cb.rectP[3] = uvSclY;
    cb.scale[0] = scale;

    auto* native = cmd.GetNative();
    cmd.SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);
    native->SetGraphicsRootSignature(m_debugRootSig.Get());
    native->SetPipelineState(m_debugPso.Get());
    native->SetGraphicsRootDescriptorTable(0, velocitySrv);
    native->SetGraphicsRoot32BitConstants(1, kTaaDebugCBNum32, &cb, 0);
    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->IASetVertexBuffers(0, 0, nullptr);
    native->IASetIndexBuffer(nullptr);
    native->DrawInstanced(3, 1, 0, 0);
}

} // namespace dx12e
