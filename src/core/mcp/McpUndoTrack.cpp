// ===========================================================================
// MCP の編集を「値のスナップショット」で Undo にする部品（McpUndoTrack.h の実装）
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/mcp/McpUndoTrack.h"

#include <random>

namespace dx12e
{

namespace
{
u64 NewUndoGuid()
{
    // SceneSerializer の NewEntityGuid と同じ作り（あちらは TU ローカルなので写し）。0 は番兵。
    static std::mt19937_64 rng{ std::random_device{}() };
    u64 v = 0;
    while (v == 0) v = rng();
    return v;
}

u64 EnsureGuid(entt::registry& reg, entt::entity e)
{
    if (!reg.valid(e)) return 0;
    if (auto* g = reg.try_get<EntityGuid>(e))
    {
        if (g->value == 0) g->value = NewUndoGuid();
        return g->value;
    }
    return reg.emplace<EntityGuid>(e, EntityGuid{ NewUndoGuid() }).value;
}

// 値が変わったエンティティぶんの前後の値。Undo は逆順・Redo は順に当てる。
class McpEntityStateCommand final : public IUndoCommand
{
public:
    struct Item
    {
        entt::entity e = entt::null;
        u64 guid = 0;
        std::vector<std::unique_ptr<IMcpUndoSlot>> slots;
    };

    McpEntityStateCommand(Scene* scene, std::vector<Item> items)
        : m_scene(scene), m_items(std::move(items)) {}

    void Undo() override { Apply(false); }
    void Redo() override { Apply(true); }
    const char* GetName() const override { return "Entity Edit"; }

private:
    // id が生きていて guid も一致すればそれ。違えば guid で引き直す（生成/削除の取り消しで
    // id が変わった後でも、同じエンティティへ当てるため）。
    static entt::entity Resolve(entt::registry& reg, Item& it)
    {
        if (reg.valid(it.e))
        {
            const auto* g = reg.try_get<EntityGuid>(it.e);
            if (it.guid == 0 || (g && g->value == it.guid)) return it.e;
        }
        if (it.guid != 0)
        {
            const entt::entity found = FindEntityByGuid(reg, it.guid);
            if (found != entt::null) it.e = found;
            return found;
        }
        return entt::null;
    }

    void Apply(bool after)
    {
        if (!m_scene) return;
        auto& reg = m_scene->GetRegistry();
        const size_t n = m_items.size();
        for (size_t k = 0; k < n; ++k)
        {
            Item& it = m_items[after ? k : n - 1 - k];
            const entt::entity e = Resolve(reg, it);
            if (e == entt::null)
            {
                Logger::Warn("[Undo] AI の編集の対象が見つからないので飛ばしました（削除済み？ guid={:016x}）",
                             it.guid);
                continue;
            }
            const size_t m = it.slots.size();
            for (size_t j = 0; j < m; ++j)
                it.slots[after ? j : m - 1 - j]->Apply(*m_scene, reg, e, after);
        }
    }

