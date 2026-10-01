// ===========================================================================
// DXR パストレーサー(リファレンスレンダー)の Application 側の糊。設計は PathTracerHost.h / docs/PATH_TRACER.md。
//
//   ★既定 OFF・遅延確保。要求(MCP render_reference / エディタの「リファレンスレンダー」)が来るまで何もしない。
//   ★通常の描画経路には触れない。ジョブが走っている間だけ、PrepareFrame の末尾で
//     「GPU 時間の予算つき」のディスパッチをフレームのコマンドリストへ足す。
//   ★スナップショット: 要求を受けたフレームの描画リスト(m_drawItems)から専用の BLAS / TLAS / 材質表を組む。
//     ジョブの途中でシーンが切り替わる / メッシュが解放されると中止する(専用 TLAS は解放されたバッファを指せないため)。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/PathTracerHost.h"
#include "core/AtmosphereHost.h"   // 物理大気(A1): m_atmo(SRV ブロック先頭を PT の環境へ渡す)
#include "renderer/pt/PtImageIO.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <unordered_map>

namespace dx12e
{
using namespace DirectX;

namespace
{
const char* kPhaseNames[] = {"idle", "requested", "preparing", "running", "finalizing", "done", "failed", "cancelled"};

float Luma3(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

std::string NowStamp()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

// 材質の 8bit 量子化を戻す(Forward.hlsl の packedTint / packedEmissive の読み方と同じ)
void UnpackTint(u32 packed, float out[3], float& opacity)
{
    out[0] = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
    out[1] = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<float>(packed & 0xFF) / 255.0f;
    opacity = static_cast<float>((packed >> 24) & 0xFF) / 255.0f;
}
void UnpackEmissiveColor(u32 packed, float out[3])
{
    if (packed == 0u) { out[0] = out[1] = out[2] = 0.0f; return; }
    const float n = static_cast<float>((packed >> 24) & 0xFF) / 255.0f;
    const float k = n * n * 64.0f;
    out[0] = static_cast<float>((packed >> 16) & 0xFF) / 255.0f * k;
    out[1] = static_cast<float>((packed >> 8) & 0xFF) / 255.0f * k;
    out[2] = static_cast<float>(packed & 0xFF) / 255.0f * k;
}
} // namespace

const char* PtHost::PhaseName(Phase p) { return kPhaseNames[static_cast<int>(p)]; }

// ---------------------------------------------------------------------------
//  要求 / キャンセル
// ---------------------------------------------------------------------------
bool PtHost::Request(Application& app, const PtRequest& r, std::string* err)
{
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (Busy()) return fail("リファレンスレンダーが実行中(render_reference_status で進捗、render_reference_cancel で中止)");
    if (!app.m_graphicsDevice || !app.m_scene) return fail("エンジンが未初期化");
    if (!app.m_graphicsDevice->SupportsInlineRaytracing() || !app.m_graphicsDevice->SupportsDynamicResources())
        return fail("この GPU は DXR 1.1(inline RayQuery)と SM 6.6 / Resource Binding Tier 3 に対応していない");
    if (r.width < 1 || r.height < 1 || r.width > 16384 || r.height > 16384) return fail("size が範囲外(1..16384)");
    if (static_cast<uint64_t>(r.width) * r.height > 100ull * 1000 * 1000) return fail("解像度が大きすぎる(1 億画素まで)");
    if (r.spp < 1) return fail("spp は 1 以上");
    if (r.bounces < 1 || r.bounces > 64) return fail("bounces は 1..64");
    if (app.m_camera && app.m_camera->IsOrthographic() && r.useSceneCamera)
        return fail("正射カメラのビューは未対応(camera を position / target で指定するか、透視カメラにする)");
    if (app.m_editorCtx && app.m_editorCtx->view2D && r.useSceneCamera)
        return fail("2D ビューは未対応(camera を指定するか 3D ビューへ)");

    req = r;
    ++jobId;
    phase = Phase::Requested;
    error.clear();
    message = "要求を受け付けた。次のフレームで開始する";
    files.clear();
    outputBase = ResolveOutputBase(app);   // 要求時に確定(途中経過のプレビューと最終出力が同じ場所になる)
    samplesDone = 0;
    stats = pt::PtStatsOut{};
    gpuMsTotal = 0.0;
    snap = PtSnapshotInfo{};
    sceneStats = pt::BuilderStats{};
    cancelRequested = false;
    saveOnCancel = false;
    truncated = false;
    previewPath.clear();
    previewRequested = false;
    settleFrames = 0;
    terminalFrames = 0;
    tRequest = std::chrono::steady_clock::now();
    return true;
}

void PtHost::Cancel(bool save)
{
    if (!Busy()) return;
    cancelRequested = true;
    saveOnCancel = save;
}

void PtHost::Invalidate(const char* reason)
{
    if (phase == Phase::Preparing || phase == Phase::Running || phase == Phase::Requested)
        FailJob(std::string("中止: ") + reason);
}

void PtHost::FailJob(const std::string& why)
{
    error = why;
    message = why;
    phase = Phase::Failed;
    tEnd = std::chrono::steady_clock::now();
    if (tracer && tracer->HasJob()) tracer->Cancel();
    Logger::Warn("リファレンスレンダー: {}", why);
}

void PtHost::ReleaseOwnedSrvs()
{
    if (ownedSrvHeap) for (uint32_t i : ownedSrvs) ownedSrvHeap->Free(i);
    ownedSrvs.clear();
}

void PtHost::Shutdown()
{
    if (tracer) tracer->Shutdown();
    tracer.reset();
    scene.reset();
    ReleaseOwnedSrvs();
    phase = Phase::Idle;
}

bool PtHost::EnsureTracer(Application& app)
{
    if (tracer && tracer->IsReady()) return true;
    if (initTried && !tracer) return false;
    initTried = true;
    auto t = std::make_unique<pt::PathTracer>();
    std::string err;
    if (!t->Initialize(app.m_graphicsDevice->GetDevice(), app.m_commandQueue->GetQueue(), PathResolver::ShaderDirW(), &err))
    {
        initError = err;
        Logger::Warn("パストレーサー: 初期化に失敗: {}", err);
        return false;
    }
    tracer = std::move(t);
    return true;
}

std::string PtHost::ResolveOutputBase(Application& app) const
{
    namespace fs = std::filesystem;
    if (!req.outputBase.empty())
    {
        std::string b = req.outputBase;
        // 拡張子が付いていたら外す(.pfm / .exr / .png / .json)
        for (const char* ext : {".pfm", ".exr", ".png", ".json"})
        {
            const size_t n = std::strlen(ext);
            if (b.size() > n && _stricmp(b.c_str() + b.size() - n, ext) == 0) { b.resize(b.size() - n); break; }
        }
        return b;
    }
    (void)app;
    fs::path root;
    try { root = fs::path(PathResolver::AssetsDir()).parent_path().parent_path(); } catch (...) {}
    if (root.empty() || !fs::exists(root)) root = fs::temp_directory_path() / "UnoEngine";
    return (root / ".dx12" / "pt" / ("render_" + NowStamp())).generic_string();
}

// ---------------------------------------------------------------------------
//  スナップショット(描画リスト → pt::SceneBuilder)
// ---------------------------------------------------------------------------
// 描画項目 1 つ・サブメッシュ 1 つの材質を、フォワード(RenderSceneMeshes)と同じ優先順で解決する。
// パストレーサーのスナップショットと RT(DDGI のヒットシェーディング)の【共有】実装:
//   両者のヒットの色 / 自己発光が同じ値になることをここで保証する。
void PtHost::ResolveHitMaterial(Application& app, const DrawItem& it, u32 mi, ID3D12GraphicsCommandList* cmd,
                                bool quantizeLikeForward, pt::MaterialGpu& m, float& emLuma)
{
    const MeshRenderer& r = *it.renderer;
    Mesh* mesh = r.meshes[mi];
    emLuma = 0.0f;
    m = pt::MaterialGpu{};
    // ---- 材質(Forward の RenderSceneMeshes と同じ優先順で解決)----
    const Material* mat = mesh->GetMaterial();
    const AlphaParams alphaP = ResolveAlphaParams(mat, r.alphaModeOverride, r.alphaCutoffOverride, r.opacity);
    const bool wantBlend = (alphaP.mode == AlphaMode::Blend)
                        || (alphaP.mode == AlphaMode::Opaque && r.opacity < 0.999f);
    const bool wantMask = (alphaP.mode == AlphaMode::Mask);

    const MaterialAssetManager::Entry* matAsset = nullptr;
    if (r.HasMaterialAsset(mi) && app.m_materialAssetManager)
    {
        const auto* loaded = app.m_materialAssetManager->GetOrLoad(MeshRenderer::SafeGetOverride(r.materialAsset, mi), cmd);
        if (loaded && loaded->valid) matAsset = loaded;
    }
    const u32 overrideBlock = app.EnsureMaterialOverrideSrv(it.e, mi, r, mat, cmd);
    u32 block = 0xFFFFFFFFu;
    if (matAsset)                                        block = matAsset->srvBlockStart;
    else if (overrideBlock != 0xFFFFFFFFu)               block = overrideBlock;
    else if (mat && mat->srvBlockIndex != 0xFFFFFFFFu)   block = mat->srvBlockIndex;

    m.albedoSrv = m.normalSrv = m.mrSrv = m.emissiveSrv = pt::kNoIndex;
    u32 pbrFlags = 0;
    float metallic, roughness;
    if (matAsset)
    {
        metallic  = (r.overrideMetallic  >= 0.0f) ? r.overrideMetallic  : matAsset->data.metallic;
        roughness = (r.overrideRoughness >= 0.0f) ? r.overrideRoughness : matAsset->data.roughness;
        if (matAsset->hasNormalTex)   pbrFlags |= 1u;
        if (matAsset->hasMRTex)       pbrFlags |= 2u;
        if (matAsset->hasEmissiveTex) pbrFlags |= kPbrFlagEmissiveTex;
    }
    else
    {
        metallic  = (r.overrideMetallic  >= 0.0f) ? r.overrideMetallic  : (mat ? mat->defaultMetallic : 0.0f);
        roughness = (r.overrideRoughness >= 0.0f) ? r.overrideRoughness : (mat ? mat->defaultRoughness : 0.5f);
        const bool ovBlockOk = (overrideBlock != 0xFFFFFFFFu);
        const bool ovNormal = ovBlockOk && !MeshRenderer::SafeGetOverride(r.overrideNormalTexture, mi).empty();
        const bool ovMR     = ovBlockOk && !MeshRenderer::SafeGetOverride(r.overrideMetalRoughnessTexture, mi).empty();
        const bool ovEmis   = ovBlockOk && !MeshRenderer::SafeGetOverride(r.overrideEmissiveTexture, mi).empty();
        if (ovNormal || (mat && mat->normalMapTexture))      pbrFlags |= 1u;
        if (ovMR || (mat && mat->metalRoughnessTexture))     pbrFlags |= 2u;
        if (ovEmis || (mat && mat->emissiveTexture))         pbrFlags |= kPbrFlagEmissiveTex;
    }
    if (block != 0xFFFFFFFFu)
    {
        m.albedoSrv = block;
        m.normalSrv = block + 1;
        m.mrSrv = block + 2;
        m.emissiveSrv = block + 3;
    }
    else
    {
        if (mat && mat->albedoTexture && mat->albedoTexture->GetSrvIndex() != 0xFFFFFFFFu)
            m.albedoSrv = mat->albedoTexture->GetSrvIndex();
        pbrFlags &= ~(1u | 2u | kPbrFlagEmissiveTex);   // ブロックが無ければ 2〜4 枚目は存在しない
    }
    m.metallic = metallic;
    m.roughness = roughness;
    m.flags = ((pbrFlags & 1u) ? pt::kMatNormalMap : 0u)
            | ((pbrFlags & 2u) ? pt::kMatMrTex : 0u)
            | ((pbrFlags & kPbrFlagEmissiveTex) ? pt::kMatEmissiveTex : 0u);

    // 色ティント + 不透明度 + アルファテストの閾値
    {
        const XMFLOAT4 tc = r.hasColorTint ? r.colorTint : XMFLOAT4{1.0f, 1.0f, 1.0f, 1.0f};
        if (quantizeLikeForward)
        {
            auto q = [](f32 v) { return static_cast<u32>(static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f)); };
            const u32 rgb = (q(tc.x) << 16) | (q(tc.y) << 8) | q(tc.z);
            const u32 packed = PackTintWithOpacity(rgb, alphaP.opacity);
            UnpackTint(packed, m.tint, m.opacity);
            const u32 fl = PackAlphaTestFlags(0u, alphaP);
            m.alphaCutoff = static_cast<float>((fl >> 8) & 0xFFu) / 255.0f;
        }
        else
        {
            m.tint[0] = tc.x; m.tint[1] = tc.y; m.tint[2] = tc.z;
            m.opacity = alphaP.opacity;
            m.alphaCutoff = alphaP.cutoff;
        }
    }
    m.alphaMode = wantBlend ? 2u : (wantMask ? 1u : 0u);

