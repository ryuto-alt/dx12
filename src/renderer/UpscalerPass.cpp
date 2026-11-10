#include "renderer/UpscalerPass.h"
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

namespace
{
// UpscalePrep.hlsl の cbuffer PrepCB(b0) と一致させること（36 DWORD）。
struct PrepCB
{
    float invViewProj[16];
    float prevViewProj[16];
    u32   size[2];
    u32   pad[2];
};
static_assert(sizeof(PrepCB) == 36 * sizeof(float), "PrepCB must be 36 DWORDs");
constexpr UINT kPrepCBNum32 = 36;

constexpr DXGI_FORMAT kMvFormat  = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kOutFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;   // kSceneColorFormat と同じ
constexpr D3D12_RESOURCE_STATES kReadAll =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

void UpscalerPass::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, const std::wstring& shaderDir,
                              const std::wstring& ngxDataDir)
{
    m_device    = &device;
    m_srvHeap   = srvHeap;
    m_shaderDir = shaderDir;
    CreateRootSignature(device);
    RecreatePipelines(device);
    if (!m_dlss.Initialize(device.GetDevice(), ngxDataDir))
        Logger::Info("DLSS: 使えない（{}）。アップスケーラに dlss を選んでも TAA で描く", m_dlss.Reason());
}

void UpscalerPass::Shutdown()
{
    m_dlss.Shutdown();
    m_mv.Reset();
    m_out.Reset();
    m_resW = m_resH = m_resOutW = m_resOutH = 0;
}

void UpscalerPass::CreateRootSignature(GraphicsDevice& device)
{
    // b0(36 DWORD) + t0 / t1 / u0 の 1 個ずつのテーブル（連続した番号を要求しない＝既存の SRV 番号をそのまま張れる）。
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1; ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 1;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[2].NumDescriptors = 1; ranges[2].BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = kPrepCBNum32;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    for (u32 i = 0; i < 3; ++i)
    {
        params[i + 1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        params[i + 1].DescriptorTable.pDescriptorRanges   = &ranges[i];
        params[i + 1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters   = params;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ThrowIfFailed(device.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_prepRs)));
}

void UpscalerPass::RecreatePipelines(GraphicsDevice& device)
{
    auto cs = ShaderCompiler::LoadFromFile(m_shaderDir + L"UpscalePrep_CS.cso");
    if (cs.GetSize() == 0)
    {
        Logger::Warn("UpscalerPass: UpscalePrep_CS.cso が無い（DLSS は使わない）");
        m_prepPso.Reset();
        return;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_prepRs.Get();
    pso.CS = { cs.GetData(), cs.GetSize() };
    ThrowIfFailed(device.GetDevice()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_prepPso)));
}

void UpscalerPass::Resize(u32 renderW, u32 renderH, u32 outW, u32 outH)
{
    m_renderW = renderW; m_renderH = renderH; m_outW = outW; m_outH = outH;
    // ★呼び出し側（ApplyRenderResolution）が WaitIdle 済み。機能は寸法に紐づくので捨てて次の評価で作り直す。
    m_dlss.ReleaseFeature();
    if (m_resW != renderW || m_resH != renderH || m_resOutW != outW || m_resOutH != outH)
    {
        m_mv.Reset();
        m_out.Reset();
        m_resW = m_resH = m_resOutW = m_resOutH = 0;   // 次に DLSS を使うフレームで遅延確保（TAA だけの人に VRAM を払わせない）
    }
    m_reset = true;
}

bool UpscalerPass::EnsureResources()
{
    if (!m_device || !m_srvHeap || m_renderW == 0 || m_outW == 0) return false;
    if (m_mv && m_out && m_resW == m_renderW && m_resH == m_renderH && m_resOutW == m_outW && m_resOutH == m_outH)
        return true;
    auto* dev = m_device->GetDevice();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto make = [&](u32 w, u32 h, DXGI_FORMAT f, Microsoft::WRL::ComPtr<ID3D12Resource>& out, D3D12_RESOURCE_STATES& state)
    {
        out.Reset();
        D3D12_RESOURCE_DESC d{};
        d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width            = w;
        d.Height           = h;
        d.DepthOrArraySize = 1;
        d.MipLevels        = 1;
        d.Format           = f;
        d.SampleDesc       = {1, 0};
        d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };
    make(m_renderW, m_renderH, kMvFormat, m_mv, m_mvState);
    make(m_outW, m_outH, kOutFormat, m_out, m_outState);
    m_mv->SetName(L"Upscaler.MotionVectors");
    m_out->SetName(L"Upscaler.Output");

    if (m_mvUav == 0xFFFFFFFFu)  m_mvUav  = m_srvHeap->AllocateIndex();
    if (m_outSrv == 0xFFFFFFFFu) m_outSrv = m_srvHeap->AllocateIndex();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format        = kMvFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(m_mv.Get(), nullptr, &uav, m_srvHeap->GetCpuHandle(m_mvUav));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = kOutFormat;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    dev->CreateShaderResourceView(m_out.Get(), &srv, m_srvHeap->GetCpuHandle(m_outSrv));

    m_resW = m_renderW; m_resH = m_renderH; m_resOutW = m_outW; m_resOutH = m_outH;
    Logger::Info("Upscaler: 入出力を確保 MV {}x{} / 出力 {}x{}", m_renderW, m_renderH, m_outW, m_outH);
    return true;
}

