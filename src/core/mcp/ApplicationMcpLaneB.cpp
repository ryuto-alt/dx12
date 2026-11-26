// ===========================================================================
// レーン B の MCP 登録口（W05-01b。EnsureMcpMethodTable() が呼ぶ）。
// 中身は各レーンが書く。Application.h は変えない（W7 まで凍結）。
// Application の private に触れる処理は core/AppAccess.h のレーン B のブロックへ宣言する。
//
// W1-BD4（UnoFX）: vfx_place / vfx_seek / vfx_stats と、アセットブラウザの .uvfx のダブルクリック（pendingVfxPlace）の消化。
// ===========================================================================
#include <fstream>
#include <iterator>

#include "core/ApplicationInternal.h"
#include "core/AppAccess.h"
#include "core/QualityPresetApply.h"
#include "core/vfs/Vfs.h"
#include "ecs/components/VfxComponent.h"
#include "editor/UndoSystem.h"
#include "renderer/QualityPreset.h"
#include "renderer/vfx/VfxAsset.h"
#include "renderer/vfx/VfxRuntime.h"

namespace dx12e
{
namespace
{
// プロジェクトの assets に無ければ、開発時はエンジン同梱の assets（ASSETS_DIR）から読む（LightCookieSystem と同じ方針）。
std::vector<u8> ReadVfxAssetBytes(const std::string& rel)
{
    std::vector<u8> b = vfs::ReadAsset(rel);
    if (b.empty() && !vfs::InGameMode() && rel.find("..") == std::string::npos)
    {
        std::ifstream in(std::string(ASSETS_DIR) + rel, std::ios::binary);
        if (in.is_open()) b.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return b;
}
bool VfxAssetExists(const std::string& rel) { return !ReadVfxAssetBytes(rel).empty(); }

// preset 名 → assets 相対パス。パス（'/' か ".uvfx" を含む）はそのまま。
std::string ResolveVfxAsset(const std::string& presetOrAsset)
{
    if (presetOrAsset.empty()) return {};
    if (presetOrAsset.find('/') != std::string::npos || presetOrAsset.find(".uvfx") != std::string::npos)
        return presetOrAsset;
    for (const char* dir : { "engine/vfx2/presets/", "engine/vfx_core/", "vfx/" })
    {
        const std::string p = std::string(dir) + presetOrAsset + ".uvfx";
        if (VfxAssetExists(p)) return p;
    }
    return {};
}

nlohmann::json VfxStatsJson()
{
    const vfx::Stats& st = vfx::Runtime::Get().GetStats();
    nlohmann::json j = nlohmann::json::object();
    j["gpuReady"] = st.gpuReady;
    j["poolUsed"] = st.poolUsed;
    j["poolCapacity"] = st.poolCapacity;
    j["instances"] = st.instances;
    j["vramBytes"] = st.vramBytes;
    if (!st.lastError.empty()) j["lastError"] = st.lastError;
    j["list"] = nlohmann::json::array();
    for (const vfx::InstanceStat& s : st.list)
        j["list"].push_back({ {"entity", s.entity}, {"emitter", s.emitter}, {"cap", s.cap}, {"spawned", s.spawned},
                              {"alive", s.alive}, {"time", s.time}, {"paused", s.paused}, {"finished", s.finished} });
    return j;
}
} // namespace

void AppAccess::VfxBindHost(Application& app)
{
    vfx::Host h;
    h.user = &app;
    h.registry = [](void* u) -> entt::registry*
    {
        Application* a = static_cast<Application*>(u);
        return a->m_scene ? &a->m_scene->GetRegistry() : nullptr;
    };
    h.frameDt = [](void* u) -> float
    {
        Application* a = static_cast<Application*>(u);
        if (a->m_deterministicCapture) return 0.0f;                      // 決定論キャプチャ中は進めない（#31）
        if (a->m_editorCtx && a->m_editorCtx->paused) return 0.0f;       // F1 の一時停止
        return a->m_gameClock.GetDeltaTime();
    };
    h.quality = [](void* u, float& mul, u32& maxParticles)
    {
        Application* a = static_cast<Application*>(u);
        const QualityPreset& p = GetPreset(GetQualityPreset(*a));
        mul = p.emissionMultiplier;
        maxParticles = p.hasBudget ? p.maxParticles : 0;   // 0 = 上限なし
    };
    h.readAsset = [](void*, const std::string& rel) { return ReadVfxAssetBytes(rel); };
    h.pollPending = [](void* u)
    {
        Application* a = static_cast<Application*>(u);
        if (!a->m_editorCtx || a->m_editorCtx->pendingVfxPlace.empty() || !a->m_scene || !a->m_camera) return;
        const std::string rel = a->m_editorCtx->pendingVfxPlace;
        a->m_editorCtx->pendingVfxPlace.clear();
        // ビューポートの注視点: カメラの前方。下向きなら床（y=0）との交点（50m まで）、そうでなければ前方 8m
        const DirectX::XMFLOAT3 cp = a->m_camera->GetPosition(), cf = a->m_camera->GetForward();
        float dist = 8.0f;
        if (cf.y < -0.05f) dist = std::min(50.0f, -cp.y / cf.y);
        const DirectX::XMFLOAT3 pos{cp.x + cf.x * dist, cp.y + cf.y * dist, cp.z + cf.z * dist};
        entt::entity e = entt::null; std::string asset, err;
        if (!VfxPlace(*a, rel, pos, entt::null, std::string(), e, asset, err))
        {
            Logger::Warn("UnoFX: .uvfx を置けなかった: {}", err);
            return;
        }
        if (a->m_editorCtx)
        {
            a->m_editorCtx->Select(e);
            a->m_editorCtx->revealInHierarchy = true;
        }
    };
    vfx::Runtime::Get().SetHost(h);
}

bool AppAccess::VfxPlace(Application& app, const std::string& presetOrAsset, const DirectX::XMFLOAT3& worldPos, entt::entity parent,
                         const std::string& nameIn, entt::entity& outEntity, std::string& outAsset, std::string& err)
{
    if (!app.m_scene) { err = "シーンが無い"; return false; }
    const std::string asset = ResolveVfxAsset(presetOrAsset);
    if (asset.empty())
    {
        err = "preset / asset が見つからない: '" + presetOrAsset + "'（preset 名か、assets 相対の .uvfx のパス）";
        return false;
    }
    const std::vector<u8> bytes = ReadVfxAssetBytes(asset);
    if (bytes.empty()) { err = "ファイルを読めない: " + asset; return false; }
    const vfx::ParseResult pr = vfx::ParseUvfx(std::string(bytes.begin(), bytes.end()));
    if (!pr.ok) { err = asset + ": " + pr.error; return false; }

    auto& reg = app.m_scene->GetRegistry();
    if (parent != entt::null && !reg.valid(parent)) { err = "parent のエンティティが無い"; return false; }

    std::string stem = nameIn;
    if (stem.empty())
    {
        stem = asset.substr(asset.find_last_of('/') == std::string::npos ? 0 : asset.find_last_of('/') + 1);
        if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".uvfx") == 0) stem.resize(stem.size() - 5);
    }
    auto exists = [&](const std::string& q)
    {
        for (auto [e, tag] : reg.view<const NameTag>().each()) if (tag.name == q) return true;
        return false;
    };
    std::string name = stem;
    for (int i = 1; exists(name) && i < 1000; ++i) name = stem + " (" + std::to_string(i) + ")";

