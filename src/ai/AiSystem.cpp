// ============================================================================
// ゲーム AI のランタイム。1 ステップの順番は AiSystem.h の冒頭を参照。
// ============================================================================
#include "ai/AiSystem.h"

#include "ecs/Components.h"
#include "scene/Scene.h"
#include "physics/PhysicsSystem.h"
#include "engine/core/EventBus.h"
#include "core/Logger.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace dx12e
{
namespace ai
{

namespace
{
constexpr u32 kNullId = 0xffffffffu;
constexpr f32 kAwarenessDecay = 0.5f;   // 見えていない間、awareness が 1 秒に下がる量
constexpr size_t kMaxHeard = 8;
constexpr size_t kMaxHistory = 16;

inline u32 Id(entt::entity e) { return static_cast<u32>(entt::to_integral(e)); }
inline entt::entity Ent(u32 id) { return static_cast<entt::entity>(id); }

// 角度の補間（度）。最短の向きで回る
f32 AngLerpDeg(f32 a, f32 b, f32 t)
{
    f32 d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
    return a + d * t;
}

void SetYaw(Transform& t, f32 yawDeg)
{
    t.rotation.y = std::fmod(yawDeg + 360.0f, 360.0f);
    if (t.useQuaternion)
    {
        using namespace DirectX;
        const XMVECTOR q = XMQuaternionRotationRollPitchYaw(XMConvertToRadians(t.rotation.x),
                                                            XMConvertToRadians(t.rotation.y),
                                                            XMConvertToRadians(t.rotation.z));
        XMStoreFloat4(&t.quaternion, q);
    }
}

std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// "MainCamera, tag:player" → 対象のエンティティ（id 昇順・重複なし）
std::vector<u32> ResolveTargets(entt::registry& reg, const std::string& spec, entt::entity self)
{
    std::vector<u32> out;
    size_t start = 0;
    while (start <= spec.size())
    {
        const size_t comma = spec.find(',', start);
        const std::string tok = Trim(spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!tok.empty())
        {
            if (tok.rfind("tag:", 0) == 0)
            {
                const std::string tag = Trim(tok.substr(4));
                for (auto [e, tg] : reg.view<Tag>().each())
                    if (e != self && std::find(tg.tags.begin(), tg.tags.end(), tag) != tg.tags.end())
                        out.push_back(Id(e));
            }
            else
            {
                u32 best = kNullId;
                for (auto [e, nt] : reg.view<NameTag>().each())
                    if (nt.name == tok && e != self && Id(e) < best) best = Id(e);
                if (best != kNullId) out.push_back(best);
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

u32 BodyOf(entt::registry& reg, entt::entity e)
{
    if (reg.valid(e))
        if (const RigidBody* rb = reg.try_get<RigidBody>(e)) return rb->bodyId;
    return 0xFFFFFFFFu;
}

// from → to の視線が通るか。self / target 自身の体に当たったら、その先から撃ち直す。
bool LineOfSight(PhysicsSystem* physics, entt::registry& reg, const f32 from[3], const f32 to[3],
                 entt::entity self, entt::entity target)
{
    if (!physics) return true;
    f32 d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    const f32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-3f) return true;
    d[0] /= len; d[1] /= len; d[2] /= len;
    const u32 ignore = BodyOf(reg, target);
    f32 o[3] = { from[0], from[1], from[2] };
    f32 remaining = len;
    for (int it = 0; it < 4 && remaining > 0.05f; ++it)
    {
        const RaycastHit h = physics->Raycast({ o[0], o[1], o[2] }, { d[0], d[1], d[2] }, remaining, ignore);
        if (!h.hit) return true;
        if (h.distance >= remaining - 0.3f) return true;   // 対象のすぐ手前（床や対象の持ち物）
        const entt::entity he = physics->EntityForBody(h.bodyId);
        if (he == self || he == target)
        {
            const f32 adv = h.distance + 0.05f;
            o[0] += d[0] * adv; o[1] += d[1] * adv; o[2] += d[2] * adv;
            remaining -= adv;
            continue;
        }
        return false;
    }
    return true;
}
} // namespace

AiSystem::AiSystem() = default;
AiSystem::~AiSystem() = default;

void AiSystem::Clear()
{
    m_crowd.Clear();
    m_agents.clear();
    m_brains.clear();
    m_sounds.clear();
    m_bridges.clear();
    m_bus = nullptr;
    m_soundSubscribed = false;
    m_scene = nullptr;
    m_accum = 0.0f;
    m_tick = 0;
    m_time = 0.0;
}

// ===========================================================================
// 群衆
// ===========================================================================
AiSystem::AgentLink* AiSystem::Link(entt::entity e)
{
    auto it = m_agents.find(Id(e));
    return it == m_agents.end() ? nullptr : &it->second;
}

const AiSystem::AgentLink* AiSystem::Link(entt::entity e) const
{
    auto it = m_agents.find(Id(e));
    return it == m_agents.end() ? nullptr : &it->second;
}

bool AiSystem::AddAgent(Scene& scene, entt::entity e, const nav::NavAgentParams& p, const AgentOptions& o)
{
    auto& reg = scene.GetRegistry();
    if (!reg.valid(e) || !reg.all_of<Transform>(e) || !scene.HasNavMesh()) return false;
    if (m_scene != &scene)
    {
        Clear();
        m_scene = &scene;
    }
    m_crowd.SetNavMesh(&scene.GetNavMesh());
    if (AgentLink* l = Link(e))
    {
        m_crowd.SetParams(l->idx, p);
        l->opt = o;
        return true;
    }
    const auto& t = reg.get<Transform>(e);
    const f32 pos[3] = { t.position.x, t.position.y - o.yOffset, t.position.z };
    const i32 idx = m_crowd.AddAgent(pos, p);
    if (idx < 0) return false;
    AgentLink l;
    l.idx = idx;
    l.opt = o;
    m_agents[Id(e)] = l;
    return true;
}

void AiSystem::RemoveAgent(entt::entity e)
{
    auto it = m_agents.find(Id(e));
    if (it == m_agents.end()) return;
    m_crowd.RemoveAgent(it->second.idx);
    m_agents.erase(it);
    if (BrainState* b = GetBrain(e)) b->agent = false;
}

bool AiSystem::HasAgent(entt::entity e) const { return Link(e) != nullptr; }

bool AiSystem::MoveTo(entt::entity e, const f32 pos[3], f32 speed)
{
    const AgentLink* l = Link(e);
    if (!l) return false;
    if (speed > 0.0f)
    {
        if (BrainState* b = GetBrain(e)) b->speedOverride = speed;
        if (const nav::NavCrowdAgent* ag = m_crowd.Agent(l->idx); ag && ag->params.maxSpeed != speed)
        {
            nav::NavAgentParams p = ag->params;
            p.maxSpeed = speed;
            m_crowd.SetParams(l->idx, p);
        }
    }
    return m_crowd.RequestMoveTarget(l->idx, pos);
}

bool AiSystem::MoveVelocity(entt::entity e, const f32 vel[3])
{
    const AgentLink* l = Link(e);
    return l && m_crowd.RequestMoveVelocity(l->idx, vel);
}

bool AiSystem::Stop(entt::entity e)
{
    const AgentLink* l = Link(e);
    return l && m_crowd.ResetMoveTarget(l->idx);
}

bool AiSystem::SetParams(entt::entity e, const nav::NavAgentParams& p)
{
    const AgentLink* l = Link(e);
    if (!l) return false;
    m_crowd.SetParams(l->idx, p);
    return true;
}

bool AiSystem::SetOptions(entt::entity e, const AgentOptions& o)
{
    AgentLink* l = Link(e);
    if (!l) return false;
    l->opt = o;
    return true;
}

const nav::NavCrowdAgent* AiSystem::GetAgent(entt::entity e) const
{
    const AgentLink* l = Link(e);
    return l ? m_crowd.Agent(l->idx) : nullptr;
}

const AgentOptions* AiSystem::GetOptions(entt::entity e) const
{
    const AgentLink* l = Link(e);
    return l ? &l->opt : nullptr;
}

bool AiSystem::ActualVelocity(entt::entity e, f32 out[3]) const
{
    const AgentLink* l = Link(e);
    if (!l) return false;
    out[0] = l->actualVel[0]; out[1] = l->actualVel[1]; out[2] = l->actualVel[2];
    return true;
}

f32 AiSystem::DistanceToGoal(entt::entity e) const
{
    const AgentLink* l = Link(e);
    return l ? m_crowd.DistanceToGoal(l->idx) : 0.0f;
}

bool AiSystem::Arrived(entt::entity e, f32 tol) const
{
    const AgentLink* l = Link(e);
    return l && m_crowd.Arrived(l->idx, tol);
}

// ===========================================================================
// Brain
// ===========================================================================
BrainState* AiSystem::GetBrain(entt::entity e)
{
    auto it = m_brains.find(Id(e));
    return it == m_brains.end() ? nullptr : &it->second;
}

const BrainState* AiSystem::GetBrain(entt::entity e) const
{
    auto it = m_brains.find(Id(e));
    return it == m_brains.end() ? nullptr : &it->second;
}

BrainState* AiSystem::EnsureBrain(Scene& scene, entt::entity e)
{
    auto& reg = scene.GetRegistry();
    if (!reg.valid(e) || !reg.all_of<Brain>(e)) return nullptr;
    if (m_scene != &scene)
    {
        Clear();
        m_scene = &scene;
    }
    auto it = m_brains.find(Id(e));
    if (it != m_brains.end()) return &it->second;
    BrainState& st = m_brains[Id(e)];
    st.rng.Seed(static_cast<u64>(static_cast<u32>(reg.get<Brain>(e).seed)));
    st.nextThink = m_time;
    st.nextSight = m_time;
    return &st;
}

std::vector<entt::entity> AiSystem::Brains() const
{
    std::vector<entt::entity> out;
    out.reserve(m_brains.size());
    for (const auto& [id, st] : m_brains) out.push_back(Ent(id));
    return out;
}

i32 AiSystem::DefineAction(entt::entity e, ActionDef def)
{
    BrainState* st = GetBrain(e);
    if (!st) return -1;
    for (size_t i = 0; i < st->actions.size(); ++i)
    {
        if (st->actions[i].name == def.name)
        {
            st->actions[i] = std::move(def);
            st->thinkNow = true;
            return static_cast<i32>(i);
        }
    }
    st->actions.push_back(std::move(def));
    st->cooldownUntil.resize(st->actions.size(), -1e9);
    st->thinkNow = true;
    return static_cast<i32>(st->actions.size() - 1);
}

void AiSystem::ClearActions(entt::entity e)
{
    BrainState* st = GetBrain(e);
    if (!st) return;
    st->actions.clear();
    st->cooldownUntil.clear();
    st->current = -1;
    st->currentDone = false;
    st->thinkNow = true;
}

bool AiSystem::ForceAction(entt::entity e, const std::string& name)
{
    BrainState* st = GetBrain(e);
    if (!st) return false;
    for (size_t i = 0; i < st->actions.size(); ++i)
        if (st->actions[i].name == name) { st->forced = static_cast<i32>(i); return true; }
    return false;
}

void AiSystem::RequestThink(entt::entity e)
{
    if (BrainState* st = GetBrain(e)) st->thinkNow = true;
}

void AiSystem::SetBrainSpeed(entt::entity e, f32 speed)
{
    if (BrainState* st = GetBrain(e)) st->speedOverride = (std::max)(0.0f, speed);
}

bool AiSystem::Remember(entt::entity e, const f32 pos[3])
{
    BrainState* st = GetBrain(e);
    if (!st) return false;
    if (st->targets.empty()) st->targets.push_back(TargetMemory{});
    TargetMemory& m = st->targets[static_cast<size_t>(st->primary >= 0 ? st->primary : 0)];
    RememberHeard(m, pos, m_time);
    return true;
}

void AiSystem::Forget(entt::entity e)
{
    BrainState* st = GetBrain(e);
    if (!st) return;
    for (TargetMemory& m : st->targets)
    {
        m.known = false;
        m.awareness = 0.0f;
        m.lastSeen = m.lastHeard = m.lastStimulus = -1e9;
    }
    st->heard.clear();
}

// ===========================================================================
// 音
// ===========================================================================
void AiSystem::EmitSound(const SoundEvent& s)
{
    SoundEvent c = s;
    c.time = m_time;
    m_sounds.push_back(std::move(c));
}

void AiSystem::AddSoundBridge(const std::string& eventName, f32 radius, f32 loudness, const std::string& tag)
{
    for (SoundBridge& b : m_bridges)
        if (b.name == eventName) { b.radius = radius; b.loudness = loudness; b.tag = tag; return; }
    m_bridges.push_back({ eventName, radius, loudness, tag, false });
}

void AiSystem::Subscribe(EventBus* bus)
{
    if (!bus) return;
    if (bus != m_bus)
    {
        m_bus = bus;
        m_soundSubscribed = false;
        for (SoundBridge& b : m_bridges) b.subscribed = false;
    }
    if (!m_soundSubscribed)
    {
        // ★購読は Play 開始の EventBus::Clear より後（最初の Update）で張る
        bus->On("ai.sound", [this](const EngineEvent& ev)
        {
            SoundEvent s;
            s.pos[0] = static_cast<f32>(ev.num("x"));
            s.pos[1] = static_cast<f32>(ev.num("y"));
            s.pos[2] = static_cast<f32>(ev.num("z"));
            s.radius = static_cast<f32>(ev.num("radius", 10.0));
            s.loudness = static_cast<f32>(ev.num("loudness", 1.0));
            s.tag = ev.str("tag");
            if (ev.source != entt::null) s.source = Id(ev.source);
            else if (const f64 src = ev.num("source", -1.0); src >= 0.0) s.source = static_cast<u32>(src);
            EmitSound(s);
        });
        m_soundSubscribed = true;
    }
    for (SoundBridge& b : m_bridges)
    {
        if (b.subscribed) continue;
        const std::string name = b.name;
        bus->On(name, [this, name](const EngineEvent& ev)
        {
            if (ev.source == entt::null || !m_scene) return;
            const auto& reg = m_scene->GetRegistry();
            if (!reg.valid(ev.source) || !reg.all_of<Transform>(ev.source)) return;
            for (const SoundBridge& br : m_bridges)
            {
                if (br.name != name) continue;
                const auto& t = reg.get<Transform>(ev.source);
                SoundEvent s;
                s.pos[0] = t.position.x; s.pos[1] = t.position.y; s.pos[2] = t.position.z;
                s.radius = br.radius;
                s.loudness = br.loudness;
                s.tag = br.tag.empty() ? name : br.tag;
                s.source = Id(ev.source);
                EmitSound(s);
                break;
            }
        });
        b.subscribed = true;
    }
}

// ===========================================================================
// 更新
// ===========================================================================
void AiSystem::Update(Scene& scene, PhysicsSystem* physics, EventBus* bus, f32 dt)
{
    if (m_scene != &scene)
    {
        // シーンが差し替わった（ランタイムのシーン切替）。前のシーンのエンティティは無効
        Clear();
        m_scene = &scene;
    }
    Subscribe(bus);
    m_crowd.SetNavMesh(scene.HasNavMesh() ? &scene.GetNavMesh() : nullptr);
    if (dt <= 0.0f) return;

    // 固定ステップ。ヒッチで溜まった分は 4 ステップまでで捨てる（追いつこうとして重くならない）
    m_accum += dt;
    i32 steps = 0;
    while (m_accum >= kStep - 1e-6f && steps < 4)
    {
        m_accum -= kStep;
        if (m_accum < 0.0f) m_accum = 0.0f;
        Step(scene, physics, kStep);
        ++steps;
    }
    if (steps == 4 && m_accum > kStep) m_accum = kStep;
}

void AiSystem::SyncBrains(Scene& scene)
{
    auto& reg = scene.GetRegistry();
    // 消えた / Brain を外された / 無効化された ものの状態を捨てる
    for (auto it = m_brains.begin(); it != m_brains.end();)
    {
        const entt::entity e = Ent(it->first);
        if (!reg.valid(e) || !reg.all_of<Brain, Transform>(e))
        {
            if (it->second.agent) { auto li = m_agents.find(it->first); if (li != m_agents.end()) { m_crowd.RemoveAgent(li->second.idx); m_agents.erase(li); } }
            it = m_brains.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // Brain を持つエンティティ（id 昇順）に状態と群衆エージェントを用意する
    std::vector<u32> ids;
    for (auto [e, br, t] : reg.view<Brain, Transform>().each())
        if (br.enabled) ids.push_back(Id(e));
    std::sort(ids.begin(), ids.end());
    for (const u32 id : ids)
    {
        const entt::entity e = Ent(id);
        const Brain& cfg = reg.get<Brain>(e);
        BrainState* st = EnsureBrain(scene, e);
        if (!st) continue;
        if (cfg.useCrowd && scene.HasNavMesh())
        {
            nav::NavAgentParams p;
            if (const nav::NavCrowdAgent* cur = GetAgent(e)) p = cur->params;
            p.radius = cfg.agentRadius;
            p.maxSpeed = st->speedOverride > 0.0f ? st->speedOverride : cfg.maxSpeed;
            p.maxAccel = cfg.maxAccel;
            p.separationWeight = cfg.separation;
            p.wallMargin = cfg.wallMargin;
            AgentOptions o;
            if (const AgentOptions* co = GetOptions(e)) o = *co;
            o.faceMovement = cfg.turnRate > 0.0f;
            o.turnRate = cfg.turnRate;
            if (!st->agent) st->agent = AddAgent(scene, e, p, o);
            else { SetParams(e, p); SetOptions(e, o); }
        }
        else if (st->agent)
        {
            RemoveAgent(e);
            st->agent = false;
        }
    }
}

void AiSystem::Perceive(Scene& scene, PhysicsSystem* physics, entt::entity self, const Brain& cfg,
                        BrainState& st, f32 h)
{
    auto& reg = scene.GetRegistry();
    const Transform& t = reg.get<Transform>(self);
    const f32 eye[3] = { t.position.x, t.position.y + cfg.eyeHeight, t.position.z };
    st.eye[0] = eye[0]; st.eye[1] = eye[1]; st.eye[2] = eye[2];
    st.yaw = t.rotation.y;
    const bool sightTick = m_time + 1e-9 >= st.nextSight;
    if (sightTick) st.nextSight = m_time + (std::max)(static_cast<f64>(cfg.sightInterval), static_cast<f64>(h));

    // ---- 対象の一覧（視線の間隔で探し直す。消えた相手の記憶は捨てる）----
    if (sightTick)
    {
        const std::vector<u32> ids = ResolveTargets(reg, cfg.targets, self);
        std::vector<TargetMemory> next;
        next.reserve(ids.size());
        for (const u32 id : ids)
        {
            auto it = std::find_if(st.targets.begin(), st.targets.end(),
                                   [&](const TargetMemory& m) { return m.entity == id; });
            if (it != st.targets.end()) next.push_back(*it);
            else { TargetMemory m; m.entity = id; next.push_back(m); }
        }
        // 外から与えた記憶（entity 無し）は残す
        for (const TargetMemory& m : st.targets)
            if (m.entity == kNullId && m.known) next.push_back(m);
        st.targets.swap(next);
        st.rays.clear();
    }

    // ---- 視覚 ----
    for (TargetMemory& m : st.targets)
    {
        if (m.entity == kNullId) continue;
        const entt::entity te = Ent(m.entity);
        if (!reg.valid(te) || !reg.all_of<Transform>(te)) { m.visible = false; m.seen = false; continue; }
        const Transform& tt = reg.get<Transform>(te);
        const f32 aim[3] = { tt.position.x, tt.position.y + cfg.targetHeight, tt.position.z };
        const f32 ground[3] = { tt.position.x, tt.position.y, tt.position.z };
        const ViewCheck vc = CheckView(eye, t.rotation.y, aim, cfg.sightRange, cfg.sightFov, cfg.nearSense);
        m.dist = vc.dist;
        if (sightTick)
        {
            bool visible = false;
            if (vc.Candidate()) visible = LineOfSight(physics, reg, eye, aim, self, te);
            m.visible = visible;
            BrainState::SightRay ray{};
            ray.from[0] = eye[0]; ray.from[1] = eye[1]; ray.from[2] = eye[2];
            ray.to[0] = aim[0]; ray.to[1] = aim[1]; ray.to[2] = aim[2];
            ray.blocked = !visible;
            ray.candidate = vc.Candidate();
            ray.target = m.entity;
            st.rays.push_back(ray);
        }
        // 近く（気配の距離）はすぐに気づく＝確認時間を 1/4 にする
        const f32 confirm = (vc.isNear ? cfg.confirmTime * 0.25f : cfg.confirmTime);
        m.awareness = StepAwareness(m.awareness, m.visible, h, confirm, kAwarenessDecay);
        m.seen = m.visible && m.awareness >= 1.0f;
        if (m.seen) RememberSeen(m, ground, m_time);
    }

    // ---- 聴覚（ai.sound。半径 × 耳の良さ、壁越しなら occlusion 倍）----
    for (const SoundEvent& s : m_sounds)
    {
        if (s.source == Id(self)) continue;
        const f32 dx = s.pos[0] - eye[0], dy = s.pos[1] - eye[1], dz = s.pos[2] - eye[2];
        const f32 d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const f32 reachOpen = HearingReach(s.radius * s.loudness, cfg.hearingScale, false, cfg.occlusion);
        if (d > reachOpen) continue;
        const f32 from[3] = { s.pos[0], s.pos[1] + 0.2f, s.pos[2] };
        const entt::entity srcE = (s.source != kNullId) ? Ent(s.source) : entt::null;
        const bool occluded = !LineOfSight(physics, reg, from, eye, self, srcE);
        const f32 reach = HearingReach(s.radius * s.loudness, cfg.hearingScale, occluded, cfg.occlusion);
        f32 loud = 0.0f;
        if (!Hears(d, reach, &loud)) continue;
        HeardSound hs;
        hs.pos[0] = s.pos[0]; hs.pos[1] = s.pos[1]; hs.pos[2] = s.pos[2];
        hs.radius = s.radius;
        hs.loudness = loud;
        hs.time = s.time;
        hs.source = s.source;
        hs.occluded = occluded;
        hs.tag = s.tag;
        st.heard.push_back(hs);
        if (st.heard.size() > kMaxHeard) st.heard.erase(st.heard.begin());
        // 対象が出した音なら、その相手の居場所として覚える
        for (TargetMemory& m : st.targets)
            if (m.entity != kNullId && m.entity == s.source) RememberHeard(m, s.pos, m_time);
    }

    // ---- 忘れる ----
    for (TargetMemory& m : st.targets) ForgetIfStale(m, m_time, cfg.memoryTime);

    // ---- 一番気にしている相手（見つけている > 気づき度 > 覚えている > 近い > id）----
    st.primary = -1;
    f32 bestKey = -1.0f;
    for (size_t i = 0; i < st.targets.size(); ++i)
    {
        const TargetMemory& m = st.targets[i];
        const f32 key = (m.seen ? 4.0f : 0.0f) + m.awareness * 2.0f + (m.known ? 1.0f : 0.0f) -
                        (std::min)(m.dist, 1000.0f) * 1e-4f;
        if (key > bestKey) { bestKey = key; st.primary = static_cast<i32>(i); }
    }

    // ---- 黒板へ事実を書く ----
    Blackboard& bb = st.bb;
    bb.SetNumber("time", m_time);
    if (st.primary >= 0)
    {
        const TargetMemory& m = st.targets[static_cast<size_t>(st.primary)];
        if (m.entity != kNullId) bb.SetEntity("target", m.entity); else bb.Erase("target");
        bb.SetBool("target.visible", m.visible);
        bb.SetBool("target.seen", m.seen);
        bb.SetNumber("target.awareness", m.awareness);
        bb.SetNumber("target.distance", m.entity != kNullId ? m.dist : 1e9);
        bb.SetBool("target.known", m.known);
        if (m.known) bb.SetVec3("target.lastKnown", m.knownPos[0], m.knownPos[1], m.knownPos[2]);
        else bb.Erase("target.lastKnown");
        bb.SetNumber("target.lastSeenAge", m.lastSeen > -1e8 ? m_time - m.lastSeen : 1e9);
        bb.SetNumber("target.lastHeardAge", m.lastHeard > -1e8 ? m_time - m.lastHeard : 1e9);
    }
    else
    {
        bb.Erase("target");
        bb.SetBool("target.visible", false);
        bb.SetBool("target.seen", false);
        bb.SetNumber("target.awareness", 0.0);
        bb.SetNumber("target.distance", 1e9);
        bb.SetBool("target.known", false);
        bb.Erase("target.lastKnown");
        bb.SetNumber("target.lastSeenAge", 1e9);
        bb.SetNumber("target.lastHeardAge", 1e9);
    }
    if (!st.heard.empty())
    {
        const HeardSound& hs = st.heard.back();
        bb.SetNumber("heard.age", m_time - hs.time);
        bb.SetVec3("heard.pos", hs.pos[0], hs.pos[1], hs.pos[2]);
        bb.SetNumber("heard.loudness", hs.loudness);
        bb.SetString("heard.tag", hs.tag);
    }
    else
    {
        bb.SetNumber("heard.age", 1e9);
        bb.Erase("heard.pos");
        bb.SetNumber("heard.loudness", 0.0);
        bb.Erase("heard.tag");
    }
    f32 av[3]{};
    ActualVelocity(self, av);
    bb.SetNumber("self.speed", std::sqrt(av[0] * av[0] + av[2] * av[2]));
}

void AiSystem::Invoke(entt::entity e, BrainState& st, i32 idx, ActionPhase ph, f32 dt, i32* result)
{
    if (result) *result = 0;
    if (!m_invoke || idx < 0) return;
    const i32 r = m_invoke(e, idx, ph, dt);
    if (result) *result = r;
    if (r == -1)
    {
        ++st.errorCount;
        st.lastError = "行動 '" + (idx < static_cast<i32>(st.actions.size()) ? st.actions[static_cast<size_t>(idx)].name
                                                                          : std::string("?")) +
                       "' の Lua がエラー（ログを参照）";
    }
}

void AiSystem::Think(entt::entity e, const Brain& cfg, BrainState& st, f32 h)
{
    if (st.actions.empty()) return;
    const bool doThink = st.thinkNow || st.forced >= 0 || m_time + 1e-9 >= st.nextThink;
    if (doThink)
    {
        st.nextThink = m_time + (std::max)(static_cast<f64>(cfg.thinkInterval), static_cast<f64>(h));
        st.thinkNow = false;
        SelectInput in;
        in.current = st.current;
        in.currentDone = st.currentDone;
        in.now = m_time;
        in.enteredAt = st.enteredAt;
        in.hysteresis = cfg.hysteresis;
        in.minCommit = cfg.minCommitTime;
        in.cooldownUntil = &st.cooldownUntil;
        Decision d = SelectAction(st.actions, st.bb, in);
        if (st.forced >= 0 && st.forced < static_cast<i32>(st.actions.size()))
        {
            d.chosen = st.forced;
            d.reason = DecisionReason::Forced;
        }
        st.forced = -1;
        const bool restart = (d.chosen == st.current && st.currentDone && d.chosen >= 0);
        if (d.chosen != st.current || restart)
        {
            const i32 prev = st.current;
            if (prev >= 0) Invoke(e, st, prev, ActionPhase::Exit, 0.0f);
            st.current = d.chosen;
            st.enteredAt = m_time;
            st.currentDone = false;
            ++st.switches;
            BrainState::Switch sw;
            sw.time = m_time;
            sw.from = prev >= 0 ? st.actions[static_cast<size_t>(prev)].name : std::string();
            sw.to = d.chosen >= 0 ? st.actions[static_cast<size_t>(d.chosen)].name : std::string();
            sw.reason = d.reason;
            st.history.push_back(sw);
            if (st.history.size() > kMaxHistory) st.history.erase(st.history.begin());
            if (st.current >= 0) Invoke(e, st, st.current, ActionPhase::Enter, 0.0f);
        }
        st.last = std::move(d);
    }
    if (st.current >= 0 && !st.currentDone)
    {
        i32 r = 0;
        Invoke(e, st, st.current, ActionPhase::Update, h, &r);
        if (r == 1)
        {
            st.currentDone = true;
            if (static_cast<size_t>(st.current) < st.cooldownUntil.size())
                st.cooldownUntil[static_cast<size_t>(st.current)] =
                    m_time + static_cast<f64>(st.actions[static_cast<size_t>(st.current)].cooldown);
            st.thinkNow = true;
        }
    }
    if (st.current >= 0) st.bb.SetString("action", st.actions[static_cast<size_t>(st.current)].name);
    else st.bb.Erase("action");
}

void AiSystem::Step(Scene& scene, PhysicsSystem* physics, f32 h)
{
    auto& reg = scene.GetRegistry();

    // ---- 1. Brain の同期 ----
    SyncBrains(scene);

    // ---- 消えたエンティティのエージェントを捨てる ----
    for (auto it = m_agents.begin(); it != m_agents.end();)
    {
        const entt::entity e = Ent(it->first);
        if (!reg.valid(e) || !reg.all_of<Transform>(e))
        {
            m_crowd.RemoveAgent(it->second.idx);
            it = m_agents.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // ---- 2. ゲーム側のワープに気づく（前に書いた位置から外れていたら置き直す）----
    // ★思考（Lua の行動）の後にももう一度見る。行動の中でワープさせた（捕獲して遠くへ戻す等）のを
    //   同じステップの書き戻しで上書きしていた（捕まえたのにミクがその場に残った）。
    auto detectWarps = [&]()
    {
        for (auto& [id, l] : m_agents)
        {
            if (!l.written) continue;
            const auto& t = reg.get<Transform>(Ent(id));
            const f32 dx = t.position.x - l.lastWritten[0];
            const f32 dy = t.position.y - l.lastWritten[1];
            const f32 dz = t.position.z - l.lastWritten[2];
            if (dx * dx + dy * dy + dz * dz > 0.05f * 0.05f)
            {
                const f32 p[3] = { t.position.x, t.position.y - l.opt.yOffset, t.position.z };
                m_crowd.Teleport(l.idx, p);
                l.written = false;   // 次の書き戻しでは速度を測らない（ワープの距離を速さにしない）
            }
        }
    };
    detectWarps();

    // ---- 3〜4. 知覚 → 思考（id 昇順）----
    for (auto& [id, st] : m_brains)
    {
        const entt::entity e = Ent(id);
        if (!reg.valid(e) || !reg.all_of<Brain, Transform>(e)) continue;
        const Brain& cfg = reg.get<Brain>(e);
        if (!cfg.enabled) continue;
        Perceive(scene, physics, e, cfg, st, h);
    }
    m_sounds.clear();   // 知覚で使い切った（行動の中で鳴った音は次のステップで聞く）
    // ★行動の Lua が別の Brain を作ったり消したりしても壊れないよう、id の写しで回す
    std::vector<u32> ids;
    ids.reserve(m_brains.size());
    for (const auto& [id, st] : m_brains) ids.push_back(id);
    for (const u32 id : ids)
    {
        auto it = m_brains.find(id);
        if (it == m_brains.end()) continue;
        const entt::entity e = Ent(id);
        if (!reg.valid(e) || !reg.all_of<Brain, Transform>(e)) continue;
        const Brain cfg = reg.get<Brain>(e);   // 行動の中で config を書き換えられても安全なように写す
        if (!cfg.enabled) continue;
        Think(e, cfg, it->second, h);
    }

    // ---- 5. 群衆（行動の中のワープを拾ってから）----
    detectWarps();
    m_crowd.Update(h);

    // ---- Transform へ書き戻す ----
    for (auto& [id, l] : m_agents)
    {
        const nav::NavCrowdAgent* ag = m_crowd.Agent(l.idx);
        const entt::entity e = Ent(id);
        if (!ag || !reg.valid(e) || !reg.all_of<Transform>(e)) continue;
        auto& t = reg.get<Transform>(e);
        const f32 nx = ag->npos[0], ny = ag->npos[1] + l.opt.yOffset, nz = ag->npos[2];
        if (l.written)
        {
            l.actualVel[0] = (nx - t.position.x) / h;
            l.actualVel[1] = (ny - t.position.y) / h;
            l.actualVel[2] = (nz - t.position.z) / h;
        }
        else
        {
            l.actualVel[0] = l.actualVel[1] = l.actualVel[2] = 0.0f;
        }
        t.position = { nx, ny, nz };
        l.lastWritten[0] = nx; l.lastWritten[1] = ny; l.lastWritten[2] = nz;
        l.written = true;

        if (l.opt.faceMovement)
        {
            const f32 vx = ag->vel[0], vz = ag->vel[2];
            if (vx * vx + vz * vz > 0.01f)
            {
                const f32 want = DirectX::XMConvertToDegrees(std::atan2(vx, vz));
                SetYaw(t, AngLerpDeg(t.rotation.y, want, (std::min)(1.0f, l.opt.turnRate * h)));
            }
        }
    }
    ++m_tick;
    m_time += h;
}

} // namespace ai
} // namespace dx12e
