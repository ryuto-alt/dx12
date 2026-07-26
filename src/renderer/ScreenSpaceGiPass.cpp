#include "renderer/ScreenSpaceGiPass.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "graphics/Buffer.h"
#include "graphics/CommandList.h"
#include "graphics/FrameResources.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>

namespace dx12e
{
using namespace DirectX;

// shaders/screenspace/ScreenSpaceParams.hlsli の cbuffer SsParams(b0) とバイト単位で一致させること。
struct SsParamsCB
{
    XMFLOAT4X4 proj;      // 転置済み
    XMFLOAT4X4 invProj;   // 転置済み
    XMFLOAT4X4 view;      // 転置済み
    XMFLOAT4X4 invView;   // 転置済み

    XMFLOAT4 rt;          // xy = 1/フルRT, zw = 1/ハーフRT
    XMFLOAT4 viewport;    // xy = 原点(px), zw = サイズ(px)（フル解像度基準）

    XMFLOAT4 ssr0;        // x=maxDistance y=thickness z=stride w=bias
    XMFLOAT4 ssr1;        // x=maxSteps y=roughnessCutoff z=edgeFade w=intensity

    XMFLOAT4 ssgi0;       // x=radius y=thickness z=rayCount w=stepCount
    XMFLOAT4 ssgi1;       // x=intensity y=clampValue z=feedback w=iblFallback

