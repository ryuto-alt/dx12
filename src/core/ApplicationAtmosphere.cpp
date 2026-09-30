// ===========================================================================
// 物理ベース大気 A1: 時刻系 / AtmosphereRenderer の遅延確保 / IBL 増分再ベイク / 空・AP の呼び出し。
// ---------------------------------------------------------------------------
// 本体は renderer/atmosphere/。ここは Application 側の糊だけ。
// ★enabled=false（既定）の間は AtmosphereRenderer を作らず、描画コマンドも 1 命令も変わらない（決定論スクショ差分 0）。
// 「1 本の時刻系」: timeOfDay → 太陽の向き(天文計算)→ 太陽の色と照度(透過率 LUT と同じ媒質の数値積分)→ 平行光(影・CSM・RT 影・DDGI の直接光)
//                     → 空(Sky-View LUT)→ 環境キューブ → IBL(irradiance / prefilter の増分再ベイク)→ DDGI の空項(IBL の irradiance キューブを読む)
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/AtmosphereHost.h"
#include "ecs/EditorFlags.h"
#include "renderer/ViewDesc.h"
#include "renderer/atmosphere/AtmospherePasses.h"

namespace dx12e
{
using namespace DirectX;
using namespace atmosphere;

namespace
{

// 最初の（無効でない）平行光。描画側（ApplicationRender.cpp の太陽の決め方）と同じ規則。
entt::entity FirstSun(entt::registry& reg)
{
    auto view = reg.view<DirectionalLight>();
    for (auto e : view)
        if (!eflags::IsDisabled(reg, e)) return e;
    return entt::null;
}

uint64_t Fnv(uint64_t h, const void* d, size_t n)
{
    const unsigned char* b = static_cast<const unsigned char*>(d);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// IBL の環境キューブを「大きく」作り直すべき設定の鍵。媒質・照度・色・単位。太陽の向きとカメラ高度は含めない（閾値で別に見る）。
uint64_t MakeIblKey(const AtmosphereGpuParams& g, const AtmosphereSettings& s, double unitScale)
{
    uint64_t h = HashMediumParams(g);
    h = Fnv(h, &s.sunIlluminance, sizeof(float));
    h = Fnv(h, &s.moonIlluminance, sizeof(float));
    h = Fnv(h, s.sunTint, sizeof(float) * 3);
    h = Fnv(h, s.moonTint, sizeof(float) * 3);
    h = Fnv(h, &s.nightSkyNits, sizeof(float));
    h = Fnv(h, &s.multiScatteringFactor, sizeof(float));
    h = Fnv(h, &unitScale, sizeof(double));
    const int drawMoon = s.drawMoon ? 1 : 0;
    h = Fnv(h, &drawMoon, sizeof(int));
    return h;
}

void StoreMatT(float* dst, const XMMATRIX& m)   // HLSL の mul(v, M) 用に転置して 16 float へ
{
    XMFLOAT4X4 f;
    XMStoreFloat4x4(&f, XMMatrixTranspose(m));
    std::memcpy(dst, &f, sizeof(float) * 16);
}

// 設定 + ホストの時刻系の結果 + ビュー → GPU 定数。
void BuildParams(AtmoHost& h, const AtmosphereSettings& sIn, const ViewDesc* view, const XMFLOAT3& camPos, float skyBoxIntensity,
                 AtmosphereGpuParams& out)
{
    AtmosphereSettings s = sIn;
    if (s.sunMode == 1) s.drawMoon = false;   // 太陽の向きを直接指定するモードでは月を使わない（光源の向きを勝手に反転しない）
    // 標高の下限 1 m（float の 6360 km 座標では 0.5 m 未満は量子化される。海面ちょうど・海面下でも壊れないように）
    const double camAltKm = std::max(1.0, static_cast<double>(camPos.y - s.seaLevelY)) * 0.001;
    const float cp[3] = {camPos.x, camPos.y, camPos.z};
    FillGpuParams(out, s, h.light, h.sunDir, camAltKm, h.unitScale, cp, h.starAngle, h.poleAxis);
    out.skyScale *= skyBoxIntensity;
    if (view)
    {
        StoreMatT(out.invViewProj, XMMatrixInverse(nullptr, XMLoadFloat4x4(&view->viewProj)));
        StoreMatT(out.invViewProjJ, XMMatrixInverse(nullptr, XMLoadFloat4x4(&view->viewProjJittered)));
    }
    else
    {
        StoreMatT(out.invViewProj, XMMatrixIdentity());
        StoreMatT(out.invViewProjJ, XMMatrixIdentity());
    }
}

} // namespace

// ---------------------------------------------------------------------------
bool Application::AtmosphereEnsure()
{
    if (m_atmo && m_atmo->renderer.IsReady()) return true;
    if (m_atmo && m_atmo->initFailed) return false;
    if (!m_graphicsDevice || !m_srvHeap || !m_commandQueue) return false;
    if (!m_atmo) m_atmo = std::make_shared<AtmoHost>();
    bool ok = false;
    try
    {
        ok = m_atmo->renderer.Initialize(*m_graphicsDevice, m_commandQueue->GetQueue(), m_srvHeap.get(),
                                         PathResolver::ShaderDirW());
    }
    catch (const std::exception& e)
    {
        Logger::Warn("物理大気: 初期化に失敗した（{}）。従来の空のまま", e.what());
    }
    if (!ok) { m_atmo->initFailed = true; return false; }
    return true;
}

bool Application::AtmosphereActive() const
{
    return m_atmo && m_atmo->renderer.IsReady() && m_scene && m_scene->GetAtmosphereSettings().enabled && m_atmo->timeValid;
}

bool Application::AtmosphereWantsEnvironment() const
{
    return AtmosphereActive() && m_scene->GetAtmosphereSettings().driveIBL;
}

void Application::AtmosphereShutdown()
{
    if (!m_atmo) return;
    m_atmo->renderer.Shutdown();
    m_atmo.reset();
}

// ---------------------------------------------------------------------------
// 時刻系（Application::Update から）。enabled=false のときは遷移（ON→OFF）の後始末だけ。
// dt は「進めてよい時間」（Editor / 一時停止 / 決定論撮影では 0 を渡すこと）。
// ---------------------------------------------------------------------------
void Application::UpdateAtmosphereTime(f32 dt)
{
    if (!m_scene) return;
    AtmosphereSettings& s = m_scene->GetAtmosphereSettings();

    if (!s.enabled)
    {
        if (m_atmo && m_atmo->enabledPrev)
        {
            // ON → OFF: 従来の空・従来の環境へ戻す（envMapPath の環境を次フレーム冒頭で焼き直す）
            m_atmo->enabledPrev = false;
            m_atmo->iblValid = false;
            m_atmo->iblRunning = false;
            m_atmo->timeValid = false;
            m_loadedSkyboxPath.clear();
            m_skyboxDirty = true;
        }
        return;
    }
    if (!AtmosphereEnsure()) return;
    AtmoHost& h = *m_atmo;

    const bool wantEnvNow = s.driveIBL;
    if (!h.enabledPrev || wantEnvNow != h.iblEnvWanted)
    {
        h.enabledPrev = true;
        h.iblEnvWanted = wantEnvNow;
        h.iblValid = false;
        h.iblRunning = false;
        h.forceLut = true;
        m_loadedSkyboxPath.clear();
        m_skyboxDirty = true;   // 環境の元が変わる → 次フレーム冒頭で全部焼く
    }

    // 時間経過（Play 中だけ。Stop でシーン JSON のスナップショットから元の時刻へ戻る）
    if (dt > 0.0f && s.timeSpeed != 0.0f)
        s.timeOfDay = AdvanceTimeOfDay(s.timeOfDay, s.timeSpeed, dt);

    auto& reg = m_scene->GetRegistry();
    const entt::entity sun = FirstSun(reg);
    DirectionalLight* dl = (sun != entt::null) ? &reg.get<DirectionalLight>(sun) : nullptr;

    // 太陽の向き
    Vec3d sunDir;
    if (s.sunMode == 1 && dl)
    {
        const XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&dl->direction));
        XMFLOAT3 f; XMStoreFloat3(&f, XMVectorNegate(d));
        sunDir = {f.x, f.y, f.z};
    }
    else
        sunDir = SunDirectionFromTime(s);
    h.sunDir = sunDir;