    // 自己発光
    {
        XMFLOAT3 baseColor = mat ? mat->emissiveColor : XMFLOAT3{0.0f, 0.0f, 0.0f};
        f32 baseIntensity = mat ? mat->emissiveIntensity : 0.0f;
        if (matAsset)
        {
            baseColor = {matAsset->data.emissiveColor[0], matAsset->data.emissiveColor[1], matAsset->data.emissiveColor[2]};
            baseIntensity = matAsset->data.emissiveIntensity;
        }
        const EmissiveParams ep = ResolveEmissiveParams(baseColor, baseIntensity, r.overrideEmissiveColor, r.overrideEmissiveIntensity);
        if (ep.intensity > 0.0f)
        {
            if (quantizeLikeForward) UnpackEmissiveColor(PackEmissive(ep), m.emissive);
            else { m.emissive[0] = ep.color.x * ep.intensity; m.emissive[1] = ep.color.y * ep.intensity; m.emissive[2] = ep.color.z * ep.intensity; }
            emLuma = Luma3(m.emissive);
        }
    }

    // UV 変換(連番アニメ > スクロール > 恒等)
    m.uvScaleOffset[0] = 1.0f; m.uvScaleOffset[1] = 1.0f;
    if (r.animFrames > 0)
    {
        const SpriteUvRect ur = ComputeFlipbookUvEx(r.animFrames, r.animFps, r.animCols, r.animRow, r.animRows, r.animMode, r._animT);
        m.uvScaleOffset[0] = ur.u1 - ur.u0; m.uvScaleOffset[1] = ur.v1 - ur.v0;
        m.uvScaleOffset[2] = ur.u0;         m.uvScaleOffset[3] = ur.v0;
    }
    else if (r.uvScrollU != 0.0f || r.uvScrollV != 0.0f)
    {
        const float du = r.uvScrollU * r._animT, dv = r.uvScrollV * r._animT;
        m.uvScaleOffset[2] = du - std::floor(du);
        m.uvScaleOffset[3] = dv - std::floor(dv);
    }
}

bool PtHost::Snapshot(Application& app, ID3D12GraphicsCommandList* cmd, std::string* err)
{
    const auto t0 = std::chrono::steady_clock::now();
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    auto& reg = app.m_scene->GetRegistry();
    GraphicsDevice& dev = *app.m_graphicsDevice;

    ReleaseOwnedSrvs();
    scene = std::make_unique<pt::SceneBuilder>();
    if (!scene->Init(dev.GetDevice(), PathResolver::ShaderDirW())) return fail("SceneBuilder を初期化できない");
    ownedSrvHeap = app.m_srvHeap.get();
    scene->SetRawSrvFactory([this](ID3D12Resource* buf, uint64_t bytes) -> uint32_t
    {
        if (!ownedSrvHeap) return pt::kNoIndex;
        const uint32_t idx = ownedSrvHeap->AllocateIndex();
        if (idx == DescriptorHeap::kInvalidIndex) return pt::kNoIndex;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = static_cast<UINT>(bytes / 4);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        Microsoft::WRL::ComPtr<ID3D12Device> d;
        buf->GetDevice(IID_PPV_ARGS(&d));
        d->CreateShaderResourceView(buf, &srv, ownedSrvHeap->GetCpuHandle(idx));
        ownedSrvs.push_back(idx);
        return idx;
    });

    job = pt::JobDesc{};
    job.width = req.width; job.height = req.height;
    job.spp = req.spp; job.bounces = req.bounces; job.seed = req.seed;
    job.maxRadiance = req.maxRadiance;
    job.flags = (req.russianRoulette ? pt::kFlagRR : 0u)
              | (req.physicalFalloff ? pt::kFlagPhysFalloff : 0u)
              | (req.forceLambert ? pt::kFlagForceLambert : 0u)
              | (req.noNormalMaps ? pt::kFlagNoNormalMap : 0u);
    job.tileSize = req.tileSize;
    job.samplesPerDispatch = req.samplesPerDispatch;

    // ---- カメラ ----
    {
        pt::CameraDesc& c = job.cam;
        c.aspect = static_cast<float>(req.width) / static_cast<float>(req.height);
        if (req.useSceneCamera && app.m_camera)
        {
            const XMMATRIX inv = XMMatrixInverse(nullptr, app.m_camera->GetViewMatrix());
            XMFLOAT3 right, up, fwd, pos;
            XMStoreFloat3(&right, XMVector3Normalize(inv.r[0]));
            XMStoreFloat3(&up,    XMVector3Normalize(inv.r[1]));
            XMStoreFloat3(&fwd,   XMVector3Normalize(inv.r[2]));
            XMStoreFloat3(&pos, inv.r[3]);
            std::memcpy(c.right, &right, 12); std::memcpy(c.up, &up, 12);
            std::memcpy(c.fwd, &fwd, 12);     std::memcpy(c.pos, &pos, 12);
            c.tanHalfFovY = std::tan(app.m_camera->GetFovY() * 0.5f);
        }
        else
        {
            const XMVECTOR p = XMVectorSet(req.camPos[0], req.camPos[1], req.camPos[2], 0);
            const XMVECTOR t = XMVectorSet(req.camTarget[0], req.camTarget[1], req.camTarget[2], 0);
            XMVECTOR f = XMVector3Normalize(XMVectorSubtract(t, p));
            if (XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(t, p))) < 1e-10f) return fail("camera の position と target が同じ");
            XMVECTOR upRef = XMVectorSet(0, 1, 0, 0);
            if (std::fabs(XMVectorGetY(f)) > 0.9999f) upRef = XMVectorSet(0, 0, 1, 0);
            const XMVECTOR r = XMVector3Normalize(XMVector3Cross(upRef, f));   // 左手系
            const XMVECTOR u = XMVector3Cross(f, r);
            XMFLOAT3 right, up, fwd;
            XMStoreFloat3(&right, r); XMStoreFloat3(&up, u); XMStoreFloat3(&fwd, f);
            std::memcpy(c.right, &right, 12); std::memcpy(c.up, &up, 12);
            std::memcpy(c.fwd, &fwd, 12);
            std::memcpy(c.pos, req.camPos, 12);
            c.tanHalfFovY = std::tan(XMConvertToRadians(std::clamp(req.fovDeg, 1.0f, 170.0f)) * 0.5f);
        }
        c.lensRadius = req.lensRadius;
        c.focusDist = req.focusDist;
    }

    // ---- 太陽(最初の DirectionalLight。フォワードと同じ)と環境光 ----
    float ambient = 0.25f;   // DirectionalLight が無いときのフォワードの既定
    {
        auto dl = reg.view<const DirectionalLight>();
        if (!dl.empty())
        {
            const auto& d = dl.get<const DirectionalLight>(*dl.begin());
            XMFLOAT3 dir = d.direction;
            const XMVECTOR v = XMVector3Normalize(XMLoadFloat3(&dir));
            XMFLOAT3 toLight;
            XMStoreFloat3(&toLight, XMVectorNegate(v));
            std::memcpy(job.sun.toLight, &toLight, 12);
            job.sun.E[0] = d.color.x * d.intensity;
            job.sun.E[1] = d.color.y * d.intensity;
            job.sun.E[2] = d.color.z * d.intensity;
            job.sun.enabled = (Luma3(job.sun.E) > 0.0f);
            ambient = d.ambient;
        }
        const ShadowPcssSettings& pcss = app.m_scene->GetShadowPcssSettings();
        job.sun.tanRadius = (req.sunAngularRadiusDeg >= 0.0f)
            ? std::tan(XMConvertToRadians(std::min(req.sunAngularRadiusDeg, 45.0f)))
            : (pcss.enabled ? pcss.lightTanAngle : 0.0f);
    }
    const bool hasEnv = app.m_iblReady && app.m_iblBaker && app.m_iblBaker->HasEnvironment()
                     && app.m_envCubeSrvIndex != 0xFFFFFFFFu;
    // 物理大気(A1): 大気が有効なら環境は大気の LUT(従来の envCube / 一様な空より優先)。背景は SkyRadiance(skyboxIntensity は gSkyScale 側に込み)。
    // ライティングは AtEnvRadiance × iblIntensity(フォワードの IBL と同じ掛け方)。太陽の向き・色・強度は上で DirectionalLight から読んだ値(大気が駆動)。
    if (app.AtmosphereActive() && app.m_atmo->paramsValid)
    {
        job.env.atmosSrvBase = app.m_atmo->renderer.SrvBlockBase();
        job.env.cubeSrv = pt::kNoIndex;
        job.env.lightScale = app.m_iblIntensity;
        job.env.bgScale = (app.m_drawSkybox && req.background) ? 1.0f : 0.0f;
        job.env.nee = app.m_iblIntensity > 0.0f;
        if (req.sunAngularRadiusDeg < 0.0f)
            job.sun.tanRadius = std::tan(app.m_scene->GetAtmosphereSettings().sunAngularRadius);
    }
    else if (hasEnv)
    {
        job.env.cubeSrv = app.m_envCubeSrvIndex;
        job.env.lightScale = app.m_iblIntensity;
        job.env.bgScale = (app.m_drawSkybox && req.background) ? app.m_skyboxIntensity : 0.0f;
        job.env.nee = app.m_iblIntensity > 0.0f;
    }
    else
    {
        // 環境マップ無し = フォワードは定数の ambient(DirectionalLight.ambient)を「一様な空」として使う。
        job.env.cubeSrv = pt::kNoIndex;
        job.env.uniform[0] = job.env.uniform[1] = job.env.uniform[2] = ambient;
        job.env.lightScale = 1.0f;
        job.env.bgScale = 0.0f;
        job.env.nee = ambient > 0.0f;
    }

    // ---- 点 / スポット光 ----
    {
        auto addLight = [&](const pt::LightGpu& l) { scene->AddLight(l); ++snap.lights; };
        for (auto [e, pl, tf] : reg.view<const PointLight, const Transform>().each())
        {
            if (snap.lights >= 65536) break;
            const XMMATRIX w = (tf.parent != entt::null) ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            pt::LightGpu l{};
            XMFLOAT3 p; XMStoreFloat3(&p, w.r[3]);
            l.position[0] = p.x; l.position[1] = p.y; l.position[2] = p.z;
            l.range = pl.range;
            l.color[0] = pl.color.x * pl.intensity; l.color[1] = pl.color.y * pl.intensity; l.color[2] = pl.color.z * pl.intensity;
            l.type = 0.0f;
            l.direction[2] = 1.0f;
            l.cosOuter = -1.0f; l.cosInner = 1.0f;
            addLight(l);
        }
        for (auto [e, sl, tf] : reg.view<const SpotLight, const Transform>().each())
        {
            if (snap.lights >= 65536) break;
            const XMMATRIX w = (tf.parent != entt::null) ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            pt::LightGpu l{};
            XMFLOAT3 p; XMStoreFloat3(&p, w.r[3]);
            l.position[0] = p.x; l.position[1] = p.y; l.position[2] = p.z;
            l.range = sl.range;
            l.type = 1.0f;
            XMFLOAT3 dir; XMStoreFloat3(&dir, XMVector3Normalize(XMLoadFloat3(&sl.direction)));
            l.direction[0] = dir.x; l.direction[1] = dir.y; l.direction[2] = dir.z;
            const float outerDeg = std::max(sl.outerConeDeg, sl.innerConeDeg);
            l.cosInner = std::cos(XMConvertToRadians(sl.innerConeDeg));
            l.cosOuter = std::cos(XMConvertToRadians(outerDeg));
            l.sinOuter = std::sqrt(std::max(0.0f, 1.0f - l.cosOuter * l.cosOuter));
            l.color[0] = sl.color.x * sl.intensity; l.color[1] = sl.color.y * sl.intensity; l.color[2] = sl.color.z * sl.intensity;
            addLight(l);
        }
    }

    // ---- メッシュ / 材質 / インスタンス ----
    std::unordered_map<std::string, uint32_t> matCache;   // MaterialGpu のバイト列 → 材質表の添字(重複排除)
    auto addMaterial = [&](const pt::MaterialGpu& m) -> uint32_t
    {
        const std::string key(reinterpret_cast<const char*>(&m), sizeof(m));
        auto it = matCache.find(key);
        if (it != matCache.end()) return it->second;
        const uint32_t idx = scene->AddMaterial(m);
        matCache.emplace(key, idx);
        return idx;
    };

    // スキンドの変形後頂点(SkinningCompute の出力)。フレームごとに書き換わるので、後でジョブ専用バッファへ写す。
    const bool haveSkinning = (app.m_skinningCompute != nullptr) && app.m_skinningCompute->IsReady();
    if (haveSkinning) app.m_skinningCompute->BeginFrame();
    const u32 frameIndexSk = app.m_swapChain->GetCurrentBackBufferIndex();

    for (const DrawItem& it : app.m_drawItems)
    {
        if (!it.renderer) continue;
        ++snap.drawItems;
        const MeshRenderer& r = *it.renderer;
        const XMMATRIX world = XMLoadFloat4x4(&it.world);
        const bool skinnedItem = (it.skin != nullptr);
        const Terrain* terrain = reg.try_get<Terrain>(it.e);
        const bool splatTerrain = terrain && !terrain->layerSetPath.empty();
        const bool vgProxy = reg.all_of<VirtualGeometry>(it.e);

        for (u32 mi = 0; mi < static_cast<u32>(r.meshes.size()); ++mi)
        {
            Mesh* mesh = r.meshes[mi];
            if (!mesh) continue;
            const auto& vbv = mesh->GetVertexBuffer().GetView();
            const auto& ibv = mesh->GetIndexBufferLod(0).GetView();   // ★LOD0 固定(GT はカメラ距離で LOD を落とさない)
            const u32 indexCount = mesh->GetIndexCountLod(0);
            if (vbv.BufferLocation == 0 || ibv.BufferLocation == 0 || indexCount < 3 || vbv.StrideInBytes == 0)
            { ++snap.skippedNoMesh; continue; }
            if (skinnedItem && !haveSkinning) { ++snap.skippedSkinned; continue; }
            mesh->EnsureRaytracingSrvs(dev, *app.m_srvHeap);
            if (mesh->GetVbSrvIndex() == DescriptorHeap::kInvalidIndex || mesh->GetIbSrvIndex() == DescriptorHeap::kInvalidIndex)
            { ++snap.skippedNoMesh; continue; }

            XMMATRIX meshWorld = world;
            if (it.hasNodeAnim && mi < static_cast<u32>(r.meshNodeTransforms.size()))
                meshWorld = XMLoadFloat4x4(&r.meshNodeTransforms[mi]) * world;

            // ---- 幾何 ----
            pt::GeometryDesc gd;
            gd.key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mesh)) & 0x0000FFFFFFFFFFFFull)
                   | (static_cast<uint64_t>(mesh->GetGeometryVersion() & 0xFFFFu) << 48);
            gd.vbVA = vbv.BufferLocation;
            gd.vbStride = vbv.StrideInBytes;
            gd.vertexCount = vbv.SizeInBytes / vbv.StrideInBytes;
            gd.ibVA = ibv.BufferLocation;
            gd.indexCount = indexCount;
            gd.vbSrv = mesh->GetVbSrvIndex();
            gd.ibSrv = mesh->GetIbSrvIndex();
            const auto& cpuIdx = mesh->GetIndices();
            const auto& cpuPos = mesh->GetPositions();
            if (cpuIdx.size() == indexCount && cpuPos.size() == gd.vertexCount)
            {
                gd.cpuPositions = cpuPos.data();
                gd.cpuIndices = cpuIdx.data();
            }
            const uint32_t geo = scene->AddGeometry(gd);

            // スキンド: compute スキニングで変形後位置を作る(RT の TLAS 構築と同じキー / ポーズハッシュ)。
            D3D12_GPU_VIRTUAL_ADDRESS deformedVa = 0;
            if (skinnedItem)
            {
                const auto* skelAnim = reg.try_get<SkeletalAnimation>(it.e);
                if (!skelAnim || !skelAnim->animator) { ++snap.skippedSkinned; continue; }
                const auto& mats = skelAnim->animator->GetSkinningMatrices();
                u64 poseHash = 1469598103934665603ull;
                {
                    const u8* p = reinterpret_cast<const u8*>(mats.data());
                    const size_t n = mats.size() * sizeof(mats[0]);
                    for (size_t bi = 0; bi < n; ++bi) { poseHash ^= p[bi]; poseHash *= 1099511628211ull; }
                    if (poseHash == 0) poseHash = 1;
                }
                const u64 key = (static_cast<u64>(entt::to_integral(it.e)) << 32) | (mi + 1);
                deformedVa = app.m_skinningCompute->Skin(cmd, dev, key, *mesh, it.skin->GetGpuAddress(frameIndexSk), poseHash);
                if (deformedVa == 0) { ++snap.skippedSkinned; continue; }
            }

            // 材質の解決は RT(DDGI のヒット)と共有する(PtHost::ResolveHitMaterial)
            pt::MaterialGpu m{};
            float emLuma = 0.0f;
            ResolveHitMaterial(app, it, mi, cmd, req.quantizeLikeForward, m, emLuma);
            if (m.albedoSrv != pt::kNoIndex) ++snap.textured;

            pt::InstanceDesc id;
            id.geometry = geo;
            XMStoreFloat4x4(&id.world, meshWorld);
            id.material = addMaterial(m);
            id.emissiveLuma = emLuma;
            id.deformedVbVA = deformedVa;
            if (scene->AddInstance(id) != pt::kNoIndex)
            {
                ++snap.instances;
                if (vgProxy) ++snap.vgProxies;
                if (!r.shaderPath.empty()) ++snap.customShader;
                if (splatTerrain) ++snap.terrainSplat;
            }
        }
    }
    snap.materials = static_cast<uint32_t>(scene->GetMaterialCount());
    if (haveSkinning)
    {
        app.m_skinningCompute->TransitionForAccelerationStructureBuild(cmd);
        app.m_skinningCompute->EndFrame();
    }
    {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cmd4;
        if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&cmd4)))) return fail("ID3D12GraphicsCommandList4 を取得できない");
        std::string cerr;
        if (!scene->RecordSkinnedCopies(cmd4.Get(), &cerr)) return fail(cerr);
    }

    if (snap.instances == 0) return fail("描画対象のメッシュが 1 つも無い(スキンドだけのシーンは未対応)");
    std::string berr;
    if (!scene->BeginBuild(&berr)) return fail(berr);
    snap.snapshotMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