XMFLOAT2 UpscalerPass::NextJitterNdc(u32 renderW, u32 renderH, u32 outW)
{
    m_phaseCount  = upscale::JitterPhaseCount(renderW, outW);
    m_jitterIndex = (m_jitterIndex + 1) % m_phaseCount;
    const TaaJitterNdc j = ComputeTaaJitterNdc(m_jitterIndex, renderW, renderH, 1.0f);
    return XMFLOAT2{j.x, j.y};
}

void UpscalerPass::ObserveCamera(const XMFLOAT3& pos, const XMFLOAT3& fwd)
{
    if (m_prevCamValid)
    {
        const float dx = pos.x - m_prevCamPos.x, dy = pos.y - m_prevCamPos.y, dz = pos.z - m_prevCamPos.z;
        const float moved = dx * dx + dy * dy + dz * dz;
        const float cosA  = fwd.x * m_prevCamFwd.x + fwd.y * m_prevCamFwd.y + fwd.z * m_prevCamFwd.z;
        if (moved > 10.0f * 10.0f || cosA < 0.766f)   // 10 m / cos(40°)
        {
            m_reset = true;
            ++m_cutCount;
        }
    }
    m_prevCamPos = pos;
    m_prevCamFwd = fwd;
    m_prevCamValid = true;
}

bool UpscalerPass::PrepareDlssFeature(ID3D12CommandQueue* queue, upscale::DlssQuality q, const std::function<void()>& waitIdle)
{
    if (!m_dlss.Available() || m_renderW == 0 || m_outW == 0) return false;
    if (m_dlss.FeatureMismatch(m_renderW, m_renderH, m_outW, m_outH, q))
    {
        if (waitIdle) waitIdle();
        m_dlss.ReleaseFeature();
        m_reset = true;
    }
    const bool wasBuilding = m_dlss.FeatureBuilding();
    const bool ready = m_dlss.PrepareFeatureAsync(queue, m_renderW, m_renderH, m_outW, m_outH, q);
    if (ready && wasBuilding) m_reset = true;   // 作成中は TAA で描いていた＝DLSS の履歴は空
    return ready;
}

u32 UpscalerPass::EvaluateDlss(CommandList& cmd, const DlssFrame& f)
{
    m_lastEvalOk = false;
    if (!m_dlss.Available() || !m_prepPso || !f.color || !f.depth || !f.velocity) return DescriptorHeap::kInvalidIndex;
    if (!EnsureResources()) return DescriptorHeap::kInvalidIndex;
    auto* native = cmd.GetNative();

    // ---- 1) MV の準備（空の画素へカメラ運動を埋める）----
    if (m_mvState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        cmd.TransitionResource(m_mv.Get(), m_mvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_mvState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    f.velocity->Transition(cmd, kReadAll);

    PrepCB cb{};
    std::memcpy(cb.invViewProj,  &f.invViewProjT,  sizeof(cb.invViewProj));
    std::memcpy(cb.prevViewProj, &f.prevViewProjT, sizeof(cb.prevViewProj));
    cb.size[0] = m_renderW;
    cb.size[1] = m_renderH;
    cmd.SetDescriptorHeap(m_srvHeap->GetHeap());
    native->SetComputeRootSignature(m_prepRs.Get());
    native->SetPipelineState(m_prepPso.Get());
    native->SetComputeRoot32BitConstants(0, kPrepCBNum32, &cb, 0);
    native->SetComputeRootDescriptorTable(1, f.depthSrv);
    native->SetComputeRootDescriptorTable(2, m_srvHeap->GetGpuHandle(f.velocity->GetSrvIndex()));
    native->SetComputeRootDescriptorTable(3, m_srvHeap->GetGpuHandle(m_mvUav));
    native->Dispatch((m_renderW + 7) / 8, (m_renderH + 7) / 8, 1);

    cmd.TransitionResource(m_mv.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadAll);
    m_mvState = kReadAll;
    if (m_outState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        cmd.TransitionResource(m_out.Get(), m_outState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_outState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // ---- 2) 寸法 / 品質が変わっていたら機能を作り直す（使用中の機能は GPU アイドルでしか捨てられない）----
    if (m_dlss.FeatureMismatch(m_renderW, m_renderH, m_outW, m_outH, f.quality))
    {
        if (f.waitIdle) f.waitIdle();
        m_dlss.ReleaseFeature();
        m_reset = true;
    }

    // ---- 3) 評価 ----
    const upscale::JitterPx jp = upscale::NdcJitterToPixels(f.jitterNdc.x, f.jitterNdc.y, m_renderW, m_renderH);
    DlssBackend::Eval e{};
    e.color   = f.color->GetResource();
    e.depth   = f.depth;
    e.motion  = m_mv.Get();
    e.output  = m_out.Get();
    e.renderW = m_renderW; e.renderH = m_renderH; e.outW = m_outW; e.outH = m_outH;
    e.jitterX = jp.x * m_jitterSign;
    e.jitterY = jp.y * m_jitterSign;
    // 速度は「現UV - 前UV」（UV 単位）。DLSS は「前 - 現」をピクセルで欲しいので -size を掛けさせる。
    e.mvScaleX = -static_cast<float>(m_renderW);
    e.mvScaleY = -static_cast<float>(m_renderH);
    e.reset    = m_reset;
    e.frameMs  = f.frameMs;
    e.quality  = f.quality;
    const bool ok = m_dlss.Evaluate(native, e);

    cmd.TransitionResource(m_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadAll);
    m_outState = kReadAll;
    if (!ok) return DescriptorHeap::kInvalidIndex;

    m_reset = false;
    m_lastEvalOk = true;
    ++m_evalCount;
    return m_outSrv;
}

} // namespace dx12e
