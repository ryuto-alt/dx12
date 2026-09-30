#include "core/SequencerApply.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <DirectXMath.h>

#include "core/SequencerBinding.h"
#include "ecs/ComponentMeta.h"
#include "renderer/PostProcessSettings.h"

namespace dx12e
{
namespace seqhost
{

using namespace DirectX;

// ===========================================================================
// プロパティアダプタ
// ===========================================================================
namespace
{

template <typename T>
entt::meta_any HandleOf(entt::registry& reg, entt::entity e)
{
    if (e == entt::null || !reg.valid(e)) return {};
    T* p = reg.try_get<T>(e);
    if (!p) return {};
    return entt::forward_as_meta(*p);
}

struct CompEntry
{
    const char* name;
    CompHandleFn fn;
};

// ComponentMeta.cpp に meta 登録されている「実体を持つコンポーネント」。名前は meta の型名と同じ。
// (Terrain / SculptMesh / Tag / PrefabLink / NameTag は対象外: 配列や識別子で、補間する値ではない)
const std::vector<CompEntry>& CompTable()
{
    static const std::vector<CompEntry> t = {
        { "Transform", &HandleOf<Transform> },
        { "PointLight", &HandleOf<PointLight> },
        { "DirectionalLight", &HandleOf<DirectionalLight> },
        { "SpotLight", &HandleOf<SpotLight> },
        { "CameraComponent", &HandleOf<CameraComponent> },
        { "RigidBody", &HandleOf<RigidBody> },
        { "BoxCollider", &HandleOf<BoxCollider> },
        { "SphereCollider", &HandleOf<SphereCollider> },
        { "CapsuleCollider", &HandleOf<CapsuleCollider> },
        { "CharacterController", &HandleOf<CharacterController> },
        { "AudioReverbZone", &HandleOf<AudioReverbZone> },
        { "AudioSource", &HandleOf<AudioSource> },
        { "Sprite2D", &HandleOf<Sprite2D> },
        { "TrailRenderer", &HandleOf<TrailRenderer> },
        { "DecalComponent", &HandleOf<DecalComponent> },
        { "UICanvas", &HandleOf<UICanvas> },
        { "UIRect", &HandleOf<UIRect> },
        { "UIImage", &HandleOf<UIImage> },
        { "UIText", &HandleOf<UIText> },
        { "UIButton", &HandleOf<UIButton> },
        { "UISlider", &HandleOf<UISlider> },
        { "UIScrollView", &HandleOf<UIScrollView> },
        { "UILayout", &HandleOf<UILayout> },
        { "UIToggle", &HandleOf<UIToggle> },
        { "UIAnimator", &HandleOf<UIAnimator> },
        { "SpriteAnimator", &HandleOf<SpriteAnimator> },
        { "FootIK", &HandleOf<FootIK> },
        { "Brain", &HandleOf<Brain> },
    };
    return t;
}

} // namespace

CompHandleFn FindCompHandle(std::string_view compName)
{
    for (const CompEntry& e : CompTable())
        if (compName == e.name) return e.fn;
    return nullptr;
}

std::vector<std::string> SupportedComponentNames()
{
    std::vector<std::string> v;
    for (const CompEntry& e : CompTable()) v.emplace_back(e.name);
    v.emplace_back("Light");
    return v;
}

bool ResolveFieldRef(entt::registry& reg, entt::entity entity, std::string_view comp, std::string_view field,
                     FieldRef& out, std::string& err)
{
    out = FieldRef{};
    std::string compName(comp);
    // 総称 "Light": そのエンティティが持っているライトへ解決する
    if (compName == "Light")
    {
        if (entity != entt::null && reg.valid(entity))
        {
            if (reg.all_of<PointLight>(entity)) compName = "PointLight";
            else if (reg.all_of<SpotLight>(entity)) compName = "SpotLight";
            else if (reg.all_of<DirectionalLight>(entity)) compName = "DirectionalLight";
        }
        if (compName == "Light") { err = "\"Light\" に対応するライト(PointLight / SpotLight / DirectionalLight)をこのエンティティが持っていない"; return false; }
    }
    const CompHandleFn fn = FindCompHandle(compName);
    if (!fn)
    {
        std::string all;
        for (const std::string& n : SupportedComponentNames()) { if (!all.empty()) all += ", "; all += n; }
        err = "未対応のコンポーネント \"" + compName + "\"(対応: " + all + ")";
        return false;
    }
    RegisterCoreComponentMeta();   // 冪等
    const entt::meta_type mt = entt::resolve(entt::hashed_string::value(compName.data(), compName.size()));
    if (!mt) { err = "コンポーネント \"" + compName + "\" は meta に登録されていない"; return false; }
    const entt::meta_data d = mt.data(entt::hashed_string::value(field.data(), field.size()));
    if (!d) { err = compName + " に \"" + std::string(field) + "\" というフィールドは無い"; return false; }

    const entt::meta_type t = d.type();
    FieldRef::Kind k = FieldRef::Kind::Invalid;
    if (t == entt::resolve<float>()) k = FieldRef::Kind::Float;
    else if (t == entt::resolve<std::int32_t>() || t == entt::resolve<std::uint32_t>()) k = FieldRef::Kind::Int;
    else if (t == entt::resolve<bool>()) k = FieldRef::Kind::Bool;
    else if (t == entt::resolve<XMFLOAT2>()) k = FieldRef::Kind::Vec2;
    else if (t == entt::resolve<XMFLOAT3>()) k = FieldRef::Kind::Vec3;
    else if (t == entt::resolve<XMFLOAT4>()) k = FieldRef::Kind::Vec4;
    else if (t.is_enum()) k = FieldRef::Kind::Enum;
    if (k == FieldRef::Kind::Invalid) { err = compName + "." + std::string(field) + " は補間できる型(数値 / bool / ベクトル / 列挙)ではない"; return false; }
    out.kind = k;
    out.data = d;
    out.comp = compName;
    out.field = std::string(field);
    return true;
}

// ===========================================================================
// ポストプロセスの名前表(DX12E_POST_FIELDS から生成する = 名前の二重管理をしない)
// ===========================================================================
namespace
{
#define DX12E_SEQ_PB(f) { #f, 'B', -1, [](PostProcessSettings& p, int, double v) { p.f = v >= 0.5; } },
#define DX12E_SEQ_PF(f) { #f, 'F', -1, [](PostProcessSettings& p, int, double v) { p.f = static_cast<float>(v); } },
#define DX12E_SEQ_PI(f) { #f, 'I', -1, [](PostProcessSettings& p, int, double v) { p.f = static_cast<int>(std::llround(v)); } },
#define DX12E_SEQ_PV(f) { #f, 'V', -1, [](PostProcessSettings& p, int c, double v) { \
        if (c == 0) p.f.x = static_cast<float>(v); else if (c == 1) p.f.y = static_cast<float>(v); else if (c == 2) p.f.z = static_cast<float>(v); } },
#define DX12E_SEQ_PS(f) { #f, 'S', -1, nullptr },

std::vector<PostFieldInfo> BuildPostTable()
{
    std::vector<PostFieldInfo> t = {
        DX12E_POST_FIELDS(DX12E_SEQ_PB, DX12E_SEQ_PF, DX12E_SEQ_PI, DX12E_SEQ_PV, DX12E_SEQ_PS)
    };
    // 「XxxOn」の bool は、続く値フィールド(次の XxxOn まで)の ON スイッチ。XxxOn 以外の bool(enabled / grainColored …)は割り込まない。
    int lastOn = -1;
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        const std::string n = t[i].name;
        const bool isOn = t[i].kind == 'B' && n.size() > 2 && n.compare(n.size() - 2, 2, "On") == 0;
        if (isOn) { lastOn = static_cast<int>(i); continue; }
        if (t[i].kind != 'B') t[i].onIndex = lastOn;
    }
    return t;
}
#undef DX12E_SEQ_PB
#undef DX12E_SEQ_PF
#undef DX12E_SEQ_PI
#undef DX12E_SEQ_PV
#undef DX12E_SEQ_PS
} // namespace