    XMFLOAT4 misc;        // x=zNear y=zFar z=historyValid w=hasIBL
    XMFLOAT4 misc2;       // x=フレーム連番（SSGI の時間ジッタ用） yzw=予約
};
static_assert(sizeof(SsParamsCB) == 64 * 4 + 16 * 8, "SsParamsCB layout mismatch with ScreenSpaceParams.hlsli");

// ルートパラメータ。全パスが同じ 6 テーブル + CBV を使い、そのパスが読まないレジスタにも
// 有効なディスクリプタを必ず貼る（未初期化のテーブルをバインドするとデバッグレイヤが落とす）。
enum : u32
{
    kRpDepth = 0, kRpGBuffer, kRpColorA, kRpColorB, kRpVelocity, kRpIrradiance, kRpCB, kRpCount
};

void ScreenSpaceGiPass::CreateRootSignature(GraphicsDevice& device)
{
    D3D12_DESCRIPTOR_RANGE ranges[6]{};
    for (u32 i = 0; i < 6; ++i)
    {
        ranges[i].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors     = 1;
        ranges[i].BaseShaderRegister = i;   // t0..t5
    }

    D3D12_ROOT_PARAMETER params[kRpCount]{};
    for (u32 i = 0; i < 6; ++i)
    {
        params[i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges   = &ranges[i];
        params[i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    params[kRpCB].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[kRpCB].Descriptor.ShaderRegister = 0;   // b0
    params[kRpCB].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp[2]{};
    samp[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    samp[0].MaxLOD           = D3D12_FLOAT32_MAX;
    samp[0].ShaderRegister   = 0;   // s0
    samp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samp[1]        = samp[0];
    samp[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp[1].ShaderRegister = 1;     // s1

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = kRpCount;
    desc.pParameters       = params;
    desc.NumStaticSamplers = 2;
    desc.pStaticSamplers   = samp;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ThrowIfFailed(device.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
}

void ScreenSpaceGiPass::CreateTargets(GraphicsDevice& device)
{
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    auto make = [&](std::unique_ptr<RenderTarget>& rt, u32 w, u32 h)
    {
        if (!rt)
        {
            rt = std::make_unique<RenderTarget>();
            rt->Initialize(device, m_rtvHeap, m_srvHeap, w, h, kColorFormat, clear);
        }
        else
        {
            rt->Resize(device, w, h);
        }
    };
    make(m_prevColor,     m_width, m_height);
    make(m_prevColorHalf, m_halfW, m_halfH);
    make(m_ssrTrace,      m_halfW, m_halfH);
    make(m_ssrOut,        m_width, m_height);
    make(m_ssgiTrace,     m_halfW, m_halfH);
    make(m_ssgiHist[0],   m_halfW, m_halfH);
    make(m_ssgiHist[1],   m_halfW, m_halfH);
    make(m_ssgiOut,       m_width, m_height);
}

void ScreenSpaceGiPass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap,
                                   DescriptorHeap* srvHeap, u32 width, u32 height,
                                   const std::wstring& shaderDir)
{
    m_rtvHeap = rtvHeap;
    m_srvHeap = srvHeap;
    m_width   = (width  > 0) ? width  : 1;
    m_height  = (height > 0) ? height : 1;
    m_halfW   = (m_width  + 1) / 2;
    m_halfH   = (m_height + 1) / 2;

    CreateRootSignature(device);
    m_shaderDir = shaderDir;
    RecreatePipelines(device);
    CreateTargets(device);

    m_paramCB = std::make_unique<ConstantBuffer>();
    m_paramCB->Initialize(device, sizeof(SsParamsCB), FrameResources::kFrameCount);

    Logger::Info("ScreenSpaceGiPass initialized ({}x{} / half {}x{})",
                 m_width, m_height, m_halfW, m_halfH);
}

void ScreenSpaceGiPass::RecreatePipelines(GraphicsDevice& device)
{
    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"ScreenSpace_VS.cso");
    if (vs.GetSize() == 0)
    {
        Logger::Warn("ScreenSpace_VS.cso が読めません。SSR/SSGI は無効のままになります。");
        return;
    }

    auto build = [&](const wchar_t* psName, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + psName);
        if (ps.GetSize() == 0) { out.Reset(); return; }

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
        pso.RTVFormats[0]         = kColorFormat;
        pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc            = { 1, 0 };
        ThrowIfFailed(device.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out)));
    };

    build(L"ColorDownsample_PS.cso", m_psoDownsample);
    build(L"SsrTrace_PS.cso",        m_psoSsrTrace);
    build(L"SsrUpsample_PS.cso",     m_psoSsrUpsample);
    build(L"SsgiTrace_PS.cso",       m_psoSsgiTrace);
    build(L"SsgiTemporal_PS.cso",    m_psoSsgiTemporal);
    build(L"SsgiUpsample_PS.cso",    m_psoSsgiUpsample);
}

void ScreenSpaceGiPass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width  = width;
    m_height = height;
    m_halfW  = (m_width  + 1) / 2;
    m_halfH  = (m_height + 1) / 2;
    CreateTargets(device);
    InvalidateHistory();
}

void ScreenSpaceGiPass::CaptureSceneColor(CommandList& cmd, RenderTarget& sceneRT)
{
    if (!m_prevColor || !m_psoDownsample) return;
    // サイズ/形式が一致しないとコピーできない（リサイズ直後の 1 フレームなど）。
    if (sceneRT.GetWidth() != m_prevColor->GetWidth()
        || sceneRT.GetHeight() != m_prevColor->GetHeight()
        || sceneRT.GetFormat() != m_prevColor->GetFormat())
        return;

    sceneRT.Transition(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_prevColor->Transition(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd.GetNative()->CopyResource(m_prevColor->GetResource(), sceneRT.GetResource());
    m_prevColor->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ハーフ解像度版（SSGI のヒット色 / SSR の粗い面用）。
    // DownsamplePS は cbuffer を読まない（テクセルサイズは GetDimensions で引く）ので
    // b0 のバインドは不要＝このパスはパラメータ確定より前に走らせてよい。
    m_prevColorHalf->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto* nat = cmd.GetNative();
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(m_halfW), static_cast<float>(m_halfH), 0.0f, 1.0f};
    D3D12_RECT     sc{0, 0, static_cast<LONG>(m_halfW), static_cast<LONG>(m_halfH)};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_prevColorHalf->GetRtv();
    nat->SetGraphicsRootSignature(m_rootSig.Get());
    nat->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    nat->IASetVertexBuffers(0, 0, nullptr);
    nat->IASetIndexBuffer(nullptr);
    nat->RSSetViewports(1, &vp);
    nat->RSSetScissorRects(1, &sc);
    nat->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    nat->SetPipelineState(m_psoDownsample.Get());
    // DownsamplePS は t2(colorA) しか読まない。他のレジスタは DXC が除去済み＝バインド不要。
    nat->SetGraphicsRootDescriptorTable(kRpColorA, m_srvHeap->GetGpuHandle(m_prevColor->GetSrvIndex()));
    nat->DrawInstanced(3, 1, 0, 0);
    m_prevColorHalf->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_colorValid = true;
}

void ScreenSpaceGiPass::Generate(CommandList& cmd, const GenerateDesc& d,
                                 u32& outSsrSrv, u32& outSsgiSrv)
{
    outSsrSrv  = DescriptorHeap::kInvalidIndex;
    outSsgiSrv = DescriptorHeap::kInvalidIndex;

    const bool wantSsr  = d.ssr  && d.ssr->enabled  && m_psoSsrTrace && m_psoSsrUpsample;
    const bool wantSsgi = d.ssgi && d.ssgi->enabled && m_psoSsgiTrace && m_psoSsgiTemporal
                                                   && m_psoSsgiUpsample;
    if ((!wantSsr && !wantSsgi) || !m_colorValid || !m_paramCB) return;

    // ---- パラメータ ----
    SsParamsCB cb{};
    XMStoreFloat4x4(&cb.proj,    XMMatrixTranspose(d.proj));
    XMStoreFloat4x4(&cb.invProj, XMMatrixTranspose(XMMatrixInverse(nullptr, d.proj)));
    XMStoreFloat4x4(&cb.view,    XMMatrixTranspose(d.view));
    XMStoreFloat4x4(&cb.invView, XMMatrixTranspose(XMMatrixInverse(nullptr, d.view)));
    cb.rt = { 1.0f / static_cast<float>(m_width),  1.0f / static_cast<float>(m_height),
              1.0f / static_cast<float>(m_halfW),  1.0f / static_cast<float>(m_halfH) };
    cb.viewport = { static_cast<float>(d.vpLeft), static_cast<float>(d.vpTop),
                    static_cast<float>(d.vpW),    static_cast<float>(d.vpH) };

    const SsrSettings  ssrDef{};
    const SsgiSettings ssgiDef{};
    const SsrSettings&  s  = d.ssr  ? *d.ssr  : ssrDef;
    const SsgiSettings& g  = d.ssgi ? *d.ssgi : ssgiDef;
    cb.ssr0 = { (std::max)(s.maxDistance, 0.1f), (std::max)(s.thickness, 0.01f),
                std::clamp(s.stride, 1.0f, 8.0f), (std::max)(s.bias, 0.0f) };
    cb.ssr1 = { static_cast<float>(std::clamp(s.maxSteps, 8, 128)),
                std::clamp(s.roughnessCutoff, 0.02f, 1.0f),
                std::clamp(s.edgeFade, 0.0f, 0.5f), std::clamp(s.intensity, 0.0f, 1.0f) };
    cb.ssgi0 = { (std::max)(g.radius, 0.1f), (std::max)(g.thickness, 0.01f),
                 static_cast<float>(std::clamp(g.rayCount, 1, 4)),
                 static_cast<float>(std::clamp(g.stepCount, 4, 24)) };
    cb.ssgi1 = { (std::max)(g.intensity, 0.0f), (std::max)(g.clampValue, 0.01f),
                 std::clamp(g.feedback, 0.0f, 0.98f), g.iblFallback ? 1.0f : 0.0f };
    cb.misc = { d.zNear, d.zFar, m_ssgiHistValid ? 1.0f : 0.0f, d.hasIbl ? 1.0f : 0.0f };
    // フレーム連番。SSGI のディザを時間方向にも回すため（固定だと時間蓄積が収束しない）。
    // 64 で折り返すのは fp32 の精度と frac() の分解能を保つため。
    cb.misc2 = { static_cast<float>(m_frameCounter % 64u), 0.0f, 0.0f, 0.0f };
    ++m_frameCounter;
    m_paramCB->Update(&cb, sizeof(cb), d.frameIndex);

    auto* nat = cmd.GetNative();
    nat->SetGraphicsRootSignature(m_rootSig.Get());
    nat->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    nat->IASetVertexBuffers(0, 0, nullptr);
    nat->IASetIndexBuffer(nullptr);
    nat->SetGraphicsRootConstantBufferView(kRpCB, m_paramCB->GetGpuAddress(d.frameIndex));

    // そのパスが読まないテーブルにも必ず有効なディスクリプタを貼る。
    nat->SetGraphicsRootDescriptorTable(kRpDepth,      d.depthSrv);
    nat->SetGraphicsRootDescriptorTable(kRpGBuffer,    d.gbufferSrv);
    nat->SetGraphicsRootDescriptorTable(kRpVelocity,   d.velocitySrv);
    nat->SetGraphicsRootDescriptorTable(kRpIrradiance, d.irradianceSrv);

    auto srv = [&](const RenderTarget* rt) { return m_srvHeap->GetGpuHandle(rt->GetSrvIndex()); };

    auto drawTo = [&](RenderTarget* dst, ID3D12PipelineState* pso,
                      RenderTarget* colorA, RenderTarget* colorB)
    {
        // 入力側も必ず PSR へ（作りたての RT は RENDER_TARGET 状態。同一状態なら no-op）。
        colorA->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        colorB->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        dst->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(dst->GetWidth()),
                          static_cast<float>(dst->GetHeight()), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(dst->GetWidth()), static_cast<LONG>(dst->GetHeight())};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = dst->GetRtv();
        nat->RSSetViewports(1, &vp);
        nat->RSSetScissorRects(1, &sc);
        nat->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        nat->SetPipelineState(pso);
        nat->SetGraphicsRootDescriptorTable(kRpColorA, srv(colorA));
        nat->SetGraphicsRootDescriptorTable(kRpColorB, srv(colorB));
        nat->DrawInstanced(3, 1, 0, 0);
        dst->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };

    if (wantSsr)
    {
        // t2 = 前フレームカラー(フル) / t3 = 同ハーフ（粗い面用）
        drawTo(m_ssrTrace.get(), m_psoSsrTrace.Get(), m_prevColor.get(), m_prevColorHalf.get());
        // t2 = ハーフのトレース結果
        drawTo(m_ssrOut.get(), m_psoSsrUpsample.Get(), m_ssrTrace.get(), m_prevColorHalf.get());
        outSsrSrv = m_ssrOut->GetSrvIndex();
    }

    if (wantSsgi)
    {
        // t3 = 前フレームカラー(ハーフ)。ヒット点の色をここから取ると分散が激減する。
        drawTo(m_ssgiTrace.get(), m_psoSsgiTrace.Get(), m_prevColor.get(), m_prevColorHalf.get());

        const u32 write = m_histWrite;
        const u32 read  = 1u - m_histWrite;
        // t2 = 今フレームのトレース / t3 = 前フレームの履歴
        drawTo(m_ssgiHist[write].get(), m_psoSsgiTemporal.Get(),
               m_ssgiTrace.get(), m_ssgiHist[read].get());
        drawTo(m_ssgiOut.get(), m_psoSsgiUpsample.Get(),
               m_ssgiHist[write].get(), m_prevColorHalf.get());

        m_histWrite      = read;
        m_ssgiHistValid  = true;
        outSsgiSrv       = m_ssgiOut->GetSrvIndex();
    }
    else
    {
        m_ssgiHistValid = false;
    }
}

} // namespace dx12e