// ---------------------------------------------------------------------------
//  フレームごと
// ---------------------------------------------------------------------------
void PtHost::Tick(Application& app, ID3D12GraphicsCommandList* cmd)
{
    if (phase == Phase::Idle || phase == Phase::Done || phase == Phase::Failed || phase == Phase::Cancelled)
    {
        if (transientPending && scene && ++framesSinceBuild >= 4) { scene->ReleaseTransient(); transientPending = false; }
        // 終わったジョブの専用シーン(BLAS / TLAS / 写し / raw SRV)を、GPU が使い終えた頃に返す。
        if ((scene || !ownedSrvs.empty()) && ++terminalFrames >= 6)
        {
            ReleaseOwnedSrvs();
            scene.reset();
            if (tracer && tracer->HasJob()) tracer->ReleaseJobBuffers();   // 累積 / 統計バッファ(1080p で約 35MB)も返す
            terminalFrames = 0;
        }
        return;
    }
    if (!cmd) return;

    // シーンが切り替わった(entity / メッシュ / SRV の対応が変わる)ら中止。
    if ((phase == Phase::Preparing || phase == Phase::Running) && sceneGeneration != app.m_sceneGeneration)
    {
        FailJob("シーンが切り替わったため中止した(リファレンスレンダー中はシーンを切り替えない)");
        return;
    }

    switch (phase)
    {
    case Phase::Requested:
    {
        if (cancelRequested) { phase = Phase::Cancelled; message = "開始前に中止された"; return; }
        // アセットのアップロード(テクスチャ / メッシュ)が残っていたら少し待つ。
        if (app.m_resourceManager && app.m_resourceManager->HasPendingUploads() && settleFrames < 30) { ++settleFrames; return; }
        if (!EnsureTracer(app)) { FailJob("パストレーサーを初期化できない: " + initError); return; }
        std::string err;
        if (!Snapshot(app, cmd, &err)) { scene.reset(); FailJob(err); return; }
        sceneGeneration = app.m_sceneGeneration;
        phase = Phase::Preparing;
        message = "シーンを構築中(BLAS / TLAS)";
        break;
    }
    case Phase::Preparing:
    {
        if (cancelRequested) { phase = Phase::Cancelled; message = "準備中に中止された"; scene.reset(); return; }
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cmd4;
        if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&cmd4)))) { FailJob("ID3D12GraphicsCommandList4 を取得できない"); return; }
        // ★BLAS のビルドは 1 フレームあたり三角形 300 万本まで(重いシーンで 1 フレームに固まらない)。
        if (!scene->BuildStep(cmd4.Get(), 3000000ull)) { message = "BLAS を構築中"; return; }
        std::string err;
        if (!scene->FinishBuild(cmd4.Get(), &err)) { FailJob(err); return; }
        sceneStats = scene->GetStats();
        transientPending = true;
        framesSinceBuild = 0;
        if (!tracer->BeginJob(job, scene->GetScene(), &err)) { FailJob(err); return; }
        prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tRequest).count();
        tRunStart = std::chrono::steady_clock::now();
        phase = Phase::Running;
        message = "レンダー中";
        Logger::Info("リファレンスレンダー開始: {}x{} {}spp {}bounces / インスタンス {} 三角形 {} 材質 {} 光 {} エミッシブ三角形 {}",
                     job.width, job.height, job.spp, job.bounces, sceneStats.instances, sceneStats.triangles,
                     sceneStats.materials, sceneStats.lights, sceneStats.emissiveTris);
        break;
    }
    case Phase::Running:
    {
        runSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - tRunStart).count();
        if (tracer->Running())
        {
            if (cancelRequested) { tracer->Cancel(); truncated = true; }
            else if (req.maxSeconds > 0.0 && runSeconds > req.maxSeconds) { tracer->Cancel(); truncated = true; }
            else
            {
                if (transientPending && ++framesSinceBuild >= 4) { scene->ReleaseTransient(); transientPending = false; }
                tracer->Record(cmd, app.m_srvHeap->GetHeap(), std::max(req.frameBudgetMs, 0.5f));
            }
        }
        if (!tracer->Running()) { phase = Phase::Finalizing; message = "結果を書き出し中"; }
        break;
    }
    case Phase::Finalizing:
        Finalize(app);
        break;
    default: break;
    }
}