const std::vector<PostFieldInfo>& PostFieldTable()
{
    static const std::vector<PostFieldInfo> t = BuildPostTable();
    return t;
}

int FindPostField(std::string_view name)
{
    const auto& t = PostFieldTable();
    for (std::size_t i = 0; i < t.size(); ++i)
        if (name == t[i].name) return static_cast<int>(i);
    return -1;
}

void ApplyPostWrites(PostProcessSettings& pp, const std::vector<PostWrite>& writes, bool includeCamera)
{
    const auto& t = PostFieldTable();
    // 1 周目: 値のフィールド(F / I / V)。対応する XxxOn を自動で ON にする
    for (const PostWrite& w : writes)
    {
        if (w.camera && !includeCamera) continue;
        if (w.field < 0 || w.field >= static_cast<int>(t.size())) continue;
        const PostFieldInfo& f = t[static_cast<std::size_t>(w.field)];
        if (f.kind == 'B' || !f.set) continue;
        f.set(pp, w.comp, w.value);
        if (f.onIndex >= 0) t[static_cast<std::size_t>(f.onIndex)].set(pp, 0, 1.0);
    }
    // 2 周目: 明示の bool(明示の OFF が自動 ON に勝つ)
    for (const PostWrite& w : writes)
    {
        if (w.camera && !includeCamera) continue;
        if (w.field < 0 || w.field >= static_cast<int>(t.size())) continue;
        const PostFieldInfo& f = t[static_cast<std::size_t>(w.field)];
        if (f.kind == 'B' && f.set) f.set(pp, 0, w.value);
    }
}

