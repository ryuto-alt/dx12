// ===========================================================================
// ボリュメトリッククラウド: Application 側の糊（遅延確保・風の時間・履歴の管理・AP 直後の呼び出し）。
// 本体は renderer/atmosphere/CloudRenderer.{h,cpp}、シェーダは shaders/atmosphere/Cloud*.hlsl。
// ★clouds.enabled=false（既定）の間は CloudRenderer を作らず、描画コマンドも 1 命令も変わらない。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/AtmosphereHost.h"
#include "renderer/ViewDesc.h"
#include "renderer/atmosphere/AtmospherePasses.h"

namespace dx12e
{
using namespace DirectX;

void Application::AtmosphereDrawClouds(const ViewDesc& view, const RenderPassContext& ctx, u32 depthSrvIndex)
{
    if (!AtmosphereSkyUsable(view) || !view.primary || !m_drawSkybox || !(m_skyboxIntensity > 0.0f)) return;
    AtmoHost& h = *m_atmo;
    const CloudSettings& cs = m_scene->GetAtmosphereSettings().clouds;
    if (!cs.enabled)
    {
        h.cloudPrevValid = false;   // OFF の間は履歴を持たない（ON に戻したとき一括生成で作り直す）
        return;
    }
    if (depthSrvIndex == DescriptorHeap::kInvalidIndex || !view.sceneColor) return;

    if (!h.clouds.IsReady())
    {
        if (h.cloudsInitFailed) return;
        bool ok = false;
        try { ok = h.clouds.Initialize(*m_graphicsDevice, m_srvHeap.get(), PathResolver::ShaderDirW()); }
        catch (const std::exception& e) { Logger::Warn("雲: 初期化に失敗した（{}）。雲なしのまま", e.what()); }
        if (!ok) { h.cloudsInitFailed = true; return; }
    }
    // 解像度が変わるときは履歴・粗い RT を作り直すので、使用中でないことを保証する（まれ）
    if (h.clouds.NeedsResize(view.width, view.height)) m_commandQueue->WaitIdle();

    // 風で流れる時間。決定論撮影では 0 固定（同じ絵になる）。実時間の大きなヒッチは丸める。
    if (m_deterministicCapture) h.cloudTime = 0.0;
    else h.cloudTime += std::min(static_cast<double>(m_gameClock.GetDeltaTime()), 0.25);

    CloudRenderer::FrameInfo fi{};
    fi.width = view.width;
    fi.height = view.height;
    const float wind = cs.windSpeed * static_cast<float>(h.cloudTime) * 0.001f;   // km
    const float yaw = cs.windDirDeg * 3.14159265f / 180.0f;
    fi.windOffsetKm[0] = -std::sin(yaw) * wind;   // 雲は風下へ流れる＝ノイズを引く座標は風上側へずらす
    fi.windOffsetKm[1] = 0.0f;
    fi.windOffsetKm[2] = -std::cos(yaw) * wind;
    {
        XMFLOAT4X4 f;
        XMStoreFloat4x4(&f, XMMatrixTranspose(XMLoadFloat4x4(&view.viewProj)));
        std::memcpy(fi.viewProjT, &f, sizeof(fi.viewProjT));
    }
    std::memcpy(fi.prevViewProjT, h.cloudPrevVP, sizeof(fi.prevViewProjT));
    fi.havePrev = h.cloudPrevValid;
    // 連続していない（前フレームに描いていない）/ 決定論撮影 → 履歴を捨てて 16 サブフレームを一括生成
    fi.forceReset = m_deterministicCapture || (h.cloudLastFrame + 1 != m_framesSinceStart);
    fi.settingsChanged = !(h.cloudLast == cs);
    h.cloudLast = cs;
    h.cloudLastFrame = m_framesSinceStart;

    if (m_gpuTimer) m_gpuTimer->Begin(ctx.native, GpuTimer::Clouds);
    const D3D12_GPU_VIRTUAL_ADDRESS atmCb = h.renderer.CbAddress(ctx.frameIndex);
    const D3D12_GPU_DESCRIPTOR_HANDLE atmSrv = h.renderer.LutSrvGpu();
    h.clouds.Record(ctx.native, ctx.frameIndex, *m_graphicsDevice, cs, fi, atmCb, atmSrv);

    CloudCompositePass::Inputs ci{};
    ci.clouds = &h.clouds;
    ci.atmCb = atmCb;
    ci.atmSrv = atmSrv;
    ci.sceneRtv = view.sceneColor->GetRtv();
    ci.sceneColor = view.sceneColor->GetResource();
    ci.depthSrv = m_srvHeap->GetGpuHandle(depthSrvIndex);
    ci.width = view.width;
    ci.height = view.height;
    CloudCompositePass(ci).Execute(ctx);
    if (m_gpuTimer) m_gpuTimer->End(ctx.native, GpuTimer::Clouds);

    std::memcpy(h.cloudPrevVP, fi.viewProjT, sizeof(h.cloudPrevVP));
    h.cloudPrevValid = true;
}

} // namespace dx12e