void PtHost::Finalize(Application& app)
{
    (void)app;
    namespace fs = std::filesystem;
    tEnd = std::chrono::steady_clock::now();
    runSeconds = std::chrono::duration<double>(tEnd - tRunStart).count();
    const pt::PtProgress prog = tracer->GetProgress();
    gpuMsTotal = prog.gpuMsTotal;
    lastMsPerUnit = prog.msPerUnit;
    if (cancelRequested && !saveOnCancel)
    {
        phase = Phase::Cancelled;
        message = "中止された(結果は保存しない)";
        return;
    }
    std::vector<float> rgb;
    uint32_t samples = 0;
    std::string err;
    if (!tracer->ReadbackAverage(rgb, &samples, &err)) { FailJob("累積バッファの読み戻しに失敗: " + err); return; }
    tracer->ReadbackStats(stats);
    samplesDone = samples;
    if (samples < job.spp) truncated = true;

    std::error_code ec;
    fs::create_directories(fs::path(outputBase).parent_path(), ec);
    files.clear();
    if (req.writePfm) { const std::string f = outputBase + ".pfm"; if (pt::io::WritePfm(fs::path(f), job.width, job.height, rgb.data())) files.push_back(f); }
    if (req.writeExr) { const std::string f = outputBase + ".exr"; if (pt::io::WriteExr(fs::path(f), job.width, job.height, rgb.data())) files.push_back(f); }
    if (req.writePng) { const std::string f = outputBase + ".png"; if (pt::io::WritePreviewPng(fs::path(f), job.width, job.height, rgb.data(), req.exposure)) files.push_back(f); }

    // メタ JSON(再現に要る設定 + 数値)
    const double unitsPerSpp = double((job.width + job.tileSize - 1) / job.tileSize) * double((job.height + job.tileSize - 1) / job.tileSize);
    const double msPerSpp = prog.msPerUnit * unitsPerSpp / double(std::max(1u, job.samplesPerDispatch));
    nlohmann::json meta = {
        {"tool", "Uno Engine path tracer (Q1a)"},
        {"engineVersion", kEngineVersion},
        {"jobId", jobId},
        {"size", {job.width, job.height}},
        {"sppTarget", job.spp}, {"sppDone", samples}, {"truncated", truncated},
        {"bounces", job.bounces}, {"seed", job.seed}, {"maxRadiance", job.maxRadiance},
        {"russianRoulette", (job.flags & pt::kFlagRR) != 0}, {"physicalFalloff", (job.flags & pt::kFlagPhysFalloff) != 0},
        {"forceLambert", (job.flags & pt::kFlagForceLambert) != 0},
        {"quantizeLikeForward", req.quantizeLikeForward},
        {"camera", {{"pos", {job.cam.pos[0], job.cam.pos[1], job.cam.pos[2]}}, {"right", {job.cam.right[0], job.cam.right[1], job.cam.right[2]}},
                    {"up", {job.cam.up[0], job.cam.up[1], job.cam.up[2]}}, {"forward", {job.cam.fwd[0], job.cam.fwd[1], job.cam.fwd[2]}},
                    {"fovYDeg", XMConvertToDegrees(2.0f * std::atan(job.cam.tanHalfFovY))}, {"aspect", job.cam.aspect},
                    {"lensRadius", job.cam.lensRadius}, {"focusDist", job.cam.focusDist}}},
        {"sun", {{"enabled", job.sun.enabled}, {"toLight", {job.sun.toLight[0], job.sun.toLight[1], job.sun.toLight[2]}},
                 {"E", {job.sun.E[0], job.sun.E[1], job.sun.E[2]}}, {"tanAngularRadius", job.sun.tanRadius}}},
        {"environment", {{"cube", job.env.cubeSrv != pt::kNoIndex}, {"uniform", {job.env.uniform[0], job.env.uniform[1], job.env.uniform[2]}},
                         {"lightScale", job.env.lightScale}, {"backgroundScale", job.env.bgScale}}},
        {"scene", {{"drawItems", snap.drawItems}, {"instances", sceneStats.instances}, {"triangles", sceneStats.triangles},
                   {"blasTriangles", sceneStats.blasTriangles}, {"blasCount", sceneStats.blasCount}, {"materials", sceneStats.materials},
                   {"lights", sceneStats.lights}, {"emissiveTriangles", sceneStats.emissiveTris},
                   {"nonOpaqueInstances", sceneStats.nonOpaqueInstances}, {"skinnedInstances", sceneStats.skinnedInstances},
                   {"skippedSkinned", snap.skippedSkinned}, {"skippedNoMesh", snap.skippedNoMesh},
                   {"vgProxyInstances", snap.vgProxies}, {"customShaderInstances", snap.customShader},
                   {"splatTerrainInstances", snap.terrainSplat}, {"texturedInstances", snap.textured},
                   {"asBytes", sceneStats.blasBytes + sceneStats.tlasBytes}}},
        {"timing", {{"prepareMs", prepareMs}, {"renderSeconds", runSeconds}, {"gpuMsTotal", gpuMsTotal}, {"msPerSppEstimate", msPerSpp}}},
        {"stats", {{"paths", stats.paths}, {"nanSamples", stats.nanSamples}, {"clampedSamples", stats.clampedSamples}, {"rrTerminated", stats.rrTerminated}}},
        {"units", {{"linear", "scene-referred linear HDR (engine units; sun/point lights as in Forward.hlsl)"},
                   {"note", "see docs/PATH_TRACER.md (light units correspondence)"}}},
        {"userNote", req.note},
    };
    {
        std::ofstream jf(outputBase + ".json");
        if (jf) { jf << meta.dump(2); files.push_back(outputBase + ".json"); }
    }
    phase = cancelRequested ? Phase::Cancelled : Phase::Done;
    message = cancelRequested ? "中止(そこまでの結果を保存した)" : (truncated ? "時間制限で打ち切り(そこまでの結果を保存した)" : "完了");
    Logger::Info("リファレンスレンダー完了: {} spp / {:.1f} 秒 / {}", samples, runSeconds, outputBase);
}