// ===========================================================================
// バインディング + トラックのコンパイル
// ===========================================================================
std::uint32_t AliveEntityCount(const entt::registry& reg)
{
    // エンティティ用ストレージの free_list() = 生きているエンティティの数(swap_only の規則。registry::valid の判定と同じ値)
    const auto* s = reg.storage<entt::entity>();
    return s ? static_cast<std::uint32_t>(s->free_list()) : 0u;
}

namespace
{
// チャンネル名 "position.x" → (group, comp)
bool ParseTransformChannel(std::string_view name, int& group, int& comp)
{
    const std::size_t dot = name.find('.');
    if (dot == std::string_view::npos || dot + 2 != name.size()) return false;
    const std::string_view g = name.substr(0, dot);
    if (g == "position") group = 0;
    else if (g == "rotation") group = 1;
    else if (g == "scale") group = 2;
    else return false;
    const char c = name[dot + 1];
    if (c == 'x') comp = 0;
    else if (c == 'y') comp = 1;
    else if (c == 'z') comp = 2;
    else return false;
    return true;
}

// ベクトルの成分名 → 添字(x/r=0, y/g=1, z/b=2, w/a=3)。"value" は -1(スカラー)。不正は -2。
int VecComponentOf(std::string_view name)
{
    if (name == "value") return -1;
    if (name.size() == 1)
    {
        switch (name[0])
        {
        case 'x': case 'r': return 0;
        case 'y': case 'g': return 1;
        case 'z': case 'b': return 2;
        case 'w': case 'a': return 3;
        default: break;
        }
    }
    return -2;
}

// "tint.x" / "tint" / "vignetteColor.r" → (フィールド名, 成分)。成分が無ければ -1
void SplitPostChannel(std::string_view name, std::string& field, int& comp)
{
    const std::size_t dot = name.rfind('.');
    if (dot != std::string_view::npos && dot + 2 == name.size())
    {
        const int c = VecComponentOf(name.substr(dot + 1));
        if (c >= 0)
        {
            field = std::string(name.substr(0, dot));
            comp = c;
            return;
        }
    }
    field = std::string(name);
    comp = -1;
}

void CompileTrack(const seq::Track& tr, entt::registry& reg, entt::entity entity, BoundTrack& out, const std::string& where)
{
    out.type = tr.type;
    out.chans.assign(tr.channels.size(), BoundChannel{});
    std::string firstWarn;
    const auto warn = [&](const std::string& m) { if (firstWarn.empty()) firstWarn = where + ": " + m; };

    switch (tr.type)
    {
    case seq::TrackType::Transform:
        for (std::size_t i = 0; i < tr.channels.size(); ++i)
        {
            int g = 0, c = 0;
            if (tr.rotation == seq::RotationMode::Quat && tr.channels[i].name.rfind("rotation.", 0) == 0) continue;   // quats へ(適用側が別に扱う)
            if (!ParseTransformChannel(tr.channels[i].name, g, c))
            {
                warn("Transform のチャンネル名が不明: " + tr.channels[i].name + "(position./rotation./scale. の x y z)");
                continue;
            }
            out.chans[i].target = BoundChannel::Target::Transform;
            out.chans[i].a = g;
            out.chans[i].b = c;
        }
        break;

    case seq::TrackType::Property:
    case seq::TrackType::Light:
    {
        const std::size_t dot = tr.path.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= tr.path.size())
        {
            warn("path が \"コンポーネント.フィールド\" の形ではない: \"" + tr.path + "\"");
            break;
        }
        FieldRef f;
        std::string err;
        if (!ResolveFieldRef(reg, entity, std::string_view(tr.path).substr(0, dot), std::string_view(tr.path).substr(dot + 1), f, err))
        {
            warn(err);
            break;
        }
        for (std::size_t i = 0; i < tr.channels.size(); ++i)
        {
            const int c = VecComponentOf(tr.channels[i].name);
            const int comps = f.Components();
            const bool scalar = (f.kind == FieldRef::Kind::Float || f.kind == FieldRef::Kind::Int ||
                                 f.kind == FieldRef::Kind::Bool || f.kind == FieldRef::Kind::Enum);
            if (c == -2 || (scalar && c != -1) || (!scalar && (c < 0 || c >= comps)))
            {
                warn("チャンネル \"" + tr.channels[i].name + "\" は " + tr.path + " に対応しない" +
                     (scalar ? "(スカラーのチャンネル名は value)" : "(x y z w または r g b a)"));
                continue;
            }
            out.chans[i].target = BoundChannel::Target::Field;
            out.chans[i].a = c;
            out.chans[i].field = f;
        }
        break;
    }

    case seq::TrackType::Camera:
        for (std::size_t i = 0; i < tr.channels.size(); ++i)
        {
            const std::string& n = tr.channels[i].name;
            const char* field = nullptr;
            if (n == "fov") field = "fovDegrees";
            else if (n == "near") field = "nearClip";
            else if (n == "far") field = "farClip";
            else if (n == "orthoSize") field = "orthoSize";
            if (field)
            {
                FieldRef f;
                std::string err;
                if (!ResolveFieldRef(reg, entity, "CameraComponent", field, f, err)) { warn(err); continue; }
                out.chans[i].target = BoundChannel::Target::Field;
                out.chans[i].a = -1;
                out.chans[i].field = f;
                continue;
            }
            if (n == "dofAperture" || n == "dofFocalLength" || n == "dofBlurSize" || n == "dofFocusDist")
            {
                out.chans[i].target = BoundChannel::Target::Dof;
                out.chans[i].a = FindPostField(n);
                continue;
            }
            warn("Camera のチャンネル名が不明: " + n + "(fov near far orthoSize dofAperture dofFocalLength dofBlurSize dofFocusDist)");
        }
        break;

    case seq::TrackType::Post:
        for (std::size_t i = 0; i < tr.channels.size(); ++i)
        {
            std::string field;
            int comp = -1;
            SplitPostChannel(tr.channels[i].name, field, comp);
            const int idx = FindPostField(field);
            if (idx < 0) { warn("ポストのフィールド \"" + tr.channels[i].name + "\" は無い(post.names() の名前)"); continue; }
            const PostFieldInfo& pf = PostFieldTable()[static_cast<std::size_t>(idx)];
            if (pf.kind == 'S') { warn("文字列のフィールドは補間できない: " + field); continue; }
            if (pf.kind == 'V' && comp < 0) { warn(field + " はベクトル(" + field + ".r / .g / .b のチャンネルで書く)"); continue; }
            if (pf.kind != 'V' && comp >= 0) { warn(field + " はスカラー(成分の指定は要らない)"); continue; }
            out.chans[i].target = BoundChannel::Target::Post;
            out.chans[i].a = idx;
            out.chans[i].b = comp < 0 ? 0 : comp;
        }
        break;

    case seq::TrackType::TimeScale:
        for (std::size_t i = 0; i < tr.channels.size(); ++i)
        {
            if (tr.channels[i].name == "value") out.chans[i].target = BoundChannel::Target::TimeScale;
            else warn("timeScale のチャンネルは value だけ");
        }
        break;

    case seq::TrackType::Aim:
        warn("aim(注視の制約)は未対応(S2a)");
        break;

    case seq::TrackType::AnimationClip:
    case seq::TrackType::Audio:
    case seq::TrackType::VfxSpawn:
    case seq::TrackType::Subsequence:
        warn(std::string(seq::TrackTypeName(tr.type)) + " トラックは未対応(S4 / S5。データは保持される)");
        break;

    case seq::TrackType::Shake:
    case seq::TrackType::Event:
    default:
        break;
    }
    out.warning = firstWarn;
}
} // namespace

