// ===========================================================================
// MCP: 知覚層（dx12_perceive）— プレイヤーの目から見た事実を数値で返す
// ---------------------------------------------------------------------------
// ★なぜ要るか
//   判断モデル（Jev）は画像を見られない。「破片が真っ黒な板に見える」「深さが見えない」
//   「仕掛けが画面を埋める」は机上検査（配置・到達性）には一切出ず、焦点に立って撮って初めて
//   分かった（JUNCTION 2026-09-08）。ここはそれを数値で出す。言葉に直すのは TS 側
//   （tools/mcp-server/perceive.ts）、指標の定義は src/renderer/PerceptionStats.h。
//
// ★流れ（状態は core/mcp/McpPerceive.h の McpPerceiveJob）:
//   受付（ここ）→ 指定カメラへ切り替えて決定論モードで N フレーム落ち着かせる →
//   Render() が最終画のコピーと ID パスを同じフレームに記録（ApplicationRender.cpp）→
//   FinishPerception（ここ）が読み戻して perception::Analyze → 遅延応答 → カメラを戻す。
// ★要求が無いフレームでは何も走らない（最終画は 1 ビットも変わらない。tools/bench/golden.mjs）。
// ===========================================================================
#include "core/ApplicationInternal.h"

#include <cmath>

namespace dx12e
{
using namespace appdetail;

namespace
{
using json = nlohmann::json;

double R4(double v) { return std::round(v * 10000.0) / 10000.0; }
json   OptF(const std::optional<float>& v) { return v ? json(R4(*v)) : json(nullptr); }

json StatsJson(const perception::Stats& s)
{
    json j{
        {"name", s.name},
        {"pixels", s.pixels},
        {"share", R4(s.share)},
        {"luma", R4(s.luma)},
        {"lumaStd", R4(s.lumaStd)},
        {"lumaRing", OptF(s.lumaRing)},
        {"contrast", OptF(s.contrast)},
        {"saturation", R4(s.saturation)},
        {"distance", OptF(s.distance)},
        {"distanceMin", OptF(s.distanceMin)},
        {"fullyInView", s.fullyInView ? json(*s.fullyInView) : json(nullptr)},
        {"occlusion", OptF(s.occlusion)},
        {"litFacing", OptF(s.litFacing)},
        {"backFacing", OptF(s.backFacing)},
    };
    if (!s.parent.empty()) j["parent"] = s.parent;
    if (s.pixels > 0)
    {
        j["bbox"]   = { R4(s.bbox[0]), R4(s.bbox[1]), R4(s.bbox[2]), R4(s.bbox[3]) };
        j["center"] = { R4(s.center[0]), R4(s.center[1]) };
    }
    else
    {
        j["bbox"] = nullptr;
        j["center"] = nullptr;
    }
    j["projectedExtent"] = s.projectedExtent
        ? json{ R4((*s.projectedExtent)[0]), R4((*s.projectedExtent)[1]) } : json(nullptr);
    if (s.isolatedPixels) j["isolatedPixels"] = *s.isolatedPixels;
    if (s.unlit) j["unlit"] = true;
    j["mainLight"] = s.mainLight.empty() ? json(nullptr)
        : json{{"name", s.mainLight}, {"facing", OptF(s.mainLightFacing)}};
    return j;
}

json RegionJson(const perception::RegionStats& r)
{
    return json{{"empty", R4(r.empty)}, {"luma", R4(r.luma)}, {"lumaStd", R4(r.lumaStd)},
                {"distance", OptF(r.distance)}};
}

std::string NameOf(const entt::registry& reg, entt::entity e)
{
    if (e == entt::null || !reg.valid(e)) return {};
    if (const auto* nt = reg.try_get<NameTag>(e)) return nt->name;
    return "#" + std::to_string(static_cast<u32>(e));
}

// スクショ系と同じ出力先の解決（.. は弾く・相対は CWD 基準・拡張子 .png を補う）
std::filesystem::path PerceivePngPath(const std::string& rel)
{
    namespace fs = std::filesystem;
    fs::path p(rel);
    for (const auto& part : p)
        if (part == "..")
            throw McpError(McpErr::InvalidParam, "path must not contain '..'",
                           "CWD からの相対パスか絶対パスで指定する");
    if (p.extension() != ".png") p += ".png";
    p = fs::absolute(p);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    return p;
}
} // namespace

void Application::RegisterMcpPerceiveMethods()
{
    // 入力:
    //   camera   … 省略 or "editor" = 今のシーンビューのカメラ / "game" = アクティブなゲームカメラ /
    //               { position:[x,y,z], target:[x,y,z], fovDeg? } = その視点（終わったら元のカメラへ戻す）
    //   targets  … 見たい物の名前（1 件か配列、最大 16）。子孫も含めて 1 つの対象として数える
    //   top      … 画面占有の上位何件を返すか（既定 8 / 0〜64）
    //   width/height … 解析解像度（既定 = 表示矩形。片方だけならアスペクトを保つ）
    //   settleFrames … 決定論モードで落ち着かせるフレーム数（既定 8）
    //   path     … 最終画を PNG で保存する先（省略 = 保存しない）
    //   includeTransparent … 半透明を「手前の面」として数えるか（既定 true。false で深度プリパスと同じく除外）
    McpDefine("perceive", "camera:any,camera.fovDeg:number,camera.position:vec3,camera.target:vec3,"
              "height:int,includeTransparent:bool,path:string,settleFrames:int,targets:any,top:int,width:int",
              DX12E_MCP_HANDLER
        {
            if (m_mcpPerceive)
                throw McpError(McpErr::ModeConflict, "a perceive request is already in flight",
                               "前の dx12_perceive の応答を待ってから撃ち直す（1 本ずつしか受け付けない）");
            if (m_mcpFinalShot.reply.client != 0 || m_mcpFinalShot.pending || m_deterministicCapture
                || m_mcpGameViewReply.client != 0 || m_mcpRenderDebugFramesLeft > 0)
                throw McpError(McpErr::ModeConflict, "a screenshot / render_debug is in flight",
                               "撮影系（screenshot / screenshot_final / render_debug）の応答を待ってから撃ち直す");
            if (m_showLauncher || m_loading || !m_scene || !m_camera)
                throw McpError(McpErr::ModeConflict, "no scene is open",
                               "dx12_open_project / dx12_open_scene でシーンを開いてから撃つ");

            auto job = std::make_unique<McpPerceiveJob>();
            auto& reg = m_scene->GetRegistry();

            // ---- カメラ ----
            if (params.contains("camera") && !params["camera"].is_null())
            {
                const json& c = params["camera"];
                if (c.is_string())
                {
                    const std::string m = c.get<std::string>();
                    if (m == "editor") job->camMode = McpPerceiveJob::Cam::Current;
                    else if (m == "game")
                    {
                        bool hasActive = false;
                        for (auto [e, cc] : reg.view<const CameraComponent>().each())
                            if (cc.isActive) { hasActive = true; break; }
                        if (!hasActive)
                            throw McpError(McpErr::NotFound, "no active CameraComponent",
                                           "camera.isActive=true のカメラを置くか、camera に {position,target} を渡す");
                        job->camMode = McpPerceiveJob::Cam::Game;
                    }
                    else
                        throw McpError(McpErr::InvalidParam, "unknown camera mode: " + m,
                                       "\"editor\" / \"game\" / {position,target,fovDeg?} のどれかを渡す",
                                       {"editor", "game"});
                }
                else if (c.is_object())
                {
                    auto vec3 = [&](const char* key) {
                        if (!c.contains(key) || !c[key].is_array() || c[key].size() != 3)
                            throw McpError(McpErr::InvalidParam, std::string("camera.") + key + " must be [x,y,z]",
                                           "camera は {\"position\":[x,y,z],\"target\":[x,y,z]} の形で渡す");
                        const auto v = c[key].get<std::vector<float>>();
                        return DirectX::XMFLOAT3{ v[0], v[1], v[2] };
                    };
                    job->camMode   = McpPerceiveJob::Cam::Explicit;
                    job->camPos    = vec3("position");
                    job->camTarget = vec3("target");
                    const float dx = job->camTarget.x - job->camPos.x, dy = job->camTarget.y - job->camPos.y,
                                dz = job->camTarget.z - job->camPos.z;
                    if (dx * dx + dy * dy + dz * dz < 1e-8f)
                        throw McpError(McpErr::InvalidParam, "camera.position and camera.target are the same point",
                                       "target は position から離れた「見る先」の点を渡す");
                    if (c.contains("fovDeg"))
                    {
                        const float f = c["fovDeg"].get<float>();
                        if (!(f >= 1.0f && f <= 170.0f))
                            throw McpError(McpErr::InvalidParam, "camera.fovDeg out of range",
                                           "縦の視野角を 1〜170 度で渡す（省略すると今のカメラのまま）");
                        job->fovDeg = f;
                    }
                }
                else
                    throw McpError(McpErr::InvalidParam, "camera must be a string or an object",
                                   "\"editor\" / \"game\" / {position,target,fovDeg?} のどれかを渡す",
                                   {"editor", "game"});
            }

            // ---- 対象 ----
            if (params.contains("targets") && !params["targets"].is_null())
            {
                const json& t = params["targets"];
                std::vector<std::string> names;
                if (t.is_string()) names.push_back(t.get<std::string>());
                else if (t.is_array()) for (const auto& x : t) names.push_back(x.get<std::string>());
                else throw McpError(McpErr::InvalidParam, "targets must be a name or an array of names",
                                    "\"targets\": [\"C6_p0\", \"C6_p1\"] のように名前で渡す");
                if (names.size() > 16)
                    throw McpError(McpErr::InvalidParam, "too many targets (max 16)",
                                   "対象は 16 件まで。親エンティティの名前を渡せば子孫をまとめて 1 件として数える");
                for (const auto& nm : names)
                {
                    auto ent = m_scene->FindEntity(nm);
                    if (!ent.IsValid())
                        throw McpError(McpErr::NotFound, "no entity named '" + nm + "'",
                                       "dx12_find_entity / dx12_list_entities で正しい名前を確かめる");
                    job->targetNames.push_back(nm);
                    job->targetEntities.push_back(ent.GetHandle());
                }
            }

            // ---- 解像度・件数 ----
            const int top = params.value("top", 8);
            if (top < 0 || top > 64)
                throw McpError(McpErr::InvalidParam, "top out of range", "top は 0〜64（既定 8）");
            job->top = static_cast<u32>(top);
            const int wReq = params.value("width", 0), hReq = params.value("height", 0);
            if (wReq < 0 || hReq < 0 || wReq > 8192 || hReq > 8192 || (wReq > 0 && wReq < 16) || (hReq > 0 && hReq < 16))
                throw McpError(McpErr::InvalidParam, "width/height out of range",
                               "16〜8192 で渡す（省略すると表示矩形と同じ解像度）");
            if (wReq > 0 && hReq > 0)
            {
                u32 vx = 0, vy = 0, vw = 1, vh = 1;
                GetDisplayViewport(vx, vy, vw, vh);
                const double a = static_cast<double>(wReq) / hReq, b = static_cast<double>(vw) / vh;
                if (std::fabs(a / b - 1.0) > 0.02)
                    throw McpError(McpErr::InvalidParam, "width/height aspect differs from the view",
                                   "縦横比は表示矩形（" + std::to_string(vw) + "x" + std::to_string(vh) +
                                   "）に合わせるか、width だけを渡す");
            }
            job->width  = static_cast<u32>(wReq);
            job->height = static_cast<u32>(hReq);
            job->settleFrames = std::clamp(params.value("settleFrames", 8), 1, 240);
            job->path = params.value("path", std::string());
            job->includeTransparent = params.value("includeTransparent", true);
            if (!job->path.empty()) (void)PerceivePngPath(job->path);   // 先に検証だけ（.. を弾く）

            // ---- GPU 側（最初の要求で作る。要求が無いセッションでは何も作らない）----
            if (!m_perceptionPass)
            {
                try
                {
                    auto pass = std::make_unique<PerceptionPass>();
                    pass->Initialize(*m_graphicsDevice, PathResolver::ShaderDirW());
                    m_perceptionPass = std::move(pass);
                }
                catch (const std::exception& e)
                {
                    throw McpError(McpErr::Internal, std::string("perception pass init failed: ") + e.what(),
                                   "exe の隣の shaders/ に PerceptionId_VS.cso / PerceptionIdSkinned_VS.cso / "
                                   "PerceptionId_PS.cso があるか確かめる（無ければエンジンをビルドし直す）");
                }
            }

            // ---- カメラを退避してから切り替える ----
            if (job->camMode != McpPerceiveJob::Cam::Current)
            {
                job->saved       = true;
                job->savedPos    = m_camera->GetPosition();
                job->savedYaw    = m_camera->GetYaw();
                job->savedPitch  = m_camera->GetPitch();
                job->savedFov    = m_camera->GetFovY();
                job->savedAspect = m_camera->GetAspect();
                job->savedNear   = m_camera->GetNearZ();
                job->savedFar    = m_camera->GetFarZ();
                job->savedOrtho  = m_camera->IsOrthographic();
                job->savedOrthoH = m_camera->GetOrthoHeight();
            }
            job->reply = deferred;
            job->t0    = std::chrono::steady_clock::now();
            job->phase = McpPerceiveJob::Phase::Settling;
            m_mcpPerceive = std::move(job);
            ApplyPerceptionCamera();

            // ★決定論キャプチャと同じ仕組み（time / TAA ジッタ / フォグ・SSGI の位相を固定し、
            //   履歴を捨ててから固定フレーム数回す）。カメラを動かした直後の TAA の尾引きも消える。
            InvalidateTemporalHistory();
            m_deterministicFramesLeft = m_mcpPerceive->settleFrames;
            m_deterministicCapture    = true;
            isDeferred = true;
        });
}

void Application::ApplyPerceptionCamera()
{
    if (!m_mcpPerceive || !m_camera) return;
    const McpPerceiveJob& j = *m_mcpPerceive;
    switch (j.camMode)
    {
    case McpPerceiveJob::Cam::Explicit:
        m_camera->SetPosition(j.camPos);
        m_camera->LookAt(j.camPos, j.camTarget);
        if (j.fovDeg > 0.0f && !m_camera->IsOrthographic())
            m_camera->SetPerspective(DirectX::XMConvertToRadians(j.fovDeg), m_camera->GetAspect(),
                                     m_camera->GetNearZ(), m_camera->GetFarZ());
        break;
    case McpPerceiveJob::Cam::Game:
        SyncActiveCameraToGlobal();   // Editor でも Play 中の MCP カメラ上書き中でも、ゲームカメラの視点へ
        break;
    default:
        break;
    }
}

void Application::ApplyPerceptionProjection(f32 aspect)
{
    if (!m_mcpPerceive || !m_camera || !m_scene) return;
    const McpPerceiveJob& j = *m_mcpPerceive;
    if (j.camMode == McpPerceiveJob::Cam::Explicit && j.fovDeg > 0.0f && !m_camera->IsOrthographic())
    {
        m_camera->SetPerspective(DirectX::XMConvertToRadians(j.fovDeg), aspect,
                                 m_camera->GetNearZ(), m_camera->GetFarZ());
    }
    else if (j.camMode == McpPerceiveJob::Cam::Game)
    {
        // ゲームカメラの投影（Play 中の Render と同じ式。アスペクトは実ビューポート）
        for (auto [e, cam] : m_scene->GetRegistry().view<const CameraComponent>().each())
        {
            if (!cam.isActive) continue;
            if (cam.projection == CameraProjection::Orthographic)
                m_camera->SetOrthographic(2.0f * cam.orthoSize, aspect, cam.nearClip, cam.farClip);
            else
                m_camera->SetPerspective(DirectX::XMConvertToRadians(cam.fovDegrees), aspect,
                                         cam.nearClip, cam.farClip);
            break;
        }
    }
}

void Application::EndPerception()
{
    if (!m_mcpPerceive) return;
    McpPerceiveJob& j = *m_mcpPerceive;
    if (j.saved && m_camera)
    {
        m_camera->SetPosition(j.savedPos);
        m_camera->SetYaw(j.savedYaw);
        m_camera->SetPitch(j.savedPitch);
        if (j.savedOrtho) m_camera->SetOrthographic(j.savedOrthoH, j.savedAspect, j.savedNear, j.savedFar);
        else              m_camera->SetPerspective(j.savedFov, j.savedAspect, j.savedNear, j.savedFar);
    }
    // ★決定論モードは必ず戻す（戻し忘れると時間が止まったまま＝エディタが固まって見える）
    m_deterministicCapture    = false;
    m_deterministicFramesLeft = 0;
    // 応答を返しそこねたまま捨てない（クライアントが timeout まで待たされる）
    if (j.reply.client != 0)
        FailMcp(m_mcpBridge.get(), j.reply, McpErr::Internal, "perceive was aborted");
    m_mcpPerceive.reset();
}

void Application::FinishPerception()
{
    if (!m_mcpPerceive) return;
    McpPerceiveJob& j = *m_mcpPerceive;
    ++j.frames;
    auto fail = [&](const std::string& why)
    {
        FailMcp(m_mcpBridge.get(), j.reply, McpErr::Internal, "perceive: " + why);
        j.reply = {};
        EndPerception();
    };
    if (j.phase == McpPerceiveJob::Phase::Pending)
    {
        // Render がシーンを描かずに素通りした（最小化・ロード中など）。いつまでも待たせない
        if (++j.waitFrames > 60) fail("the frame was not rendered for 60 frames (window minimized or loading?)");
        return;
    }
    if (j.phase != McpPerceiveJob::Phase::Captured) return;
    using clock = std::chrono::high_resolution_clock;
    using namespace DirectX;

    // ---- 読み戻し（コピーは Present と同じコマンドリスト。GPU の完了を待つ）----
    const auto tRb = clock::now();
    m_commandQueue->WaitIdle();
    PerceptionPass::CpuFrame f;
    std::string err;
    if (!m_perceptionPass->Readback(f, j.isolationUsed, err)) { fail(err); return; }
    const double readbackMs = std::chrono::duration<double, std::milli>(clock::now() - tRb).count();

    // ---- メタ情報（ID = 描画アイテムの添字 + 1）と光源のスナップショット ----
    // ★m_drawItems は次の Render まで撮影フレームのまま（この関数は Render の直後に呼ばれる）
    const auto tAn = clock::now();
    auto& reg = m_scene->GetRegistry();
    std::vector<perception::EntityMeta> meta(m_drawItems.size() + 1);
    for (size_t i = 0; i < m_drawItems.size(); ++i)
    {
        const DrawItem& it = m_drawItems[i];
        perception::EntityMeta& m = meta[i + 1];
        m.name = NameOf(reg, it.e);
        if (const auto* tf = reg.try_get<Transform>(it.e)) m.parent = NameOf(reg, tf->parent);
        m.hasAabb = true;
        m.transparent = j.includeTransparent && (it.alphaClass == 2u || it.sortKey == 3u);
        m.aabbMin = { it.aabbMin.x, it.aabbMin.y, it.aabbMin.z };
        m.aabbMax = { it.aabbMax.x, it.aabbMax.y, it.aabbMax.z };
    }
    auto lumOf = [](const XMFLOAT3& c, float k) { return (0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z) * k; };
    std::vector<perception::Light> lights;
    {
        // 平行光はフォワードと同じく「最初の 1 灯」だけ
        auto dlView = reg.view<const DirectionalLight>();
        if (!dlView.empty())
        {
            const entt::entity e = *dlView.begin();
            const auto& dl = dlView.get<const DirectionalLight>(e);
            perception::Light L;
            L.type = perception::Light::Directional;
            L.name = NameOf(reg, e);
            XMFLOAT3 d;
            XMStoreFloat3(&d, XMVector3Normalize(XMLoadFloat3(&dl.direction)));
            L.direction = { d.x, d.y, d.z };
            L.radiance  = lumOf(dl.color, dl.intensity);
            lights.push_back(L);
        }
        for (auto [e, pl, tf] : reg.view<const PointLight, const Transform>().each())
        {
            perception::Light L;
            L.type = perception::Light::Point;
            L.name = NameOf(reg, e);
            const XMMATRIX wm = (tf.parent != entt::null) ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMFLOAT3 p;
            XMStoreFloat3(&p, wm.r[3]);
            L.position = { p.x, p.y, p.z };
            L.range    = pl.range;
            L.radiance = lumOf(pl.color, pl.intensity);
            lights.push_back(L);
        }
        for (auto [e, sl, tf] : reg.view<const SpotLight, const Transform>().each())
        {
            perception::Light L;
            L.type = perception::Light::Spot;
            L.name = NameOf(reg, e);
            const XMMATRIX wm = (tf.parent != entt::null) ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMFLOAT3 p, d;
            XMStoreFloat3(&p, wm.r[3]);
            XMStoreFloat3(&d, XMVector3Normalize(XMLoadFloat3(&sl.direction)));
            L.position  = { p.x, p.y, p.z };
            L.direction = { d.x, d.y, d.z };
            L.range     = sl.range;
            L.radiance  = lumOf(sl.color, sl.intensity);
            const float outerDeg = (std::max)(sl.outerConeDeg, sl.innerConeDeg);
            L.cosInner = std::cos(XMConvertToRadians(sl.innerConeDeg));
            L.cosOuter = std::cos(XMConvertToRadians(outerDeg));
            lights.push_back(L);
        }
    }

    // ---- 集計（純関数。定義は renderer/PerceptionStats.h）----
    for (u32 t = 0; t < static_cast<u32>(j.groups.size()) && t < static_cast<u32>(f.isolation.size()); ++t)
        j.groups[t].isolatedMask = f.isolation[t].data();
    perception::PixelFrame pf;
    pf.width   = f.w;
    pf.height  = f.h;
    pf.ids     = f.ids.data();
    pf.rgba    = f.rgba.data();
    pf.posDist = f.posDist.data();
    pf.normal  = f.normal.data();
    perception::Options opt;
    opt.top = j.top;
    const perception::Result res = perception::Analyze(pf, meta, j.groups, lights, j.camera, opt);
    const double analyzeMs = std::chrono::duration<double, std::milli>(clock::now() - tAn).count();

    // ---- 最終画の保存（任意）----
    json shot = nullptr;
    if (!j.path.empty())
    {
        try
        {
            const auto outPath = PerceivePngPath(j.path);
            std::vector<u8> bgra(f.colorRgba.size());
            for (size_t p = 0; p + 3 < bgra.size(); p += 4)
            {
                bgra[p + 0] = f.colorRgba[p + 2];
                bgra[p + 1] = f.colorRgba[p + 1];
                bgra[p + 2] = f.colorRgba[p + 0];
                bgra[p + 3] = 255;
            }
            std::string perr;
            if (WriteBgraPng(outPath.wstring(), bgra.data(), f.colorW, f.colorH, perr)) shot = outPath.string();
            else shot = json{{"error", perr}};
        }
        catch (const std::exception& e) { shot = json{{"error", e.what()}}; }
    }

    // ---- 応答 ----
    json targets = json::array();
    for (size_t t = 0; t < res.targets.size(); ++t)
    {
        json one = StatsJson(res.targets[t]);
        one["members"]  = res.targets[t].members;
        one["transparentMembers"] = res.targets[t].transparentMembers;
        if (t < j.targetEntities.size()) one["entityId"] = static_cast<u32>(j.targetEntities[t]);
        if (res.targets[t].members == 0) one["note"] = "描画物（MeshRenderer）を持たない。子孫にも無い";
        targets.push_back(std::move(one));
    }
    json topArr = json::array();
    for (const auto& s : res.top)
    {
        json one = StatsJson(s);
        one["transparent"] = s.transparentMembers > 0;
        if (s.id >= 1 && s.id <= m_drawItems.size())
            one["entityId"] = static_cast<u32>(m_drawItems[s.id - 1].e);
        topArr.push_back(std::move(one));
    }
    const auto& sc = res.scene;
    const char* camSource = j.camMode == McpPerceiveJob::Cam::Explicit ? "explicit"
                          : j.camMode == McpPerceiveJob::Cam::Game     ? "game" : "editor";
    const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - j.t0).count();
    json result{
        {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
        {"camera", {{"source", camSource},
                    {"position", {R4(j.camera.position.x), R4(j.camera.position.y), R4(j.camera.position.z)}},
                    {"forward", {R4(j.camForward.x), R4(j.camForward.y), R4(j.camForward.z)}},
                    {"fovDeg", R4(j.camFovDeg)}}},
        {"resolution", {{"width", f.w}, {"height", f.h}, {"colorWidth", f.colorW}, {"colorHeight", f.colorH}}},
        {"scene", {
            {"empty", R4(sc.empty)},
            {"regions", {{"top", RegionJson(sc.top)}, {"bottom", RegionJson(sc.bottom)},
                         {"left", RegionJson(sc.left)}, {"right", RegionJson(sc.right)}}},
            {"luma", {{"mean", R4(sc.lumaMean)}, {"p5", R4(sc.lumaP5)}, {"p50", R4(sc.lumaP50)},
                      {"p95", R4(sc.lumaP95)}, {"crushed", R4(sc.crushed)}, {"clipped", R4(sc.clipped)}}},
            {"farthest", OptF(sc.farthest)},
            {"visibleEntities", sc.visibleEntities}}},
        {"targets", std::move(targets)},
        {"top", std::move(topArr)},
        {"lights", lights.size()},
        {"transparent", {{"count", j.transparentCount}, {"included", j.includeTransparent},
                         {"note", j.includeTransparent
                              ? "半透明は ID パスに『手前の面』として描いた（奥の物は隠れた扱い。luma は混ざった色）"
                              : "半透明は ID パスから除いた（奥の物が見えている扱い）"}}},
        {"notInIdPass", "スプライト・パーティクル・ゲーム内 UI・空は ID パスに入らない（その画素は奥の物か"
                        "「空」として数える）。最終画の輝度には写っている"},
        {"cost", {{"recordMs", R4(j.recordMs)}, {"readbackMs", R4(readbackMs)}, {"analyzeMs", R4(analyzeMs)},
                  {"totalMs", R4(totalMs)}, {"frames", j.frames}, {"settleFrames", j.settleFrames},
                  {"drawCalls", j.drawCalls}}},
        {"screenshot", shot},
        {"definitions", "指標の厳密な定義は src/renderer/PerceptionStats.h の先頭。"
                        "luma は表示色（ガンマ済み）の Rec.709 Y'、contrast=(luma+0.05)/(lumaRing+0.05)、"
                        "litFacing は影を無視した直接光のうちシェーディング法線の側に届く割合"},
    };
    CompleteMcp(m_mcpBridge.get(), j.reply, std::move(result));
    j.reply = {};
    EndPerception();
}

} // namespace dx12e
