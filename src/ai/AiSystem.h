#pragma once

// ============================================================================
// ゲーム AI のランタイム（Play 中だけ動く）
// ============================================================================
//   - 群衆（nav::NavCrowd）とエンティティの対応付け。Transform へ書き戻す・向きを移動方向へ回す
//   - 固定ステップ（1/60 秒）で回す。フレームの dt を貯めて刻むので、同じ入力なら同じ結果
//     （ヘッドレスの step_frames deterministic では 1 フレーム = 1 ステップ）
//
// ★ゲーム側がエンティティをワープさせた（Transform を直接書いた）ら、次のステップで気づいて
//   群衆の位置を置き直す（通路も張り直す）。
// ★群衆に入れるエンティティは親を持たないこと（Transform をワールド座標として書く）。
//   CharacterController / 動的 RigidBody とも併用しない（物理が Transform を上書きする）。
// ============================================================================

#include <map>
#include <vector>

#include <entt/entt.hpp>

#include "core/Types.h"
#include "nav/NavCrowd.h"

namespace dx12e
{
class Scene;
class PhysicsSystem;
class EventBus;

namespace ai
{

struct AgentOptions
{
    bool faceMovement = true;   // 移動方向へ Transform.rotation.y を回す
    f32  turnRate     = 10.0f;  // 向きの追従の速さ（1/秒。大きいほど速く向く）
    f32  yOffset      = 0.0f;   // ナビ面から Transform までの高さ
};

class AiSystem
{
public:
    static constexpr f32 kStep = 1.0f / 60.0f;

    AiSystem();
    ~AiSystem();
    AiSystem(const AiSystem&) = delete;
    AiSystem& operator=(const AiSystem&) = delete;

    // Play 開始・停止・シーン切替で呼ぶ（全エージェントを捨てる）
    void Clear();

    // 毎フレーム（Play 中のみ）。dt はスクリプトと同じ時間（タイムスケール適用済み）。
    void Update(Scene& scene, PhysicsSystem* physics, EventBus* bus, f32 dt);

    // ---- 群衆 ----
    bool AddAgent(Scene& scene, entt::entity e, const nav::NavAgentParams& p, const AgentOptions& o);
    void RemoveAgent(entt::entity e);
    bool HasAgent(entt::entity e) const;
    // speed > 0 なら最高速度をその値にする（歩く / 走るの切り替え）
    bool MoveTo(entt::entity e, const f32 pos[3], f32 speed = 0.0f);
    bool MoveVelocity(entt::entity e, const f32 vel[3]);
    bool Stop(entt::entity e);
    bool SetParams(entt::entity e, const nav::NavAgentParams& p);
    bool SetOptions(entt::entity e, const AgentOptions& o);
    const nav::NavCrowdAgent* GetAgent(entt::entity e) const;
    const AgentOptions* GetOptions(entt::entity e) const;
    // 実際に動いた速度（位置の差分 / 秒）。アニメの足の速さに使う
    bool ActualVelocity(entt::entity e, f32 out[3]) const;
    f32  DistanceToGoal(entt::entity e) const;
    bool Arrived(entt::entity e, f32 tol) const;

    const nav::NavCrowd& Crowd() const { return m_crowd; }
    u64  Tick() const { return m_tick; }
    f64  Time() const { return m_time; }

private:
    struct AgentLink
    {
        i32 idx = -1;
        AgentOptions opt;
        f32 lastWritten[3]{ 0, 0, 0 };
        f32 actualVel[3]{ 0, 0, 0 };
        bool written = false;
    };

    void Step(Scene& scene, f32 h);
    AgentLink* Link(entt::entity e);
    const AgentLink* Link(entt::entity e) const;

    nav::NavCrowd m_crowd;
    std::map<u32, AgentLink> m_agents;   // エンティティ id の昇順で回す（決定論）
    const Scene* m_scene = nullptr;
    f32 m_accum = 0.0f;
    u64 m_tick = 0;
    f64 m_time = 0.0;
};

} // namespace ai
} // namespace dx12e