    Scene* m_scene;
    std::vector<Item> m_items;
};

// シーン設定（ポスト等）の前後。
class McpSceneStateCommand final : public IUndoCommand
{
public:
    explicit McpSceneStateCommand(std::vector<std::unique_ptr<IMcpSceneSlot>> slots)
        : m_slots(std::move(slots)) {}
    void Undo() override { for (auto it = m_slots.rbegin(); it != m_slots.rend(); ++it) (*it)->Apply(false); }
    void Redo() override { for (auto& s : m_slots) s->Apply(true); }
    const char* GetName() const override { return "Scene Settings"; }
private:
    std::vector<std::unique_ptr<IMcpSceneSlot>> m_slots;
};
} // namespace

// ---- 型ごとの当て方（副作用の要る型だけ）----------------------------------

void McpRestore<MeshRenderer>::Apply(Scene& scene, entt::registry& reg, entt::entity e, const MeshRenderer& v)
{
    // ★メッシュ / マテリアルの実体ポインタは今のものを残す。スナップショットの後に
    //   reload_assets がモデルを読み直すと古い Mesh* は解放済みで、そのまま戻すとぶら下がる。
    //   MCP の追跡対象ハンドラ（set_pbr / set_color / set_texture / set_mesh_shader …）は
    //   モデル自体を差し替えないので、値だけ戻せば十分。
    auto* cur = reg.try_get<MeshRenderer>(e);
    if (!cur) return;   // MeshRenderer を足す/消す MCP は無い（追跡時に有無を確かめてある）
    MeshRenderer next = v;
    next.modelPath          = cur->modelPath;
    next.meshes             = cur->meshes;
    next.materials          = cur->materials;
    next.meshNodeTransforms = cur->meshNodeTransforms;
    next._animT             = cur->_animT;
    const bool uvChanged = cur->uvScaleU != next.uvScaleU || cur->uvScaleV != next.uvScaleV;
    *cur = std::move(next);
    // ★uvScale は頂点へ焼く方式なので、値を戻すだけでは絵が戻らない（MeshRendererLookCommand と同じ）。
    if (uvChanged && scene.GetDevice())
        for (auto* mesh : cur->meshes)
            if (mesh) mesh->ApplyUVScale(*scene.GetDevice(), cur->uvScaleU, cur->uvScaleV);
}

void McpRestore<MeshRenderer>::Remove(entt::registry& /*reg*/, entt::entity /*e*/)
{
    // MeshRenderer を消す操作は MCP に無い。メッシュの所有と整合させる必要があるので消さない。
}

void McpRestore<LuaScript>::Apply(Scene& /*scene*/, entt::registry& reg, entt::entity e, const LuaScript& v)
{
    // データ（パス / 有効 / 公開プロパティ）だけ戻し、実行状態は捨てる（ReloadScript と同じ）。
    // 実行状態を写しで戻すと、別のスクリプトの環境を握ったままになる。
    LuaScript next;
    next.scriptPath = v.scriptPath;
    next.enabled    = v.enabled;
    next.props      = v.props;
    reg.emplace_or_replace<LuaScript>(e, std::move(next));
}

void McpRestore<LuaScript>::Remove(entt::registry& reg, entt::entity e)
{
    reg.remove<LuaScript>(e);
}

void McpUndoCaptureParentGuid(entt::registry& reg, const Transform& t, u64& outGuid)
{
    outGuid = 0;
    if (t.parent != entt::null && reg.valid(t.parent))
        outGuid = EnsureGuid(reg, t.parent);
}

void McpUndoFixParent(const entt::registry& reg, Transform& t, u64 parentGuid)
{
    if (t.parent == entt::null) return;
    if (reg.valid(t.parent))
    {
        if (parentGuid == 0) return;
        const auto* g = reg.try_get<EntityGuid>(t.parent);
        if (g && g->value == parentGuid) return;
    }
    // 親が作り直されて id が変わった（または消えた）。guid で引き直し、無ければルートへ（安全側）。
    t.parent = (parentGuid != 0) ? FindEntityByGuid(reg, parentGuid) : entt::null;
}

// ---- McpUndoTracker ---------------------------------------------------------

McpUndoTracker::McpUndoTracker() = default;
McpUndoTracker::~McpUndoTracker() = default;

entt::registry& McpUndoTracker::Registry() { return m_scene->GetRegistry(); }

u64 McpUndoTracker::SceneFingerprint() const
{
    return m_scene ? SceneSettingsFingerprint(*m_scene) : 0;
}

void McpUndoTracker::Begin(Scene* scene, std::string assetsDir)
{
    m_recs.clear();
    m_sceneSlots.clear();
    m_sceneFpTaken = false;
    m_scene     = scene;
    m_assetsDir = std::move(assetsDir);
}

std::string McpUndoTracker::Fingerprint(entt::entity e)
{
    // 変わったかどうかは保存形式（SerializeEntity）で比べる。保存されない実行時の値
    // （アニメの経過時間など）の揺れで空の Undo を積まないため。
    // ★親はここに入らない（SerializeEntity は親を書かない）ので別に足す。
    static u64 s_forceChanged = 0;
    auto& reg = Registry();
    if (!reg.valid(e)) return {};
    std::string fp;
    try { fp = SceneSerializer::SerializeEntity(*m_scene, e, m_assetsDir); }
    catch (const std::exception&) { fp.clear(); }
    // 直列化できない（名前が不正 UTF-8 / NameTag か Transform が無い）ときは「変わった」側に倒す。
    // 取りこぼして Undo できない方が、空の Undo が 1 つ積まれるより害が大きい。
    if (fp.empty()) fp = "<unserializable:" + std::to_string(++s_forceChanged) + ">";
    if (const auto* t = reg.try_get<Transform>(e))
        fp += "|parent=" + std::to_string(static_cast<u32>(entt::to_integral(t->parent)));
    return fp;
}

McpUndoTracker::Rec* McpUndoTracker::RecordFor(entt::entity e)
{
    auto& reg = Registry();
    if (!reg.valid(e)) return nullptr;
    for (auto& r : m_recs)
        if (r.e == e) return &r;
    Rec r;
    r.e        = e;
    r.guid     = EnsureGuid(reg, e);   // 先に振る（振ったこと自体を「変更」と数えないよう指紋より前）
    r.beforeFp = Fingerprint(e);
    m_recs.push_back(std::move(r));
    return &m_recs.back();
}

std::vector<std::unique_ptr<IUndoCommand>> McpUndoTracker::Finish()
{
    std::vector<std::unique_ptr<IUndoCommand>> out;
    if (!m_scene) return out;
    Scene* scene = m_scene;

    // シーン設定: 指紋が変わったときだけ（同じ値の再設定では積まない）
    if (!m_sceneSlots.empty() && SceneFingerprint() != m_sceneFpBefore)
    {
        for (auto& s : m_sceneSlots) s->CaptureAfter();
        out.push_back(std::make_unique<McpSceneStateCommand>(std::move(m_sceneSlots)));
    }
    m_sceneSlots.clear();
    m_sceneFpTaken = false;

    auto&  reg   = Registry();
    std::vector<McpEntityStateCommand::Item> items;
    for (auto& r : m_recs)
    {
        if (!reg.valid(r.e) || r.slots.empty()) continue;
        if (Fingerprint(r.e) == r.beforeFp) continue;   // 何も変わっていない（同じ値の再設定・検証で失敗）
        for (auto& s : r.slots) s->CaptureAfter(reg, r.e);
        McpEntityStateCommand::Item it;
        it.e     = r.e;
        it.guid  = r.guid;
        it.slots = std::move(r.slots);
        items.push_back(std::move(it));
    }
    m_recs.clear();
    m_scene = nullptr;
    if (!items.empty())
        out.push_back(std::make_unique<McpEntityStateCommand>(scene, std::move(items)));
    return out;
}

bool McpUndoTracker::TrackByJsonKey(entt::entity e, const std::string& key)
{
    // set_component / remove_component が受けるキー（RemoveRegisteredComponent + transform +
    // ApplyOrphanComponent の 4 種）と同じ表。キーを足したらここにも足すこと。
    if      (key == "transform")           Track<Transform>(e);
    else if (key == "pointLight")          Track<PointLight>(e);
    else if (key == "directionalLight")    Track<DirectionalLight>(e);
    else if (key == "spotLight")           Track<SpotLight>(e);
    else if (key == "camera")              Track<CameraComponent>(e);
    else if (key == "rigidBody")           Track<RigidBody>(e);
    else if (key == "boxCollider")         Track<BoxCollider>(e);
    else if (key == "sphereCollider")      Track<SphereCollider>(e);
    else if (key == "capsuleCollider")     Track<CapsuleCollider>(e);
    else if (key == "characterController") Track<CharacterController>(e);
    else if (key == "tags")                Track<Tag>(e);
    else if (key == "data")                Track<DataComponent>(e);
    else if (key == "sprite2d")            Track<Sprite2D>(e);
    else if (key == "audioSource")         Track<AudioSource>(e);
    else if (key == "particleEmitter")     Track<ParticleEmitter>(e);
    else if (key == "trigger")             Track<Trigger>(e);
    else if (key == "gimmick")             Track<Gimmick>(e);
    else if (key == "convexHullCollider")  Track<ConvexHullCollider>(e);
    else if (key == "meshCollider")        Track<MeshCollider>(e);
    else if (key == "luaScript")           Track<LuaScript>(e);
    else if (key == "trailRenderer")       Track<TrailRenderer>(e);
    else if (key == "decal")               Track<DecalComponent>(e);
    else if (key == "networkIdentity")     Track<NetworkIdentity>(e);
    else if (key == "networkTransform")    Track<NetworkTransform>(e);
    else if (key == "uiCanvas")            Track<UICanvas>(e);
    else if (key == "uiRect")              Track<UIRect>(e);
    else if (key == "uiImage")             Track<UIImage>(e);
    else if (key == "uiText")              Track<UIText>(e);
    else if (key == "uiButton")            Track<UIButton>(e);
    else if (key == "uiSlider")            Track<UISlider>(e);
    else if (key == "uiToggle")            Track<UIToggle>(e);
    else if (key == "uiScrollView")        Track<UIScrollView>(e);
    else if (key == "uiLayout")            Track<UILayout>(e);
    else if (key == "uiAnimator")          Track<UIAnimator>(e);
    else if (key == "uiAnimPlayer")        Track<UIAnimPlayer>(e);
    else if (key == "spriteAnimator")      Track<SpriteAnimator>(e);
    else if (key == "animatorController")  Track<AnimatorController>(e);
    else if (key == "footIK")              Track<FootIK>(e);
    else return false;
    return true;
}

} // namespace dx12e