    const entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{name});
    Transform t{};
    t.position = worldPos;
    if (parent != entt::null)
    {
        // ワールド位置を保つ: 親のワールド行列の逆で局所へ
        const DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(nullptr, ComputeWorldMatrix(reg, parent));
        DirectX::XMStoreFloat3(&t.position, DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&worldPos), inv));
        t.parent = parent;
    }
    reg.emplace<Transform>(e, t);
    VfxComponent vc;
    vc.asset = asset;
    reg.emplace<VfxComponent>(e, std::move(vc));

    // Undo: 生成 1 件（MCP の呼び出し中は「AI: vfx_place」の 1 エントリへ取り込まれる）
    if (app.m_editorCtx)
        app.m_editorCtx->undoSystem.PushCommand(std::make_unique<SpawnEntityCommand>(app.m_scene.get(), PathResolver::AssetsDir(), e));
    Logger::Info("UnoFX: 置いた: {} ({})", name, asset);
    outEntity = e;
    outAsset = asset;
    return true;
}

void Application::RegisterMcpLaneBMethods()
{
    using namespace appdetail;
    AppAccess::VfxBindHost(*this);

    McpDefine("vfx_place", "asset:string,name:string,parent:any,parentName:any,position:any,preset:string", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot place VFX while Playing; call dx12_stop first",
                    "dx12_stop で Editor へ戻してから置く（Play 中に置いても Stop で消える）");
            const std::string target = params.value("preset", params.value("asset", std::string()));
            if (target.empty())
                throw McpError(McpErr::InvalidParam, "preset か asset が要る",
                    "例: preset:\"p_campfire\"（engine/vfx2/presets/ の名前）か asset:\"engine/vfx2/presets/p_campfire.uvfx\"");
            const auto p = params.value("position", std::vector<float>{0.0f, 0.0f, 0.0f});
            if (p.size() != 3)
                throw McpError(McpErr::InvalidParam, "position must be [x,y,z]", "例: position:[0, 0, 0]（ワールド座標の 3 要素）");
            entt::entity parent = entt::null;
            auto& reg = m_scene->GetRegistry();
            if (params.contains("parent"))
            {
                parent = static_cast<entt::entity>(params["parent"].get<u32>());
                if (!reg.valid(parent))
                    throw McpError(McpErr::NotFound, "invalid parent entity id", "parent は既存エンティティの entityId（dx12_list_entities で取り直す）");
            }
            else if (params.contains("parentName"))
            {
                const std::string pn = params["parentName"].get<std::string>();
                for (auto [pe, tag] : reg.view<const NameTag>().each())
                    if (tag.name == pn) { parent = pe; break; }
                if (parent == entt::null)
                    throw McpError(McpErr::NotFound, "parentName not found: " + pn, "parentName は既存の名前と完全一致が要る");
            }
            entt::entity e = entt::null; std::string asset, err;
            if (!AppAccess::VfxPlace(*this, target, DirectX::XMFLOAT3{p[0], p[1], p[2]}, parent, params.value("name", std::string()), e, asset, err))
                throw McpError(McpErr::InvalidParam, err, "preset 名か assets 相対の .uvfx のパスを確かめる。中身の検査は読み込み時のエラーに場所つきで出る");
            resp["ok"] = true;
            resp["result"] = { {"entityId", static_cast<u32>(e)}, {"name", reg.get<NameTag>(e).name}, {"asset", asset},
                               {"position", {p[0], p[1], p[2]}},
                               {"note", "置いた。粒子は次のフレームから出る。決定論で撮るなら dx12_vfx_seek（t を指定）してから決定論スクリーンショット"} };
        });

    McpDefine("vfx_seek", "entity:any,name:string,resume:bool,t:number", DX12E_MCP_HANDLER
        {
            if (!params.contains("t") || !params["t"].is_number())
                throw McpError(McpErr::InvalidParam, "t（秒）が要る", "例: t:2.0（エフェクトの 2 秒目の状態へ決定論で移る）");
            const double t = params["t"].get<double>();
            if (!(t >= 0.0) || t > 60.0)
                throw McpError(McpErr::InvalidParam, "t は 0〜60 秒", "60 秒より先は固定ステップの再シミュレーションが重くなるので断る");
            vfx::SeekRequest r;
            r.t = t;
            r.resume = params.value("resume", false);
            auto& reg = m_scene->GetRegistry();
            if (params.contains("entity") || params.contains("name"))
            {
                r.entity = ResolveMcpEntity(*m_scene, params);
                if (!reg.all_of<VfxComponent>(r.entity))
                    throw McpError(McpErr::NotFound, "そのエンティティに VfxComponent が無い", "dx12_vfx_place で置いたエンティティの entityId を渡す");
            }
            u32 targets = 0;
            for (auto e : reg.view<VfxComponent>()) if (r.entity == entt::null || r.entity == e) ++targets;
            if (targets == 0)
                throw McpError(McpErr::NotFound, "VFX のエンティティが無い", "先に dx12_vfx_place で置く");
            vfx::Runtime::Get().RequestSeek(r);
            resp["ok"] = true;
            resp["result"] = { {"t", t}, {"targets", targets}, {"paused", !r.resume},
                               {"note", "次のフレームで巻き戻して固定ステップ(1/60s)で t まで再シミュレーションし、resume:false なら止める。撮るのは 1 フレーム以上回した後"} };
        });

    McpDefine("vfx_stats", "", DX12E_MCP_HANDLER
        {
            resp["ok"] = true;
            resp["result"] = VfxStatsJson();
        });
}

uint64_t AppAccess::ModelThumbHandle(Application& app, const std::string& path)
{
    return app.m_thumbRenderer ? app.m_thumbRenderer->GetCachedHandle(path) : 0;
}
} // namespace dx12e