nlohmann::json PtHost::Status(Application& app)
{
    using json = nlohmann::json;
    (void)app;
    json j;
    j["state"] = PhaseName(phase);
    j["jobId"] = jobId;
    j["busy"] = Busy();
    if (!error.empty()) j["error"] = error;
    j["message"] = message;
    const bool haveJob = tracer && tracer->HasJob() && (phase == Phase::Running || phase == Phase::Finalizing || phase == Phase::Done || phase == Phase::Cancelled);
    double pct = 0.0, eta = -1.0;
    uint32_t sdone = 0;
    double msPerUnit = 0.0;
    if (haveJob)
    {
        const pt::PtProgress p = tracer->GetProgress();
        pct = p.fraction * 100.0;
        sdone = p.samplesDone;
        msPerUnit = p.msPerUnit;
        if (phase == Phase::Running)
        {
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - tRunStart).count();
            if (p.fraction > 0.02) eta = el * (1.0 - p.fraction) / p.fraction;
            j["elapsedSec"] = el;
        }
    }
    if (phase == Phase::Done || phase == Phase::Cancelled) { pct = (phase == Phase::Done) ? 100.0 : pct; sdone = samplesDone; eta = 0.0; if (msPerUnit <= 0.0) msPerUnit = lastMsPerUnit; }
    if (phase == Phase::Requested) pct = 0.0;
    if (phase == Phase::Preparing) pct = 0.0;
    j["progress"] = {{"phase", PhaseName(phase)}, {"pct", pct}, {"message", message}, {"etaSec", eta < 0 ? json(nullptr) : json(eta)}};
    j["samples"] = {{"done", sdone}, {"target", req.spp}};
    j["size"] = {req.width, req.height};
    j["bounces"] = req.bounces;
    j["seed"] = req.seed;
    if (msPerUnit > 0.0)
    {
        const double unitsPerSpp = double((job.width + job.tileSize - 1) / job.tileSize) * double((job.height + job.tileSize - 1) / job.tileSize);
        j["gpu"] = {{"msPerUnit", msPerUnit}, {"msPerSpp", msPerUnit * unitsPerSpp / double(std::max(1u, job.samplesPerDispatch))},
                    {"note", "1 ユニット = 1 タイル x samplesPerDispatch サンプル。msPerSpp は全画面 1 サンプルの GPU 時間"}};
    }
    if (snap.instances > 0)
        j["scene"] = {{"instances", snap.instances}, {"materials", snap.materials}, {"lights", snap.lights},
                      {"skippedSkinned", snap.skippedSkinned}, {"vgProxyInstances", snap.vgProxies},
                      {"customShaderInstances", snap.customShader}, {"splatTerrainInstances", snap.terrainSplat},
                      {"triangles", sceneStats.triangles}, {"emissiveTriangles", sceneStats.emissiveTris}, {"snapshotMs", snap.snapshotMs}};
    if (phase == Phase::Done || phase == Phase::Cancelled)
    {
        j["output"] = {{"base", outputBase}, {"files", files}, {"truncated", truncated}, {"sppDone", samplesDone},
                       {"nanSamples", stats.nanSamples}, {"renderSeconds", runSeconds}};
    }
    if (!previewPath.empty()) j["preview"] = previewPath;
    return j;
}

