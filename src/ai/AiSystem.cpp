// ============================================================================
// ゲーム AI のランタイム。考え方は AiSystem.h の冒頭を参照。
// ============================================================================
#include "ai/AiSystem.h"

#include "ecs/Components.h"
#include "scene/Scene.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace dx12e
{
namespace ai
{

namespace
{
inline u32 Id(entt::entity e) { return static_cast<u32>(entt::to_integral(e)); }

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
} // namespace

AiSystem::AiSystem() = default;
AiSystem::~AiSystem() = default;

void AiSystem::Clear()
{
    m_crowd.Clear();
    m_agents.clear();
    m_scene = nullptr;
    m_accum = 0.0f;
    m_tick = 0;
    m_time = 0.0;
}

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
}

bool AiSystem::HasAgent(entt::entity e) const { return Link(e) != nullptr; }

bool AiSystem::MoveTo(entt::entity e, const f32 pos[3], f32 speed)
{
    const AgentLink* l = Link(e);
    if (!l) return false;
    if (speed > 0.0f)
    {
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

void AiSystem::Update(Scene& scene, PhysicsSystem* /*physics*/, EventBus* /*bus*/, f32 dt)
{
    if (m_scene != &scene)
    {
        // シーンが差し替わった（ランタイムのシーン切替）。前のシーンのエンティティは無効
        Clear();
        m_scene = &scene;
    }
    m_crowd.SetNavMesh(scene.HasNavMesh() ? &scene.GetNavMesh() : nullptr);
    if (dt <= 0.0f) return;

    // 固定ステップ。ヒッチで溜まった分は 4 ステップまでで捨てる（追いつこうとして重くならない）
    m_accum += dt;
    i32 steps = 0;
    while (m_accum >= kStep - 1e-6f && steps < 4)
    {
        m_accum -= kStep;
        if (m_accum < 0.0f) m_accum = 0.0f;
        Step(scene, kStep);
        ++steps;
    }
    if (steps == 4 && m_accum > kStep) m_accum = kStep;
}

void AiSystem::Step(Scene& scene, f32 h)
{
    auto& reg = scene.GetRegistry();

    // ---- 消えたエンティティのエージェントを捨てる ----
    for (auto it = m_agents.begin(); it != m_agents.end();)
    {
        const entt::entity e = static_cast<entt::entity>(it->first);
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

    // ---- ゲーム側のワープに気づく（前に書いた位置から外れていたら置き直す）----
    for (auto& [id, l] : m_agents)
    {
        if (!l.written) continue;
        const auto& t = reg.get<Transform>(static_cast<entt::entity>(id));
        const f32 dx = t.position.x - l.lastWritten[0];
        const f32 dy = t.position.y - l.lastWritten[1];
        const f32 dz = t.position.z - l.lastWritten[2];
        if (dx * dx + dy * dy + dz * dz > 0.05f * 0.05f)
        {
            const f32 p[3] = { t.position.x, t.position.y - l.opt.yOffset, t.position.z };
            m_crowd.Teleport(l.idx, p);
        }
    }

    m_crowd.Update(h);

    // ---- Transform へ書き戻す ----
    for (auto& [id, l] : m_agents)
    {
        const nav::NavCrowdAgent* ag = m_crowd.Agent(l.idx);
        if (!ag) continue;
        auto& t = reg.get<Transform>(static_cast<entt::entity>(id));
        const f32 nx = ag->npos[0], ny = ag->npos[1] + l.opt.yOffset, nz = ag->npos[2];
        if (l.written)
        {
            l.actualVel[0] = (nx - t.position.x) / h;
            l.actualVel[1] = (ny - t.position.y) / h;
            l.actualVel[2] = (nz - t.position.z) / h;
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
