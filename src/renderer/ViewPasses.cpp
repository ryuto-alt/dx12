// ===========================================================================
// ViewPasses — IRenderPass へ切り出した RenderView の段（宣言と契約は ViewPasses.h / RenderPass.h）
// ---------------------------------------------------------------------------
// ★各 Execute のコマンド列は、切り出す前の Application::RenderView の該当ブロックと 1 命令ずつ
//   同じ順になるように書いてある（ゴールデン画像で bitExact を確認済み）。違うのは
//   「後始末の張り直し（RootSig / PSO / ヒープ / RT）」が無くなり、代わりに使う側が入口で
//   自分の状態を張るようになったことだけ。
// ===========================================================================
#include "renderer/ViewPasses.h"

#include "graphics/CommandList.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GpuTimer.h"
#include "graphics/RenderTarget.h"
#include "graphics/RootSignature.h"
#include "renderer/ClusteredLightCulling.h"
#include "renderer/DecalSystem.h"
#include "renderer/SkyboxRenderer.h"
#include "renderer/ParticleSystem.h"
#include "renderer/GpuParticleSystem.h"

namespace dx12e
{
using namespace DirectX;

namespace
{
// 主ビューだけ計測する（副ビューは ctx.timer == nullptr）。
struct GpuScope
{
    const RenderPassContext& ctx;
    GpuTimer::Scope          scope;
    GpuScope(const RenderPassContext& c, GpuTimer::Scope s) : ctx(c), scope(s)
    {
        if (ctx.timer) ctx.timer->Begin(ctx.native, scope);
    }
    ~GpuScope() { if (ctx.timer) ctx.timer->End(ctx.native, scope); }
    GpuScope(const GpuScope&) = delete;
    GpuScope& operator=(const GpuScope&) = delete;
};
} // namespace

void TrackedState::Require(CommandList& cmd, D3D12_RESOURCE_STATES next)
{
    if (!resource || state == next) return;
    cmd.TransitionResource(resource, state, next);
    state = next;
}

// ---- ClusterCullPass --------------------------------------------------------------------
void ClusterCullPass::DeclareResources(std::vector<PassResourceUse>& out) const
{
    // ライト配列 / インデックス / カウントは ClusteredLightCulling の内部（遷移も所有者）。
    // 出口では PIXEL|NON_PIXEL の読取状態（Forward の t13..t15 とフォグの t3..t5 が読む）。
    out.push_back({"clusterLists", nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, PassAccess::Write});
}

void ClusterCullPass::Execute(const RenderPassContext& ctx)
{
    if (!m_in.culling) return;
    if (m_in.cull)
    {
        GpuScope t(ctx, GpuTimer::ClusterCull);
        m_in.culling->Dispatch(ctx.native, XMLoadFloat4x4(&m_in.view),
                               m_in.proj11, m_in.proj22,
                               m_in.zNear, m_in.zFarCluster,
                               m_in.zFarCamera, m_in.lightCount, ctx.frameIndex);
    }
    else
    {
        // フォールバック（正射 / 設定 OFF / 副ビュー）でもテーブルはバインドするので、
        // インデックス/カウントは読取状態にしておく（中身は読まれない）。
        m_in.culling->EnsureReadable(ctx.native);
    }
}

// ---- DdgiUpdatePass ---------------------------------------------------------------------
void DdgiUpdatePass::Execute(const RenderPassContext& ctx)
{
    if (!m_in.ddgi || !m_in.device || !m_in.settings) return;
    GpuScope t(ctx, GpuTimer::Ddgi);
    m_in.ddgi->Update(ctx.native, *m_in.device, *m_in.settings, m_in.desc);
}

// ---- DecalCullPass ----------------------------------------------------------------------
void DecalCullPass::Execute(const RenderPassContext& ctx)
{
    if (!m_in.decals) return;
    if (m_in.cull)
    {
        m_in.decals->Cull(ctx.native, XMLoadFloat4x4(&m_in.view),
                          m_in.proj11, m_in.proj22,
                          m_in.zNear, m_in.zFarCluster,
                          m_in.zFarCamera, m_in.decalCount, ctx.frameIndex);
    }
    else
    {
        // デカール 0 個でもテーブルはバインドするので読取状態にしておく
        // （フォワード PS は clusterExtra.w==0 で読まないが、状態は正しく保つ）。
        m_in.decals->EnsureReadable(ctx.native);
    }
}

// ---- VolumetricFogBuildPass -------------------------------------------------------------
void VolumetricFogBuildPass::DeclareResources(std::vector<PassResourceUse>& out) const
{
    out.push_back({"csm",          m_in.csm,          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, PassAccess::Read});
    out.push_back({"spotShadows",  m_in.spotShadows,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, PassAccess::Read});
    out.push_back({"pointShadows", m_in.pointShadows, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, PassAccess::Read});
    out.push_back({"clusterLists", nullptr,           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, PassAccess::Read});
    out.push_back({"fogVolumes",   nullptr,           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,         PassAccess::Write});
}

void VolumetricFogBuildPass::Execute(const RenderPassContext& ctx)
{
    if (!m_in.fog || !m_in.device || !m_in.settings || !m_in.params) return;
    GpuScope t(ctx, GpuTimer::VolumetricFog);
    // 影テクスチャは PIXEL_SHADER_RESOURCE で置かれている。compute から読むには NON_PIXEL が要る。
    // （クラスタのインデックス/カウントは ClusteredLightCulling が最初から
    //   PIXEL|NON_PIXEL の合成状態で置いているので遷移不要。）
    CommandList& cmd = *ctx.cmd;
    cmd.TransitionResource(m_in.csm,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd.TransitionResource(m_in.spotShadows,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd.TransitionResource(m_in.pointShadows,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    m_in.fog->BuildVolumes(ctx.native, *m_in.device, *m_in.settings, *m_in.params, ctx.frameIndex);

    // 出口の契約: 影マップは既定の置き場（PIXEL_SHADER_RESOURCE）へ戻す。
    cmd.TransitionResource(m_in.csm,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd.TransitionResource(m_in.spotShadows,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd.TransitionResource(m_in.pointShadows,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// ---- FogCompositePass -------------------------------------------------------------------
void FogCompositePass::DeclareResources(std::vector<PassResourceUse>& out) const
{
    out.push_back({"depth",      nullptr,           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, PassAccess::Read});
    out.push_back({"fogVolumes", nullptr,           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, PassAccess::Read});
    out.push_back({"sceneColor", m_in.sceneColor,   D3D12_RESOURCE_STATE_RENDER_TARGET,         PassAccess::Write});
}

void FogCompositePass::Execute(const RenderPassContext& ctx)
{
    if (!m_in.fog) return;
    // 深度は SRV で読む（DSV は張らない＝同じ深度を DSV と SRV で同時に使わない）。
    if (ctx.depth) ctx.depth->Require(*ctx.cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ctx.native->OMSetRenderTargets(1, &m_in.sceneRtv, FALSE, nullptr);
    ctx.cmd->SetViewportAndScissor(m_in.width, m_in.height);
    ctx.cmd->SetDescriptorHeap(ctx.srvHeap->GetHeap());
    m_in.fog->Composite(ctx.native, m_in.depthSrv, 0u, 0u, m_in.width, m_in.height, ctx.frameIndex);
}

// ---- SkyboxPass -------------------------------------------------------------------------
void SkyboxPass::DeclareResources(std::vector<PassResourceUse>& out) const
{
    out.push_back({"sceneColor", m_in.sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET, PassAccess::Write});
}

void SkyboxPass::Execute(const RenderPassContext& ctx)
{
    if (!m_in.sky) return;
    ctx.cmd->SetDescriptorHeap(ctx.srvHeap->GetHeap());
    ctx.cmd->SetRenderTarget(m_in.sceneRtv, m_in.depthDsv);
    ctx.cmd->SetViewportAndScissor(m_in.width, m_in.height);
    m_in.sky->Render(ctx.native, m_in.envCube, m_in.invViewProjT, m_in.intensity);
}

// ---- ForwardScenePass -------------------------------------------------------------------
void ForwardScenePass::DeclareResources(std::vector<PassResourceUse>& out) const
{
    out.push_back({"sceneColor",   m_in.sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET,         PassAccess::Write});
    out.push_back({"depth",        nullptr,         D3D12_RESOURCE_STATE_DEPTH_WRITE,           PassAccess::Write});
    out.push_back({"csm",          nullptr,         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, PassAccess::Read});
    out.push_back({"spotShadows",  nullptr,         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, PassAccess::Read});
    out.push_back({"pointShadows", nullptr,         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, PassAccess::Read});
    out.push_back({"clusterLists", nullptr,         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, PassAccess::Read});
}

void ForwardScenePass::Execute(const RenderPassContext& ctx)
{
    if (!m_in.rootSig) return;
    CommandList& cmd = *ctx.cmd;
    // Forward の PS が読むものを全部ここで張る。★前の段が何を張っていてもこの下だけで完結する。
    if (ctx.depth) ctx.depth->Require(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd.SetDescriptorHeap(ctx.srvHeap->GetHeap());
    cmd.SetRootSignature(*m_in.rootSig);
    cmd.SetPerFrameCBV(RootSignature::kSlotPerFrame, m_in.perFrameCB);

    // シャドウマップSRVをバインド
    cmd.SetSRVTable(RootSignature::kSlotShadowSRV, m_in.csmTable);

    // スポット/ポイント影SRV(t9,t10)をバインド（連番確保なのでスポット側の1個渡しで2枚とも有効になる）
    cmd.SetSRVTable(RootSignature::kSlotPunctualShadowSRV, m_in.punctualShadowTable);

    // IBL テーブル(t5,t6,t7)をバインド（常に有効＝ダミー含む。hasIBL で読むか分岐）
    if (m_in.hasIblTable)
        cmd.SetSRVTable(RootSignature::kSlotIBLTable, m_in.iblTable);

    // クラスタライト テーブル(t13,t14,t15 + デカール予約 t18..t21)をバインド。
    // フォワード PS が t13..t15 を参照する以上、クラスタード無効時（フォールバック経路）でも
    // 必ずバインドが要る（未バインドのテーブルはデバッグレイヤ違反）。
    if (m_in.hasClusterTable)
        cmd.SetSRVTable(RootSignature::kSlotClusterSRV, m_in.clusterTable);

    cmd.SetRenderTarget(m_in.sceneRtv, m_in.depthDsv);
    cmd.SetViewportAndScissor(m_in.width, m_in.height);

    GpuScope t(ctx, GpuTimer::MainScene);
    if (m_in.drawMeshes) m_in.drawMeshes();
}

// ---- ParticlesPass ----------------------------------------------------------------------
void ParticlesPass::DeclareResources(std::vector<PassResourceUse>& out) const
{
    out.push_back({"depth",      nullptr,         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, PassAccess::Read});
    out.push_back({"sceneColor", m_in.sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET,         PassAccess::Write});
    out.push_back({"distortion", m_in.distortRT ? m_in.distortRT->GetResource() : nullptr,
                   D3D12_RESOURCE_STATE_RENDER_TARGET, PassAccess::Write});
}

void ParticlesPass::Execute(const RenderPassContext& ctx)
{
    m_distortDrawn = false;
    if (!m_in.particles) return;
    CommandList& cmd = *ctx.cmd;

    // 深度を読み取り可能へ遷移し soft particles 用 SRV を供給。DSV はバインドせず PS で手動オクルージョン。
    if (ctx.depth) ctx.depth->Require(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ctx.native->OMSetRenderTargets(1, &m_in.sceneRtv, FALSE, nullptr);
    cmd.SetViewportAndScissor(m_in.width, m_in.height);
    cmd.SetDescriptorHeap(ctx.srvHeap->GetHeap());

    const XMMATRIX viewProjJ = XMLoadFloat4x4(&m_in.viewProjJittered);
    if (m_in.hasDepthSrv)
        m_in.particles->SetSceneDepth(m_in.depthSrv,
            m_in.projA, m_in.projB, 1.0f / m_in.rtWidth, 1.0f / m_in.rtHeight);
    else
        m_in.particles->DisableSceneDepth();
    m_in.particles->SetTime(m_in.time);
    // ラスタライズ系はジッタあり（TAA でアンチエイリアスされる）。
    m_in.particles->Render(ctx.native, viewProjJ, m_in.camRight, m_in.camUp, m_in.camPos);

    // ---- GPUパーティクル（compute シム + ExecuteIndirect）: 同じ HDR RT へ加算 ----
    if (m_in.gpuParticles)
    {
        if (m_in.hasDepthSrv)
            m_in.gpuParticles->SetSceneDepth(m_in.depthSrv,
                m_in.projA, m_in.projB, 1.0f / m_in.rtWidth, 1.0f / m_in.rtHeight);
        else
            m_in.gpuParticles->DisableSceneDepth();
        m_in.gpuParticles->SimulateAndRender(ctx.native, m_in.gpuDt, m_in.time,
                                             viewProjJ, m_in.camRight, m_in.camUp);
    }

    // ---- 歪みパーティクル（熱ゆらぎ/衝撃波）: 歪みバッファ(RG16F)へ ----
    // Render() と同一フレームのインスタンスバッファを共有するため直後に描く。
    if (m_in.particles->HasDistortion() && m_in.distortRT)
    {
        m_in.distortRT->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
        constexpr float distClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cmd.ClearRenderTarget(m_in.distortRT->GetRtv(), distClear);
        auto drtv = m_in.distortRT->GetRtv();
        ctx.native->OMSetRenderTargets(1, &drtv, FALSE, nullptr);
        cmd.SetViewportAndScissor(m_in.width, m_in.height);
        m_in.particles->RenderDistortion(ctx.native, viewProjJ, m_in.camRight, m_in.camUp);
        m_in.distortRT->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_distortDrawn = true;
    }
}

} // namespace dx12e
