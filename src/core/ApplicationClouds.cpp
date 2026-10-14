// ===========================================================================
// ボリュメトリッククラウド: Application 側の糊（遅延確保・風の時間・履歴の管理・AP 直後の呼び出し）。
// 本体は renderer/atmosphere/CloudRenderer.{h,cpp}、シェーダは shaders/atmosphere/Cloud*.hlsl。
// ★clouds.enabled=false（既定）の間は CloudRenderer を作らず、描画コマンドも 1 命令も変わらない。
//
// 雲の影 / 環境光:
//   ・影   … 深度プリパスの区間（コンタクトシャドウ / RT 影の直後）で影マップ → 影マスク t11 へ掛ける（AtmosphereCloudShadow）。
//            t11 が白ダミーだったフレームは、このパスが作る R8 を t11 にする（フォワードのシェーダは無改造）。
//   ・環境光 … 空キューブを作った直後（IBL の再ベイクの前）に、全 mip を sky*T + L に書き換える（AtmosphereCloudsToSkyCube）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/AtmosphereHost.h"
#include "renderer/ViewDesc.h"
#include "renderer/atmosphere/AtmospherePasses.h"

namespace dx12e
{
using namespace DirectX;

// CloudRenderer の遅延確保。失敗したら二度と試さない（雲なしのまま）。
bool Application::AtmosphereCloudsEnsure()
{
    if (!m_atmo) return false;
    AtmoHost& h = *m_atmo;
    if (h.clouds.IsReady()) return true;
    if (h.cloudsInitFailed || !m_graphicsDevice || !m_srvHeap) return false;
    bool ok = false;
    try { ok = h.clouds.Initialize(*m_graphicsDevice, m_srvHeap.get(), PathResolver::ShaderDirW()); }
    catch (const std::exception& e) { Logger::Warn("雲: 初期化に失敗した（{}）。雲なしのまま", e.what()); }
    if (!ok) { h.cloudsInitFailed = true; return false; }
    return true;
}

// 風で流れた距離[km]。時間は 1 フレームに 1 回だけ進める（本体・影・環境光のどれが先に呼んでも同じ値）。
// 決定論撮影では 0 固定（同じ絵になる）。実時間の大きなヒッチは丸める。
void Application::AtmosphereCloudWind(float outKm[3])
{
    AtmoHost& h = *m_atmo;
    const CloudSettings& cs = m_scene->GetAtmosphereSettings().clouds;
    if (h.cloudTimeFrame != m_framesSinceStart)
    {
        h.cloudTimeFrame = m_framesSinceStart;
        if (m_deterministicCapture) h.cloudTime = 0.0;
        else h.cloudTime += std::min(static_cast<double>(m_gameClock.GetDeltaTime()), 0.25);
    }
    const float wind = cs.windSpeed * static_cast<float>(h.cloudTime) * 0.001f;   // km
    const float yaw = cs.windDirDeg * 3.14159265f / 180.0f;
    outKm[0] = -std::sin(yaw) * wind;   // 雲は風下へ流れる＝ノイズを引く座標は風上側へずらす
    outKm[1] = 0.0f;
    outKm[2] = -std::cos(yaw) * wind;
}

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
    if (!AtmosphereCloudsEnsure()) return;
    // 解像度が変わるときは履歴・粗い RT を作り直すので、使用中でないことを保証する（まれ）
    if (h.clouds.NeedsResize(view.width, view.height)) m_commandQueue->WaitIdle();

    CloudRenderer::FrameInfo fi{};
    fi.width = view.width;
    fi.height = view.height;
    AtmosphereCloudWind(fi.windOffsetKm);
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

// ---------------------------------------------------------------------------
// 雲の影（影マスク t11 へ掛ける）。深度プリパスの区間から呼ぶ。
// ---------------------------------------------------------------------------
bool Application::AtmosphereCloudShadowWanted(const ViewDesc& view) const
{
    if (!view.primary || !m_scene || !AtmosphereSkyUsable(view) || !m_drawSkybox || !(m_skyboxIntensity > 0.0f)) return false;
    const CloudSettings& cs = m_scene->GetAtmosphereSettings().clouds;
    return cs.enabled && cs.shadows && cs.shadowStrength > 0.0f;
}

u32 Application::AtmosphereCloudShadow(const ViewDesc& view, const RenderPassContext& ctx, u32 depthSrvIndex, u32 prevMaskSrvIndex)
{
    if (!AtmosphereCloudShadowWanted(view) || depthSrvIndex == DescriptorHeap::kInvalidIndex) return prevMaskSrvIndex;
    if (!AtmosphereCloudsEnsure()) return prevMaskSrvIndex;
    AtmoHost& h = *m_atmo;
    const CloudSettings& cs = m_scene->GetAtmosphereSettings().clouds;
    // 影マスクの大きさが変わるときは作り直すので、使用中でないことを保証する（まれ）
    if (h.clouds.NeedsMaskResize(view.width, view.height)) m_commandQueue->WaitIdle();

    float wind[3];
    AtmosphereCloudWind(wind);
    if (ctx.depth) ctx.depth->Require(*ctx.cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);   // RT 影 / コンタクトシャドウと同じ（PS で深度を読む）

    D3D12_GPU_DESCRIPTOR_HANDLE prev{};
    if (prevMaskSrvIndex != DescriptorHeap::kInvalidIndex && prevMaskSrvIndex != m_ssaoWhiteSrvIndex)
        prev = m_srvHeap->GetGpuHandle(prevMaskSrvIndex);   // RT 影 / コンタクトシャドウの結果（無ければ白 = 掛けない）

    if (m_gpuTimer) m_gpuTimer->Begin(ctx.native, GpuTimer::CloudShadow);
    const u32 idx = h.clouds.RecordShadow(ctx.native, ctx.frameIndex, *m_graphicsDevice, cs, wind, h.renderer.CbAddress(ctx.frameIndex),
                                          h.renderer.LutSrvGpu(), m_srvHeap->GetGpuHandle(depthSrvIndex), prev, view.width, view.height);
    if (m_gpuTimer) m_gpuTimer->End(ctx.native, GpuTimer::CloudShadow);
    if (idx == DescriptorHeap::kInvalidIndex) return prevMaskSrvIndex;
    ++h.cloudShadowFrames;
    return idx;
}

// ---------------------------------------------------------------------------
// 環境光への焼き込み。AtmosphereRenderer::RecordSkyCube の直後（IBL の再ベイクの前）から呼ぶ。
// ---------------------------------------------------------------------------
void Application::AtmosphereCloudsToSkyCube(ID3D12GraphicsCommandList* cmd, u32 frameIndex)
{
    if (!m_atmo || !m_scene || !cmd) return;
    const CloudSettings& cs = m_scene->GetAtmosphereSettings().clouds;
    if (!cs.enabled || !AtmosphereCloudsEnsure()) return;
    AtmoHost& h = *m_atmo;
    float wind[3];
    AtmosphereCloudWind(wind);
    h.clouds.RecordSkyCubeClouds(cmd, frameIndex, *m_graphicsDevice, cs, wind, h.renderer.CbAddress(frameIndex), h.renderer.LutSrvGpu(),
                                 h.renderer.SkyCubeResource());
}

} // namespace dx12e
