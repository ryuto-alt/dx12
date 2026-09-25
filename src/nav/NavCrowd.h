#pragma once

// ============================================================================
// 群衆（クラウド）— 複数エージェントの局所回避。Detour の dtCrowd と同じ組み立て
// ============================================================================
//   1 ステップ = 通路の検証 → 経路要求の処理 → 近傍 → 角 → 操舵（先読み / 減速 / 分離 / 壁の余白）
//              → 速度サンプリングによる回避（ORCA ではなく Detour の adaptive sampling）
//              → 加速度制限つき積分 → 重なりの押し出し（4 反復）→ 通路の上へ滑らせて確定
//   ★位置の確定は必ず通路の MovePosition を通す＝押し出されてもナビメッシュの外へは出ない。
//   ★乱数を使わず、エージェント番号の昇順で処理する＝同じ入力なら同じ結果（決定論）。
//
// エンジン（ECS）とは切り離した純ロジック。エンティティとの対応付けは ai::AiSystem が持つ。
// ============================================================================

#include <vector>

#include "nav/NavCorridor.h"

namespace dx12e
{
namespace nav
{

struct NavAgentParams
{
    f32 radius           = 0.5f;   // 体の半径（群衆どうしの間合い。ナビを削った半径とは別）
    f32 height           = 2.0f;   // 上下にこれ以上離れた相手は無視する（多層）
    f32 maxSpeed         = 3.5f;   // 最高速度 m/s（moveTo の speed 指定で上書きされる）
    f32 maxAccel         = 20.0f;  // 加速度 m/s^2（急な方向転換で壁に刺さらないように）
    f32 separationWeight = 2.0f;   // 分離の強さ（0 で切る）
    f32 collisionQueryRange = 0.0f;   // 近傍・壁を拾う距離。0 = radius * 12
    f32 pathOptimizationRange = 0.0f; // 見通しで近道する距離。0 = radius * 30
    f32 wallMargin       = 0.0f;   // 壁からさらに離す距離（角の押し出し + 壁への速度成分を削る）
    f32 slowDownRadius   = 0.0f;   // 終点の手前で減速を始める距離。0 = radius * 2
    i32 avoidanceQuality = 2;      // 0..3（Detour の low / medium / good / high）
    bool anticipateTurns    = true;
    bool obstacleAvoidance  = true;
    bool separation         = true;
    bool optimizeVisibility = true;
};

enum class NavMoveState : u8
{
    None     = 0,   // 目標なし（止まる）
    Pending  = 1,   // 経路の要求を受けた（次の Update で張る）
    Valid    = 2,   // 通路あり
    Failed   = 3,   // 経路が張れなかった
    Velocity = 4,   // 速度指定で動く（通路を使わない）
};
const char* NavMoveStateName(NavMoveState s);

struct NavCrowdAgent
{
    bool active = false;
    NavAgentParams params;
    NavCorridor corridor;

    f32 npos[3]{ 0, 0, 0 };   // 現在地（ナビ面の上）
    f32 vel[3]{ 0, 0, 0 };    // 実際の速度（積分に使う）
    f32 dvel[3]{ 0, 0, 0 };   // 望む速度（操舵の結果）
    f32 nvel[3]{ 0, 0, 0 };   // 回避後の速度
    f32 disp[3]{ 0, 0, 0 };
    f32 desiredSpeed = 0.0f;

    NavMoveState targetState = NavMoveState::None;
    f32 targetPos[3]{ 0, 0, 0 };   // Velocity のときは速度ベクトル
    f32 sincePlan = 0.0f;          // 最後に A* を張ってからの秒
    bool partial = false;

    // 近傍（距離の近い順、最大 kMaxNeighbours）
    struct Nei { i32 idx; f32 dist; };
    std::vector<Nei> neis;
    // 角（最大 kMaxCorners）と、周りの壁（6 float ずつ。p→q の左が通路側）
    std::vector<f32> corners;
    std::vector<u8>  cornerFlags;
    std::vector<f32> walls;
};

class NavCrowd
{
public:
    static constexpr i32 kMaxNeighbours = 6;
    static constexpr i32 kMaxCorners    = 4;
    static constexpr i32 kMaxWalls      = 8;

    void SetNavMesh(const NavMesh* nav) { m_nav = nav; }
    const NavMesh* GetNavMesh() const { return m_nav; }

    // 追加。pos をナビメッシュへ落とせなければ -1
    i32  AddAgent(const f32 pos[3], const NavAgentParams& params);
    void RemoveAgent(i32 idx);
    void Clear();
    void SetParams(i32 idx, const NavAgentParams& params);

    // 目標へ向かわせる。前の目標から近ければ A* をやり直さずに通路の末尾を動かす
    bool RequestMoveTarget(i32 idx, const f32 pos[3]);
    bool RequestMoveVelocity(i32 idx, const f32 vel[3]);
    bool ResetMoveTarget(i32 idx);
    // 瞬間移動（ゲーム側がワープさせた時）。目標があれば張り直す
    bool Teleport(i32 idx, const f32 pos[3]);

    void Update(f32 dt);

    const NavCrowdAgent* Agent(i32 idx) const;
    i32  Capacity() const { return static_cast<i32>(m_agents.size()); }
    i32  ActiveCount() const;

    // 目標までの残り距離（通路の角をたどった距離ではなく、最後の角が終点ならそこまでの直線）
    f32  DistanceToGoal(i32 idx) const;
    // 着いたか（目標あり・終点まで tol 以内）
    bool Arrived(i32 idx, f32 tol) const;

    struct Stats { i32 plans = 0; i32 adjusts = 0; i32 samples = 0; };
    const Stats& GetStats() const { return m_stats; }

private:
    f32 QueryRange(const NavCrowdAgent& ag) const;
    void Plan(NavCrowdAgent& ag);

    const NavMesh* m_nav = nullptr;
    std::vector<NavCrowdAgent> m_agents;
    Stats m_stats;
};

// ---- 回避の中身（テストから 1 ステップ単位で叩けるように公開）----------------
struct NavAvoidParams
{
    f32 velBias = 0.4f;
    f32 weightDesVel = 2.0f;
    f32 weightCurVel = 0.75f;
    f32 weightSide = 0.75f;
    f32 weightToi = 2.5f;
    f32 horizTime = 2.5f;
    i32 adaptiveDivs = 7;
    i32 adaptiveRings = 2;
    i32 adaptiveDepth = 3;
};
NavAvoidParams NavAvoidPreset(i32 quality);

// 周りの円（他のエージェント）と線分（壁）を避ける速度を、望む速度の周りを標本化して選ぶ。
// 戻り値 = 評価した標本数。
struct NavAvoidCircle { f32 p[3]; f32 vel[3]; f32 dvel[3]; f32 rad; };
i32 SampleVelocityAdaptive(const f32 pos[3], f32 rad, f32 vmax, const f32 vel[3], const f32 dvel[3],
                           const std::vector<NavAvoidCircle>& circles, const std::vector<f32>& segs,
                           const NavAvoidParams& params, f32 outVel[3]);

} // namespace nav
} // namespace dx12e