void BindSequence(const seq::Sequence& seq, entt::registry& reg, const BindOptions& opt, BoundSequence& out)
{
    out = BoundSequence{};
    const seqhost::EngineBindingResolver resolver(reg);
    out.set = seq::ResolveBindings(seq, resolver, opt.overrides);
    out.mask = out.set.EnabledMask();
    out.entity.assign(seq.bindings.size(), entt::null);
    out.guid.assign(seq.bindings.size(), 0);
    out.tracks.resize(seq.bindings.size());
    out.aliveCount = AliveEntityCount(reg);

    for (const seq::BindingIssue& is : out.set.issues) out.warnings.push_back(is.message);

    for (std::size_t bi = 0; bi < seq.bindings.size(); ++bi)
    {
        const seq::Binding& b = seq.bindings[bi];
        const seq::BindingResolution& res = out.set.byBinding[bi];
        if (b.kind == seq::BindingKind::Entity && res.Resolved())
        {
            const entt::entity e = ToEntity(res.target);
            if (e != entt::null && reg.valid(e))
            {
                out.entity[bi] = e;
                if (opt.ensureGuids) out.guid[bi] = EnsureEntityGuid(reg, e);
                else if (const auto* g = reg.try_get<EntityGuid>(e)) out.guid[bi] = g->value;
            }
        }
        out.tracks[bi].resize(b.tracks.size());
        for (std::size_t ti = 0; ti < b.tracks.size(); ++ti)
        {
            const seq::Track& tr = b.tracks[ti];
            const std::string where = "バインディング \"" + b.name + "\" のトラック " + tr.id;
            CompileTrack(tr, reg, out.entity[bi], out.tracks[bi][ti], where);
            // 対象がエンティティでなければならないトラックが scene / 未解決に付いている
            if (b.kind == seq::BindingKind::Scene)
            {
                const bool sceneOk = tr.type == seq::TrackType::Post || tr.type == seq::TrackType::TimeScale ||
                                     tr.type == seq::TrackType::Event || tr.type == seq::TrackType::AnimationClip ||
                                     tr.type == seq::TrackType::Audio || tr.type == seq::TrackType::VfxSpawn ||
                                     tr.type == seq::TrackType::Subsequence;
                if (!sceneOk) out.tracks[bi][ti].warning = where + ": このトラックは scene バインディングには付けられない(エンティティが要る)";
            }
            if (!out.tracks[bi][ti].warning.empty() && out.mask[bi]) out.warnings.push_back(out.tracks[bi][ti].warning);
        }
    }
}