bool PtHost::WritePreview(uint32_t* samples, std::string* err)
{
    namespace fs = std::filesystem;
    if (phase != Phase::Running || !tracer || !tracer->HasJob())
    {
        if (err) *err = "実行中(running)のジョブが無い";
        return false;
    }
    std::vector<float> rgb;
    uint32_t s = 0;
    // ★積んだリストは前フレームまでに提出済み(Tick は PrepareFrame の中。呼び出しはフレーム境界)。
    if (!tracer->ReadbackAverage(rgb, &s, err)) return false;
    const std::string path = outputBase + ".preview.png";
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (!pt::io::WritePreviewPng(fs::path(path), job.width, job.height, rgb.data(), req.exposure))
    {
        if (err) *err = "プレビュー PNG を書けない: " + path;
        return false;
    }
    previewPath = path;
    if (samples) *samples = s;
    return true;
}

// ---------------------------------------------------------------------------
//  エディタ UI との同期
// ---------------------------------------------------------------------------
void PtHost::SyncUi(Application& app, pt::UiState& ui)
{
    if (ui.requestCancel) { ui.requestCancel = false; Cancel(false); }
    if (ui.requestStart)
    {
        ui.requestStart = false;
        PtRequest r;
        r.width = static_cast<uint32_t>(std::max(ui.width, 1));
        r.height = static_cast<uint32_t>(std::max(ui.height, 1));
        r.spp = static_cast<uint32_t>(std::max(ui.spp, 1));
        r.bounces = static_cast<uint32_t>(std::clamp(ui.bounces, 1, 64));
        r.seed = static_cast<uint32_t>(ui.seed);
        r.frameBudgetMs = ui.frameBudgetMs;
        r.maxRadiance = ui.maxRadiance;
        r.exposure = ui.exposure;
        r.outputBase = ui.outputBase;
        r.writePfm = ui.writePfm; r.writeExr = ui.writeExr; r.writePng = ui.writePng;
        r.useSceneCamera = ui.useEditorCamera;
        std::memcpy(r.camPos, ui.cameraPos, 12);
        std::memcpy(r.camTarget, ui.cameraTarget, 12);
        r.fovDeg = ui.fovDeg;
        std::string err;
        if (!Request(app, r, &err))
        {
            error = err; message = err;
            if (phase != Phase::Requested && !Busy()) phase = Phase::Failed;
        }
    }
    // 進捗の書き戻し
    // pt::UiState::phase の番号: 0 待機 / 1 準備 / 2 実行 / 3 仕上げ / 4 完了 / 5 失敗 / 6 中止
    switch (phase)
    {
    case Phase::Idle:       ui.phase = 0; break;
    case Phase::Requested:
    case Phase::Preparing:  ui.phase = 1; break;
    case Phase::Running:    ui.phase = 2; break;
    case Phase::Finalizing: ui.phase = 3; break;
    case Phase::Done:       ui.phase = 4; break;
    case Phase::Failed:     ui.phase = 5; break;
    case Phase::Cancelled:  ui.phase = 6; break;
    }
    const nlohmann::json st = Status(app);
    ui.progress = static_cast<float>(st["progress"].value("pct", 0.0) / 100.0);
    ui.samplesDone = static_cast<int>(st["samples"].value("done", 0));
    ui.samplesTarget = static_cast<int>(st["samples"].value("target", 0));
    ui.elapsedSec = static_cast<float>(st.value("elapsedSec", 0.0));
    ui.etaSec = (st["progress"].contains("etaSec") && st["progress"]["etaSec"].is_number()) ? static_cast<float>(st["progress"]["etaSec"].get<double>()) : -1.0f;
    ui.msPerSpp = st.contains("gpu") ? static_cast<float>(st["gpu"].value("msPerSpp", 0.0)) : 0.0f;
    std::snprintf(ui.message, sizeof(ui.message), "%s", message.c_str());
    if (!outputBase.empty()) std::snprintf(ui.lastOutput, sizeof(ui.lastOutput), "%s", outputBase.c_str());
}

// ---------------------------------------------------------------------------
//  Application 側の入口(Application.h に宣言)
// ---------------------------------------------------------------------------
PtHost& Application::EnsurePathTracerHost()
{
    if (!m_ptHost) m_ptHost = std::make_shared<PtHost>();
    return *m_ptHost;
}

void Application::PathTracerTick(ID3D12GraphicsCommandList* cmd)
{
    // UI の要求だけは PtHost が無くても見る(開始ボタンが押されたら生成する)。
    if (m_editorCtx && (m_editorCtx->ptUi.requestStart || m_editorCtx->ptUi.requestCancel))
        EnsurePathTracerHost();
    if (!m_ptHost) return;
    if (m_editorCtx) m_ptHost->SyncUi(*this, m_editorCtx->ptUi);
    m_ptHost->Tick(*this, cmd);
}

void Application::PathTracerShutdown()
{
    if (m_ptHost) { m_ptHost->Shutdown(); m_ptHost.reset(); }
}

} // namespace dx12e