    AtmosphereSettings sl = s;
    if (s.sunMode == 1) sl.drawMoon = false;
    const XMFLOAT3 cam = m_camera ? m_camera->GetPosition() : XMFLOAT3{0, 0, 0};
    h.camAltKm = std::max(1.0, static_cast<double>(cam.y - s.seaLevelY)) * 0.001;   // 下限 1 m（BuildParams と同じ）
    h.unitScale = (m_scene->GetPostSettings().lightingUnits == 1) ? 1.0 : static_cast<double>(kClassicUnitScale);
    h.light = ComputeSkyLight(sl, sunDir, h.camAltKm);
    h.poleAxis = CelestialPoleAxis(s.latitudeDeg, s.northYawDeg);
    h.starAngle = (static_cast<double>(s.timeOfDay) - 12.0) * 15.0 * kPi / 180.0;
    h.timeValid = true;

    // 平行光（太陽 / 月）を駆動する。値が変わったときだけ書く（Undo / 指紋への無駄な書き込みを避ける）。
    if (s.driveSun && dl)
    {
        if (s.sunMode == 0)
        {
            // 光が進む向き = 光源へ向かう向きの逆
            const XMFLOAT3 dir{static_cast<float>(-h.light.toLight.x), static_cast<float>(-h.light.toLight.y),
                               static_cast<float>(-h.light.toLight.z)};
            if (dir.x != dl->direction.x || dir.y != dl->direction.y || dir.z != dl->direction.z)
            {
                dl->direction = dir;
                dl->_prevRotInit = false;   // Transform 回転の差分追従をリセット（set_sun と同じ）
            }
        }
        const Rgb e = h.light.illuminanceGround;
        const double k = h.unitScale;
        const float r = static_cast<float>(e.r * k), g = static_cast<float>(e.g * k), b = static_cast<float>(e.b * k);
        const float mx = std::max(r, std::max(g, b));
        if (mx > 0.0f)
        {
            const XMFLOAT3 col{r / mx, g / mx, b / mx};
            if (col.x != dl->color.x || col.y != dl->color.y || col.z != dl->color.z) dl->color = col;
            if (mx != dl->intensity) dl->intensity = mx;
        }
        else if (dl->intensity != 0.0f)
            dl->intensity = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// 主ビューの先頭（影パスより前）。LUT を更新し、IBL を増分で焼き直す。副ビューでは呼ばない。
// ---------------------------------------------------------------------------
void Application::AtmosphereRecordFrame(const ViewDesc& view, u32 frameIndex, ID3D12GraphicsCommandList* cmd)
{
    if (!AtmosphereActive() || !view.primary || !cmd) return;
    AtmoHost& h = *m_atmo;
    const AtmosphereSettings& s = m_scene->GetAtmosphereSettings();

    h.renderer.BeginFrameTimers(frameIndex);
    BuildParams(h, s, &view, view.position, m_skyboxIntensity, h.params);
    h.paramsValid = true;
    h.recordedFrame = m_framesSinceStart;
    h.renderer.Update(cmd, frameIndex, h.params, h.forceLut);
    h.forceLut = false;

    // ---- IBL の増分再ベイク ----
    if (!s.driveIBL || !h.iblValid || m_loadedSkyboxPath != kAtmosphereSkyPath || !m_iblBaker || !m_iblBaker->HasEnvironment())
        return;

    const f32 dtReal = m_gameClock.GetDeltaTime();
    h.iblSinceStart += m_deterministicCapture ? 1.0e6 : static_cast<double>(dtReal);

    if (!h.iblRunning)
    {
        const double dot = std::clamp(Dot(h.sunDir, h.iblSunDir), -1.0, 1.0);
        const double angDeg = std::acos(dot) * 180.0 / kPi;
        h.lastSunDeltaDeg = static_cast<float>(angDeg);
        const double dAltM = std::fabs(h.camAltKm - h.iblAltKm) * 1000.0;
        const uint64_t key = MakeIblKey(h.params, s, h.unitScale);
        const bool big = key != h.iblKey || angDeg > 5.0 || m_deterministicCapture;
        const double thr = std::max(static_cast<double>(s.iblRebakeThresholdDeg), 1.0e-4);
        const bool need = big || angDeg > thr || dAltM > std::max(50.0, 0.1 * h.iblAltKm * 1000.0);
        const double minInterval = 1.0 / std::max(static_cast<double>(s.iblRebakeMaxHz), 0.1);
        if (need && (big || h.iblSinceStart >= minInterval))
        {
            h.renderer.RecordSkyCube(cmd, frameIndex);
            h.iblSunDir = h.sunDir;
            h.iblAltKm = h.camAltKm;
            h.iblKey = key;
            h.iblRunning = true;
            h.iblStage = 0;
            h.iblBurst = big;
            h.iblSinceStart = 0.0;
            ++h.iblRebakeCount;
            if (big) ++h.iblBigCount;
        }
    }
    if (h.iblRunning)
    {
        const u32 total = IBLBaker::kRebakeStageCount;
        const u32 n = h.iblBurst ? (total - h.iblStage) : 1u;
        ID3D12Resource* cube = h.renderer.SkyCubeResource();
        auto& renderer = h.renderer;
        bool preOpen = false;
        for (u32 i = 0; i < n; ++i)
        {
            const u32 st = h.iblStage;
            const bool irr = (st == 0);
            if (irr) renderer.TimerBegin(cmd, frameIndex, AtmosphereRenderer::ScopeIblIrr);
            else if (!preOpen) { renderer.TimerBegin(cmd, frameIndex, AtmosphereRenderer::ScopeIblPre); preOpen = true; }
            const bool ok = m_iblBaker->RebakeStage(*m_graphicsDevice, cmd, *m_srvHeap, cube, st, frameIndex);
            if (irr) renderer.TimerEnd(cmd, frameIndex, AtmosphereRenderer::ScopeIblIrr);
            if (!ok)
            {
                // 状態が合わない（環境の作り直しが要る）→ 全部焼き直す経路（次フレーム冒頭の WaitIdle 込み）へ倒す
                if (preOpen) renderer.TimerEnd(cmd, frameIndex, AtmosphereRenderer::ScopeIblPre);
                h.iblRunning = false;
                h.iblValid = false;
                m_loadedSkyboxPath.clear();
                m_skyboxDirty = true;
                return;
            }
            ++h.iblStage;
        }
        if (preOpen) renderer.TimerEnd(cmd, frameIndex, AtmosphereRenderer::ScopeIblPre);
        if (h.iblStage >= total) h.iblRunning = false;
    }
}

// ---------------------------------------------------------------------------
bool Application::AtmosphereSkyUsable(const ViewDesc& view) const
{
    if (!AtmosphereActive() || !m_atmo->paramsValid) return false;
    if (!view.Has(kViewSkybox)) return false;
    if (m_atmo->recordedFrame != m_framesSinceStart) return false;   // 今フレームの主ビューが LUT を作っていない
    return true;
}

void Application::AtmosphereDrawSky(const ViewDesc& view, const RenderPassContext& ctx)
{
    if (!AtmosphereSkyUsable(view) || !m_drawSkybox || !(m_skyboxIntensity > 0.0f)) return;
    AtmoHost& h = *m_atmo;
    const u32 frameIndex = ctx.frameIndex;
    u32 slot = frameIndex;
    if (!view.primary)
    {
        AtmosphereGpuParams p{};
        BuildParams(h, m_scene->GetAtmosphereSettings(), &view, view.position, m_skyboxIntensity, p);
        slot = frameIndex + 3u;
        h.renderer.UploadParams(slot, p);
    }
    AtmosphereSkyPass::Inputs si{};
    si.atmo = &h.renderer;
    si.sceneRtv = view.sceneColor->GetRtv();
    si.depthDsv = view.depthDsv;
    si.sceneColor = view.sceneColor->GetResource();
    if (view.depthSrvIndex != DescriptorHeap::kInvalidIndex) si.depthSrv = m_srvHeap->GetGpuHandle(view.depthSrvIndex);
    si.width = view.width;
    si.height = view.height;
    si.cbSlot = slot;
    AtmosphereSkyPass(si).Execute(ctx);
}

void Application::AtmosphereDrawAerialPerspective(const ViewDesc& view, const RenderPassContext& ctx, u32 depthSrvIndex)
{
    if (!AtmosphereSkyUsable(view) || !view.primary) return;   // AP の froxel は主ビューのカメラ向き。副ビューには掛けない
    const AtmosphereSettings& s = m_scene->GetAtmosphereSettings();
    if (!s.aerialPerspective || s.apStrength <= 0.0f) return;
    if (depthSrvIndex == DescriptorHeap::kInvalidIndex) return;
    AtmosphereApPass::Inputs ai{};
    ai.atmo = &m_atmo->renderer;
    ai.sceneRtv = view.sceneColor->GetRtv();
    ai.sceneColor = view.sceneColor->GetResource();
    ai.depthSrv = m_srvHeap->GetGpuHandle(depthSrvIndex);
    ai.width = view.width;
    ai.height = view.height;
    ai.cbSlot = ctx.frameIndex;
    AtmosphereApPass(ai).Execute(ctx);
}

// ---------------------------------------------------------------------------
// LoadSkyboxIfNeeded の先頭から。大気が環境の元（enabled && driveIBL）なら、LUT と環境キューブを作って IBL を焼き、true を返す。
// 呼び出し側（BeginRenderFrame）が専用 cmdList + WaitIdle で包んでいる。
// ---------------------------------------------------------------------------
bool Application::LoadAtmosphereEnvironment(ID3D12GraphicsCommandList* cmd)
{
    if (!AtmosphereWantsEnvironment() || !m_iblBaker || !m_srvHeap) return false;
    if (m_loadedSkyboxPath == kAtmosphereSkyPath && m_iblReady && m_iblBaker->HasEnvironment()) return true;

    AtmoHost& h = *m_atmo;
    const AtmosphereSettings& s = m_scene->GetAtmosphereSettings();

    // 既存の IBL / 環境を解放（LoadSkyboxIfNeeded と同じ手順）
    if (m_iblReady && m_srvHeap)
        m_srvHeap->FreeBlock(m_iblBaker->GetSrvBlockStart(), m_iblBaker->GetSrvBlockCount());
    if (m_envCubeSrvIndex != DescriptorHeap::kInvalidIndex && m_srvHeap)
        m_srvHeap->Free(m_envCubeSrvIndex);
    m_envCubeSrvIndex = DescriptorHeap::kInvalidIndex;
    m_envCubeTex.reset();
    m_iblReady = false;

    // LUT（全部）+ 環境キューブ
    const XMFLOAT3 cam = m_camera ? m_camera->GetPosition() : XMFLOAT3{0, 0, 0};
    BuildParams(h, s, nullptr, cam, m_skyboxIntensity, h.params);
    h.paramsValid = true;
    const u32 slot = m_swapChain ? m_swapChain->GetCurrentBackBufferIndex() : 0u;
    h.renderer.Update(cmd, slot, h.params, true);
    h.forceLut = false;
    h.renderer.RecordSkyCube(cmd, slot);

    // 派生（irradiance / prefilter / BRDF LUT）
    m_iblBaker->Bake(*m_graphicsDevice, cmd, *m_srvHeap, h.renderer.SkyCubeResource());
    m_iblReady = m_iblBaker->IsValid();
    m_loadedSkyboxPath = kAtmosphereSkyPath;

    h.iblValid = m_iblReady;
    h.iblRunning = false;
    h.iblSunDir = h.sunDir;
    h.iblAltKm = h.camAltKm;
    h.iblKey = MakeIblKey(h.params, s, h.unitScale);
    h.iblSinceStart = 0.0;
    Logger::Info("物理大気: 環境（空キューブ {}^2x6 → IBL）を焼いた", AtmosphereRenderer::kSkyCubeSize);
    return true;
}

} // namespace dx12e