bool BoundIsFresh(const BoundSequence& b, const entt::registry& reg, bool checkAliveCount)
{
    for (std::size_t i = 0; i < b.entity.size(); ++i)
    {
        const entt::entity e = b.entity[i];
        if (e == entt::null) continue;
        if (!reg.valid(e)) return false;
        if (b.guid[i] != 0)
        {
            const auto* g = reg.try_get<EntityGuid>(e);
            if (!g || g->value != b.guid[i]) return false;
        }
    }
    return !checkAliveCount || b.aliveCount == AliveEntityCount(reg);
}

// ===========================================================================
// PreAnimatedState
// ===========================================================================
namespace
{
std::string TfKey(std::uint64_t guid) { return std::to_string(guid) + "|tf"; }
std::string FieldKey(std::uint64_t guid, const FieldRef& f) { return std::to_string(guid) + "|" + f.comp + "." + f.field; }

bool Near(float a, float b) { return std::fabs(a - b) <= 1e-5f * (1.0f + std::fabs(a)); }
bool SameVec3(const XMFLOAT3& a, const XMFLOAT3& b) { return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z); }
} // namespace

void PreAnimatedState::SaveTransform(const entt::registry& reg, entt::entity e, std::uint64_t guid)
{
    if (guid == 0 || e == entt::null || !reg.valid(e)) return;
    const auto* tf = reg.try_get<Transform>(e);
    if (!tf) return;
    const std::string key = TfKey(guid);
    if (m_index.count(key)) return;
    Entry en;
    en.guid = guid;
    en.isTransform = true;
    en.tf = *tf;
    m_index[key] = m_entries.size();
    m_entries.push_back(std::move(en));
}

void PreAnimatedState::SaveField(std::uint64_t guid, const FieldRef& f, entt::meta_any& handle)
{
    if (guid == 0 || !f.Valid() || !handle) return;
    const std::string key = FieldKey(guid, f);
    if (m_index.count(key)) return;
    Entry en;
    en.guid = guid;
    en.isTransform = false;
    en.field = f;
    en.value = f.data.get(handle);   // 値のコピー
    m_index[key] = m_entries.size();
    m_entries.push_back(std::move(en));
}

bool PreAnimatedState::TransformDrifted(std::uint64_t guid, const Transform& current) const
{
    const auto it = m_index.find(TfKey(guid));
    if (it == m_index.end()) return false;
    const Entry& en = m_entries[it->second];
    if (!en.hasLastWritten) return false;
    return !(SameVec3(en.lastWritten.position, current.position) && SameVec3(en.lastWritten.rotation, current.rotation) &&
             SameVec3(en.lastWritten.scale, current.scale));
}

void PreAnimatedState::NoteTransformWritten(std::uint64_t guid, const Transform& written)
{
    const auto it = m_index.find(TfKey(guid));
    if (it == m_index.end()) return;
    Entry& en = m_entries[it->second];
    en.lastWritten = written;
    en.hasLastWritten = true;
}

int PreAnimatedState::RestoreAll(entt::registry& reg, bool keepMissing)
{
    if (m_entries.empty()) return 0;
    // guid → entity(退避が多いときのため 1 回だけ索引を作る)
    std::unordered_map<std::uint64_t, entt::entity> byGuid;
    for (auto [e, g] : reg.view<const EntityGuid>().each())
        if (g.value != 0) byGuid.emplace(g.value, e);

    int restored = 0;
    std::vector<Entry> kept;   // keepMissing: 戻す先が無かったもの(逆順に見るので、最後に順序を戻して残す)
    for (std::size_t i = m_entries.size(); i-- > 0;)
    {
        Entry& en = m_entries[i];
        const auto it = byGuid.find(en.guid);
        if (it == byGuid.end())   // 消えたエンティティ(戻す先が無い)
        {
            if (keepMissing) kept.push_back(std::move(en));
            continue;
        }
        const entt::entity e = it->second;
        if (en.isTransform)
        {
            if (auto* tf = reg.try_get<Transform>(e))
            {
                tf->position = en.tf.position;
                tf->rotation = en.tf.rotation;
                tf->scale = en.tf.scale;
                tf->quaternion = en.tf.quaternion;
                tf->useQuaternion = en.tf.useQuaternion;
                ++restored;
            }
        }
        else
        {
            const CompHandleFn fn = FindCompHandle(en.field.comp);
            if (fn)
            {
                entt::meta_any h = fn(reg, e);
                if (h && en.field.data.set(h, en.value)) ++restored;
            }
        }
    }
    Clear();
    if (keepMissing)
        for (std::size_t i = kept.size(); i-- > 0;)
        {
            const Entry& en = kept[i];
            m_index[en.isTransform ? TfKey(en.guid) : FieldKey(en.guid, en.field)] = m_entries.size();
            m_entries.push_back(std::move(kept[i]));
        }
    return restored;
}

