#pragma once

// ============================================================================
// ゲーム AI のランタイム（Play 中だけ動く）
// ============================================================================
// 1 ステップ（1/60 秒の固定ステップ）の順番:
//   1. Brain の同期（Brain コンポーネントを持つエンティティに実行時の状態と群衆エージェントを用意）
//   2. ゲーム側のワープに気づく（Transform を直接書かれたら群衆の位置を置き直す）
//   3. 知覚: 視覚（視野 → 物理レイで遮蔽）/ 聴覚（ai.sound イベントを半径と遮蔽で）/ 記憶
//      → 黒板へ事実を書く（target.visible / target.seen / target.lastKnown / heard.* ...）
//   4. 思考: ユーティリティ評価で行動を選ぶ（ヒステリシス・最低継続・クールダウン）
//      → 切り替わったら Lua の exit / enter、毎ステップ update（done を返したらクールダウン）
//   5. 群衆（nav::NavCrowd）を 1 ステップ → Transform へ書き戻す・移動方向へ向ける
// ★決定論: エンティティ id の昇順で回す・乱数は Brain ごとのシード付き・固定ステップ。
//   ヘッドレスの step_frames deterministic（dt=1/60）では 1 フレーム = 1 ステップ。
// ★群衆に入れるエンティティは親を持たないこと（Transform をワールド座標として書く）。
//   CharacterController / 動的 RigidBody とも併用しない（物理が Transform を上書きする）。
// ============================================================================

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "core/Types.h"
#include "nav/NavCrowd.h"
#include "ai/AiRandom.h"
#include "ai/Blackboard.h"
#include "ai/Perception.h"
#include "ai/Utility.h"

namespace dx12e
{
class Scene;
class PhysicsSystem;
class EventBus;
struct Brain;

namespace ai
{

struct AgentOptions
{
    bool faceMovement = true;   // 移動方向へ Transform.rotation.y を回す
    f32  turnRate     = 10.0f;  // 向きの追従の速さ（1/秒。大きいほど速く向く）
    f32  yOffset      = 0.0f;   // ナビ面から Transform までの高さ
};

enum class ActionPhase : i32 { Enter = 0, Update = 1, Exit = 2 };
// Lua の行動を呼ぶ口（ScriptEngine が差し込む）。戻り値: 0=続ける / 1=done / -1=エラー / -2=関数なし
using ActionInvoker = std::function<i32(entt::entity, i32 actionIndex, ActionPhase, f32 dt)>;

// ai.sound の 1 件
struct SoundEvent
{
    f32 pos[3]{ 0, 0, 0 };
    f32 radius = 10.0f;
    f32 loudness = 1.0f;
    std::string tag;
    u32 source = 0xffffffffu;
    f64 time = 0.0;
};

// Brain 1 体ぶんの実行時の状態（コンポーネントには置かない）
struct BrainState
{
    Blackboard bb;
    std::vector<ActionDef> actions;
    std::vector<f64> cooldownUntil;
    i32  current = -1;
    bool currentDone = false;
    f64  enteredAt = 0.0;
    f64  nextThink = 0.0;
    f64  nextSight = 0.0;
    bool thinkNow = true;
    i32  forced = -1;
    Decision last;                 // 最後の評価（得点の内訳つき）
    u64  switches = 0;
    struct Switch { f64 time; std::string from, to; DecisionReason reason; };
    std::vector<Switch> history;   // 直近の切り替え（最大 16）

    std::vector<TargetMemory> targets;   // 知覚の対象ごとの記憶
    i32 primary = -1;                    // targets の添字（一番気にしている相手）
    std::vector<HeardSound> heard;       // 直近に聞いた音（最大 8）
    struct SightRay { f32 from[3]; f32 to[3]; bool blocked; bool candidate; u32 target; };
    std::vector<SightRay> rays;          // 最後の視線判定（デバッグ表示）

    Rng  rng;
    bool agent = false;       // 群衆エージェントを持っているか
    f32  speedOverride = 0.0f;  // brain:moveTo(pos, speed) / setSpeed の最高速度（0 = コンポーネントの値）
    f32  eye[3]{ 0, 0, 0 };   // 最後の目の位置（デバッグ表示）
    f32  yaw = 0.0f;
    std::string lastError;
    u32  errorCount = 0;
};

class AiSystem
{
public:
    static constexpr f32 kStep = 1.0f / 60.0f;

    AiSystem();
    ~AiSystem();
    AiSystem(const AiSystem&) = delete;
    AiSystem& operator=(const AiSystem&) = delete;

    // Play 開始・停止・シーン切替で呼ぶ（全エージェント・全 Brain を捨てる）
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

    // ---- Brain ----
    void SetActionInvoker(ActionInvoker inv) { m_invoke = std::move(inv); }
    // Brain コンポーネントを持つエンティティの実行時の状態を用意する（無ければ作る）
    BrainState* EnsureBrain(Scene& scene, entt::entity e);
    BrainState* GetBrain(entt::entity e);
    const BrainState* GetBrain(entt::entity e) const;
    // 行動の定義（同じ名前なら置き換え）。戻り値 = 行動の番号（-1 = Brain が無い）
    i32  DefineAction(entt::entity e, ActionDef def);
    void ClearActions(entt::entity e);
    bool ForceAction(entt::entity e, const std::string& name);
    void RequestThink(entt::entity e);
    // brain:moveTo の最高速度（0 でコンポーネントの maxSpeed に戻す）
    void SetBrainSpeed(entt::entity e, f32 speed);
    // 記憶を外から与える（ディレクターが「このあたりを探せ」と渡す）/ 忘れさせる
    bool Remember(entt::entity e, const f32 pos[3]);
    void Forget(entt::entity e);
    // エンティティ id の昇順の Brain 一覧
    std::vector<entt::entity> Brains() const;

    // ---- 音 ----
    // ai.sound を直接積む（EventBus の "ai.sound" も同じ所へ入る）
    void EmitSound(const SoundEvent& s);
    // イベント名 → 音 の橋渡し（アニメイベントの足音など。source エンティティの位置で鳴らす）
    void AddSoundBridge(const std::string& eventName, f32 radius, f32 loudness, const std::string& tag);

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
    struct SoundBridge { std::string name; f32 radius; f32 loudness; std::string tag; bool subscribed = false; };

    void Step(Scene& scene, PhysicsSystem* physics, f32 h);
    void SyncBrains(Scene& scene);
    void Perceive(Scene& scene, PhysicsSystem* physics, entt::entity e, const Brain& cfg, BrainState& st, f32 h);
    void Think(entt::entity e, const Brain& cfg, BrainState& st, f32 h);
    void Invoke(entt::entity e, BrainState& st, i32 idx, ActionPhase ph, f32 dt, i32* result = nullptr);
    void Subscribe(EventBus* bus);
    AgentLink* Link(entt::entity e);
    const AgentLink* Link(entt::entity e) const;

    nav::NavCrowd m_crowd;
    std::map<u32, AgentLink> m_agents;   // エンティティ id の昇順で回す（決定論）
    std::map<u32, BrainState> m_brains;
    const Scene* m_scene = nullptr;
    ActionInvoker m_invoke;

    std::vector<SoundEvent> m_sounds;    // 次の知覚で処理する音
    std::vector<SoundBridge> m_bridges;
    EventBus* m_bus = nullptr;
    bool m_soundSubscribed = false;

    f32 m_accum = 0.0f;
    u64 m_tick = 0;
    f64 m_time = 0.0;
};

} // namespace ai
} // namespace dx12e