// ===========================================================================
// 適用
// ===========================================================================
void ShakeOffset(double timeSec, double amp, double freq, double seed, double decayPow, double progress01, double weight,
                 float& x, float& y, float& z)
{
    const double k = std::clamp(progress01, 0.0, 1.0);
    const double decay = decayPow <= 0.0 ? 1.0 : std::pow(1.0 - k, decayPow);
    const double a = amp * decay * weight;
    const double ph = timeSec * freq + seed * 12.9898;
    x = static_cast<float>(std::sin(ph * 1.7) * a);
    y = static_cast<float>(std::sin(ph * 2.3 + 1.1) * a);
    z = static_cast<float>(std::sin(ph * 1.3 + 2.7) * a);
}

namespace
{
struct TfWork
{
    entt::entity e = entt::null;
    std::uint64_t guid = 0;
    Transform tf{};
    bool touched = false;
};

TfWork* FindWork(std::vector<TfWork>& v, entt::entity e)
{
    for (TfWork& w : v)
        if (w.e == e) return &w;
    return nullptr;
}

void AddWarnOnce(ApplyOutput& out, const std::string& m)
{
    if (std::find(out.warnings.begin(), out.warnings.end(), m) == out.warnings.end()) out.warnings.push_back(m);
}

void WriteFieldValue(entt::meta_any& handle, const FieldRef& f, int comp, double v)
{
    switch (f.kind)
    {
    case FieldRef::Kind::Float: f.data.set(handle, static_cast<float>(v)); break;
    case FieldRef::Kind::Bool: f.data.set(handle, v >= 0.5); break;
    case FieldRef::Kind::Int:
        if (f.data.type() == entt::resolve<std::uint32_t>())
            f.data.set(handle, static_cast<std::uint32_t>(std::max<long long>(0, std::llround(v))));
        else
            f.data.set(handle, static_cast<std::int32_t>(std::llround(v)));
        break;
    case FieldRef::Kind::Enum:
    {
        entt::meta_any a{ static_cast<int>(std::llround(v)) };
        if (a.allow_cast(f.data.type())) f.data.set(handle, a);
        break;
    }
    case FieldRef::Kind::Vec2:
    {
        XMFLOAT2 cur = f.data.get(handle).cast<XMFLOAT2>();
        if (comp == 0) cur.x = static_cast<float>(v); else if (comp == 1) cur.y = static_cast<float>(v);
        f.data.set(handle, cur);
        break;
    }
    case FieldRef::Kind::Vec3:
    {
        XMFLOAT3 cur = f.data.get(handle).cast<XMFLOAT3>();
        if (comp == 0) cur.x = static_cast<float>(v); else if (comp == 1) cur.y = static_cast<float>(v); else if (comp == 2) cur.z = static_cast<float>(v);
        f.data.set(handle, cur);
        break;
    }
    case FieldRef::Kind::Vec4:
    {
        XMFLOAT4 cur = f.data.get(handle).cast<XMFLOAT4>();
        if (comp == 0) cur.x = static_cast<float>(v); else if (comp == 1) cur.y = static_cast<float>(v);
        else if (comp == 2) cur.z = static_cast<float>(v); else if (comp == 3) cur.w = static_cast<float>(v);
        f.data.set(handle, cur);
        break;
    }
    case FieldRef::Kind::Invalid: break;
    }
}
} // namespace

void ApplySequence(const seq::Sequence& seq, const seq::EvalResult& res, const BoundSequence& bound,
                   entt::registry& reg, const ApplyOptions& opt, ApplyOutput& out)
{
    std::vector<TfWork> works;
    const double tSec = seq::TicksToSeconds(res.t, seq.ticksPerSecond);

    // カットのカメラ(カメラ以外を指していても、その対象を返す)
    out.cutIndex = res.cutIndex;
    if (res.cutBinding >= 0 && static_cast<std::size_t>(res.cutBinding) < bound.entity.size())
    {
        const entt::entity ce = bound.entity[static_cast<std::size_t>(res.cutBinding)];
        if (ce != entt::null && reg.valid(ce) && reg.all_of<CameraComponent>(ce)) out.cutCamera = ce;
        else if (ce != entt::null) AddWarnOnce(out, "カットのカメラに CameraComponent が無い: バインディング \"" +
                                                        seq.bindings[static_cast<std::size_t>(res.cutBinding)].name + "\"");
    }
    const bool noCuts = seq.cuts.empty();

    // Transform 用の作業コピーを取り出す(エンティティ単位。複数のバインディングが同じ対象でも 1 つにまとめる)
    const auto workFor = [&](int b) -> TfWork* {
        const std::size_t bi = static_cast<std::size_t>(b);
        const entt::entity e = bound.entity[bi];
        if (e == entt::null || !reg.valid(e)) { ++out.skippedMissing; return nullptr; }
        if (TfWork* w = FindWork(works, e)) return w;
        const auto* tf = reg.try_get<Transform>(e);
        if (!tf) { ++out.skippedMissing; return nullptr; }
        TfWork w;
        w.e = e;
        w.guid = bound.guid[bi];
        if (w.guid == 0) if (const auto* g = reg.try_get<EntityGuid>(e)) w.guid = g->value;
        w.tf = *tf;
        works.push_back(w);
        return &works.back();
    };

    // ---- チャンネル(カーブ族)----------------------------------------------------
    for (const seq::ChannelSample& cs : res.channels)
    {
        const std::size_t bi = static_cast<std::size_t>(cs.binding), ti = static_cast<std::size_t>(cs.track), ci = static_cast<std::size_t>(cs.channel);
        if (bi >= bound.tracks.size() || ti >= bound.tracks[bi].size()) continue;
        const BoundTrack& bt = bound.tracks[bi][ti];
        if (ci >= bt.chans.size()) continue;
        const BoundChannel& bc = bt.chans[ci];
        const entt::entity e = bound.entity[bi];

        switch (bc.target)
        {
        case BoundChannel::Target::None: break;

        case BoundChannel::Target::Transform:
        {
            TfWork* w = workFor(cs.binding);
            if (!w) break;
            const float v = static_cast<float>(cs.value);
            XMFLOAT3* dst = bc.a == 0 ? &w->tf.position : bc.a == 1 ? &w->tf.rotation : &w->tf.scale;
            if (bc.b == 0) dst->x = v; else if (bc.b == 1) dst->y = v; else dst->z = v;
            if (bc.a == 1) w->tf.useQuaternion = false;
            w->touched = true;
            break;
        }

        case BoundChannel::Target::Field:
        {
            if (e == entt::null || !reg.valid(e)) { ++out.skippedMissing; break; }
            const CompHandleFn fn = FindCompHandle(bc.field.comp);
            if (!fn) break;
            entt::meta_any h = fn(reg, e);
            if (!h) { ++out.skippedMissing; break; }
            if (opt.pre)
            {
                std::uint64_t g = bound.guid[bi];
                if (g == 0) if (const auto* gp = reg.try_get<EntityGuid>(e)) g = gp->value;
                opt.pre->SaveField(g, bc.field, h);
            }
            WriteFieldValue(h, bc.field, bc.a, cs.value);
            ++out.fieldsWritten;
            break;
        }

        case BoundChannel::Target::Post:
            out.post.push_back(PostWrite{ bc.a, bc.b, cs.value });
            break;

        case BoundChannel::Target::Dof:
            // 描画時の上書きが効くのは「カットで選ばれたカメラ」の DoF だけ(カットが無いシーケンスは全カメラトラック)
            if (noCuts || cs.binding == res.cutBinding)
            {
                out.post.push_back(PostWrite{ bc.a, 0, cs.value, true });
                const int on = FindPostField("dofOn");
                if (on >= 0) out.post.push_back(PostWrite{ on, 0, 1.0, true });
            }
            break;

        case BoundChannel::Target::TimeScale:
            out.hasTimeScale = true;
            out.timeScale = static_cast<float>(std::max(0.0, cs.value));
            break;
        }
    }

    // ---- クォータニオン回転 ---------------------------------------------------------
    for (const seq::QuatSample& qs : res.quats)
    {
        if (static_cast<std::size_t>(qs.binding) >= bound.entity.size()) continue;
        TfWork* w = workFor(qs.binding);
        if (!w) continue;
        const XMFLOAT4 q(static_cast<float>(qs.q.x), static_cast<float>(qs.q.y), static_cast<float>(qs.q.z), static_cast<float>(qs.q.w));
        w->tf.quaternion = q;
        w->tf.rotation = QuaternionToEulerDegrees(q);   // 保存は Euler(quaternion は実行時専用。Components.h)
        w->tf.useQuaternion = false;
        w->touched = true;
    }

    // ---- Transform を書く(物理との競合規則)-------------------------------------------
    for (TfWork& w : works)
    {
        if (!w.touched) continue;
        const std::string nm = reg.all_of<NameTag>(w.e) ? reg.get<NameTag>(w.e).name : std::string("(無名)");
        if (opt.physicsActive)
        {
            const auto* rb = reg.try_get<RigidBody>(w.e);
            const bool dynamicBody = rb && rb->motionType == MotionType::Dynamic && rb->bodyId != kInvalidBodyId;
            const bool character = reg.all_of<CharacterController>(w.e);
            if (dynamicBody || character)
            {
                // 物理が(この後の Update で)Transform を上書きするので、書いても無駄。「物理に任せたい対象はバインドしない」運用。
                ++out.skippedPhysics;
                AddWarnOnce(out, "\"" + nm + "\" は" + (character ? "キャラクターコントローラ" : "動的な剛体") +
                                     "なので Transform を書かなかった(物理が位置を決める。キネマティックに変えるか、物理を持たない対象を指す)");
                continue;
            }
            if (rb && rb->motionType == MotionType::Static && rb->bodyId != kInvalidBodyId)
                AddWarnOnce(out, "\"" + nm + "\" は静的な剛体: 見た目は動くが当たり判定は動かない(動かすならキネマティックにする)");
        }
        auto* tf = reg.try_get<Transform>(w.e);
        if (!tf) continue;
        if (opt.pre)
        {
            opt.pre->SaveTransform(reg, w.e, w.guid);
            if (opt.pre->TransformDrifted(w.guid, *tf)) ++out.userDrift;
        }
        tf->position = w.tf.position;
        tf->rotation = w.tf.rotation;
        tf->scale = w.tf.scale;
        tf->quaternion = w.tf.quaternion;
        tf->useQuaternion = w.tf.useQuaternion;
        if (opt.pre) opt.pre->NoteTransformWritten(w.guid, *tf);
        ++out.transformsWritten;
    }

    // ---- カメラの DoF フォーカス(Transform を書いた後。カメラ → 対象のビュー距離)-------------
    for (std::size_t bi = 0; bi < seq.bindings.size(); ++bi)
    {
        if (bi >= bound.mask.size() || !bound.mask[bi]) continue;
        if (!noCuts && static_cast<int>(bi) != res.cutBinding) continue;
        const entt::entity cam = bound.entity[bi];
        if (cam == entt::null || !reg.valid(cam)) continue;
        for (const seq::Track& tr : seq.bindings[bi].tracks)
        {
            if (tr.type != seq::TrackType::Camera || tr.mute) continue;
            const seq::SeqValue* focus = tr.params.Find("focus");
            if (!focus || !focus->IsObject()) continue;
            const std::string tb = focus->GetString("binding");
            const int ti = seq::FindBinding(seq, tb);
            if (ti < 0 || static_cast<std::size_t>(ti) >= bound.entity.size()) continue;
            const entt::entity target = bound.entity[static_cast<std::size_t>(ti)];
            if (target == entt::null || !reg.valid(target)) continue;
            const XMMATRIX camW = ComputeWorldMatrix(reg, cam);
            const XMMATRIX tgtW = ComputeWorldMatrix(reg, target);
            XMFLOAT3 off(0.0f, 0.0f, 0.0f);
            if (const seq::SeqValue* o = focus->Find("offset"); o && o->IsArray() && o->arr.size() >= 3)
                off = XMFLOAT3(static_cast<float>(o->arr[0].n), static_cast<float>(o->arr[1].n), static_cast<float>(o->arr[2].n));
            const XMVECTOR wp = XMVector3TransformCoord(XMLoadFloat3(&off), tgtW);
            const XMVECTOR inCam = XMVector3TransformCoord(wp, XMMatrixInverse(nullptr, camW));
            float dist = XMVectorGetZ(inCam);
            if (!(dist > 0.0f)) dist = XMVectorGetX(XMVector3Length(XMVectorSubtract(wp, camW.r[3])));   // 背後なら直線距離
            const int fd = FindPostField("dofFocusDist"), on = FindPostField("dofOn");
            if (fd >= 0) out.post.push_back(PostWrite{ fd, 0, static_cast<double>(dist), true });
            if (on >= 0) out.post.push_back(PostWrite{ on, 0, 1.0, true });
        }
    }

    // ---- クリップ族(いまはシェイクだけ)---------------------------------------------------
    for (const seq::ClipSample& cl : res.clips)
    {
        const std::size_t bi = static_cast<std::size_t>(cl.binding), ti = static_cast<std::size_t>(cl.track), ki = static_cast<std::size_t>(cl.clip);
        if (bi >= seq.bindings.size() || ti >= seq.bindings[bi].tracks.size()) continue;
        const seq::Track& tr = seq.bindings[bi].tracks[ti];
        if (tr.type != seq::TrackType::Shake || ki >= tr.clips.size()) continue;
        const seq::Clip& c = tr.clips[ki];
        const entt::entity cam = bound.entity[bi];
        if (cam == entt::null || !reg.valid(cam)) continue;
        const double amp = c.params.GetNumber("amp", 0.3);
        const double freq = c.params.GetNumber("freq", 20.0);
        const double seed = c.params.GetNumber("seed", 0.0);
        const double decay = c.params.GetNumber("decay", 2.0);
        const double prog = c.dur > 0 ? static_cast<double>(cl.localTick) / static_cast<double>(c.dur) : 0.0;
        ShakeWrite sw;
        sw.entity = cam;
        ShakeOffset(tSec, amp, freq, seed, decay, prog, cl.weight, sw.x, sw.y, sw.z);
        out.shakes.push_back(sw);
    }
}

} // namespace seqhost
} // namespace dx12e
