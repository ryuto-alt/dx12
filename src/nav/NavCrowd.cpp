// ============================================================================
// 群衆（クラウド）。組み立ては NavCrowd.h の冒頭を参照。
// 操舵・分離・速度サンプリング・重なりの押し出しの式は Detour（recastnavigation, zlib ライセンス）の
// DetourCrowd.cpp / DetourObstacleAvoidance.cpp と同じ（係数も同じ）。
// 足したのは wallMargin（壁からさらに離す余白）だけ。MikuChase.lua の SKIN と同じ考え方で、
// 「壁へ向かう速度成分を削る + 余白の内側なら通路側へ押し返す」を操舵の段で行う。
// ============================================================================

#include "nav/NavCrowd.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace dx12e
{
namespace nav
{

namespace
{
inline void Copy3(f32* d, const f32* s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
inline void Set3(f32* d, f32 x, f32 y, f32 z) { d[0] = x; d[1] = y; d[2] = z; }
inline f32 Dot2D(const f32* a, const f32* b) { return a[0] * b[0] + a[2] * b[2]; }
inline f32 Len2D(const f32* v) { return std::sqrt(v[0] * v[0] + v[2] * v[2]); }
inline f32 Dist2D(const f32* a, const f32* b)
{
    const f32 dx = b[0] - a[0], dz = b[2] - a[2];
    return std::sqrt(dx * dx + dz * dz);
}
inline f32 Perp2D(const f32* u, const f32* v) { return u[2] * v[0] - u[0] * v[2]; }
inline f32 TriArea2D(const f32* a, const f32* b, const f32* c)
{
    const f32 abx = b[0] - a[0], abz = b[2] - a[2];
    const f32 acx = c[0] - a[0], acz = c[2] - a[2];
    return acx * abz - abx * acz;
}
inline void Normalize2D(f32* v)
{
    const f32 l = Len2D(v);
    if (l > 1e-6f) { v[0] /= l; v[2] /= l; }
    else { v[0] = 0.0f; v[2] = 0.0f; }
    v[1] = 0.0f;
}
f32 DistPtSegSqr2D(const f32* pt, const f32* p, const f32* q, f32& t)
{
    const f32 pqx = q[0] - p[0], pqz = q[2] - p[2];
    f32 dx = pt[0] - p[0], dz = pt[2] - p[2];
    const f32 d = pqx * pqx + pqz * pqz;
    t = pqx * dx + pqz * dz;
    if (d > 0.0f) t /= d;
    t = std::clamp(t, 0.0f, 1.0f);
    dx = p[0] + t * pqx - pt[0];
    dz = p[2] + t * pqz - pt[2];
    return dx * dx + dz * dz;
}
// 線分 p→q の左手（通路の内側）の単位法線（FindLocalWalls の向きの約束）
inline void WallNormal(const f32* seg, f32* n)
{
    Set3(n, -(seg[5] - seg[2]), 0.0f, seg[3] - seg[0]);
    Normalize2D(n);
}

bool SweepCircleCircle(const f32* c0, f32 r0, const f32* v, const f32* c1, f32 r1, f32& tmin, f32& tmax)
{
    constexpr f32 kEps = 0.0001f;
    const f32 s[3] = { c1[0] - c0[0], 0.0f, c1[2] - c0[2] };
    const f32 r = r0 + r1;
    const f32 c = Dot2D(s, s) - r * r;
    f32 a = Dot2D(v, v);
    if (a < kEps) return false;   // 動いていない
    const f32 b = Dot2D(v, s);
    const f32 d = b * b - a * c;
    if (d < 0.0f) return false;   // 交わらない
    a = 1.0f / a;
    const f32 rd = std::sqrt(d);
    tmin = (b - rd) * a;
    tmax = (b + rd) * a;
    return true;
}

bool IsectRaySeg(const f32* ap, const f32* u, const f32* bp, const f32* bq, f32& t)
{
    const f32 v[3] = { bq[0] - bp[0], 0.0f, bq[2] - bp[2] };
    const f32 w[3] = { ap[0] - bp[0], 0.0f, ap[2] - bp[2] };
    f32 d = Perp2D(u, v);
    if (std::fabs(d) < 1e-6f) return false;
    d = 1.0f / d;
    t = Perp2D(v, w) * d;
    if (t < 0.0f || t > 1.0f) return false;
    const f32 s = Perp2D(u, w) * d;
    if (s < 0.0f || s > 1.0f) return false;
    return true;
}

struct PreparedCircle
{
    NavAvoidCircle c;
    f32 dp[3];
    f32 np[3];
};
struct PreparedSeg
{
    f32 p[3], q[3];
    bool touch;
};

f32 ProcessSample(const f32* vcand, const f32* pos, f32 rad, const f32* vel, const f32* dvel, f32 minPenalty,
                  const std::vector<PreparedCircle>& circles, const std::vector<PreparedSeg>& segs,
                  const NavAvoidParams& prm, f32 invVmax)
{
    const f32 d1[3] = { vcand[0] - dvel[0], 0.0f, vcand[2] - dvel[2] };
    const f32 d2[3] = { vcand[0] - vel[0], 0.0f, vcand[2] - vel[2] };
    const f32 vpen  = prm.weightDesVel * (Len2D(d1) * invVmax);
    const f32 vcpen = prm.weightCurVel * (Len2D(d2) * invVmax);

    // この標本がもう勝てないと分かる衝突時刻（早期打ち切り）
    const f32 minPen = minPenalty - vpen - vcpen;
    const f32 tThreshold = (prm.weightToi / minPen - 0.1f) * prm.horizTime;
    if (tThreshold - prm.horizTime > -FLT_EPSILON) return minPenalty;

    f32 tmin = prm.horizTime;
    f32 side = 0.0f;
    i32 nside = 0;
    for (const PreparedCircle& pc : circles)
    {
        // RVO（相手も半分避けてくれる前提）
        const f32 vab[3] = { vcand[0] * 2.0f - vel[0] - pc.c.vel[0], 0.0f, vcand[2] * 2.0f - vel[2] - pc.c.vel[2] };
        side += std::clamp((std::min)(Dot2D(pc.dp, vab) * 0.5f + 0.5f, Dot2D(pc.np, vab) * 2.0f), 0.0f, 1.0f);
        ++nside;
        f32 htmin = 0.0f, htmax = 0.0f;
        if (!SweepCircleCircle(pos, rad, vab, pc.c.p, pc.c.rad, htmin, htmax)) continue;
        if (htmin < 0.0f && htmax > 0.0f) htmin = -htmin * 0.5f;   // 既に重なっている＝強く避ける
        if (htmin >= 0.0f && htmin < tmin)
        {
            tmin = htmin;
            if (tmin < tThreshold) return minPenalty;
        }
    }
    for (const PreparedSeg& s : segs)
    {
        f32 htmin = 0.0f;
        if (s.touch)
        {
            // 壁に張り付いている: 壁から離れる向きなら衝突しない
            const f32 sdir[3] = { s.q[0] - s.p[0], 0.0f, s.q[2] - s.p[2] };
            const f32 snorm[3] = { -sdir[2], 0.0f, sdir[0] };
            if (Dot2D(snorm, vcand) < 0.0f) continue;
            htmin = 0.0f;
        }
        else if (!IsectRaySeg(pos, vcand, s.p, s.q, htmin))
        {
            continue;
        }
        htmin *= 2.0f;   // 壁は少し甘めに（向かい合った時に止まりすぎない）
        if (htmin < tmin)
        {
            tmin = htmin;
            if (tmin < tThreshold) return minPenalty;
        }
    }
    if (nside) side /= static_cast<f32>(nside);
    const f32 spen = prm.weightSide * side;
    const f32 tpen = prm.weightToi * (1.0f / (0.1f + tmin / prm.horizTime));
    return vpen + vcpen + spen + tpen;
}
} // namespace

const char* NavMoveStateName(NavMoveState s)
{
    switch (s)
    {
    case NavMoveState::None:     return "none";
    case NavMoveState::Pending:  return "pending";
    case NavMoveState::Valid:    return "valid";
    case NavMoveState::Failed:   return "failed";
    case NavMoveState::Velocity: return "velocity";
    }
    return "unknown";
}

NavAvoidParams NavAvoidPreset(i32 quality)
{
    NavAvoidParams p;
    p.velBias = 0.5f;
    switch (std::clamp(quality, 0, 3))
    {
    case 0: p.adaptiveDivs = 5; p.adaptiveRings = 2; p.adaptiveDepth = 1; break;   // low
    case 1: p.adaptiveDivs = 5; p.adaptiveRings = 2; p.adaptiveDepth = 2; break;   // medium
    case 2: p.adaptiveDivs = 7; p.adaptiveRings = 2; p.adaptiveDepth = 3; break;   // good
    default: p.adaptiveDivs = 7; p.adaptiveRings = 3; p.adaptiveDepth = 3; break;  // high
    }
    return p;
}

i32 SampleVelocityAdaptive(const f32 pos[3], f32 rad, f32 vmax, const f32 vel[3], const f32 dvel[3],
                           const std::vector<NavAvoidCircle>& circles, const std::vector<f32>& segs,
                           const NavAvoidParams& prm, f32 outVel[3])
{
    Set3(outVel, 0.0f, 0.0f, 0.0f);
    if (vmax <= 0.0f) return 0;
    const f32 invVmax = 1.0f / vmax;

    // ---- 障害物の下ごしらえ ----
    thread_local std::vector<PreparedCircle> pcs;
    thread_local std::vector<PreparedSeg> pss;
    pcs.clear();
    pss.clear();
    for (const NavAvoidCircle& c : circles)
    {
        PreparedCircle pc;
        pc.c = c;
        Set3(pc.dp, c.p[0] - pos[0], 0.0f, c.p[2] - pos[2]);
        Normalize2D(pc.dp);
        const f32 dv[3] = { c.dvel[0] - dvel[0], 0.0f, c.dvel[2] - dvel[2] };
        const f32 orig[3] = { 0, 0, 0 };
        const f32 a = TriArea2D(orig, pc.dp, dv);
        if (a < 0.01f) Set3(pc.np, -pc.dp[2], 0.0f, pc.dp[0]);
        else           Set3(pc.np, pc.dp[2], 0.0f, -pc.dp[0]);
        pcs.push_back(pc);
    }
    for (size_t i = 0; i + 5 < segs.size(); i += 6)
    {
        PreparedSeg s;
        Copy3(s.p, &segs[i]);
        Copy3(s.q, &segs[i + 3]);
        f32 t = 0.0f;
        s.touch = DistPtSegSqr2D(pos, s.p, s.q, t) < 0.01f * 0.01f;
        pss.push_back(s);
    }

    // ---- 標本の並び（望む速度の向きにそろえた同心円）----
    const i32 nd = std::clamp(prm.adaptiveDivs, 1, 32);
    const i32 nr = std::clamp(prm.adaptiveRings, 1, 4);
    const i32 depth = (std::max)(1, prm.adaptiveDepth);
    const f32 da = (1.0f / static_cast<f32>(nd)) * 6.28318530718f;
    f32 ddir[3] = { dvel[0], 0.0f, dvel[2] };
    Normalize2D(ddir);
    if (Len2D(ddir) < 0.5f) Set3(ddir, 1.0f, 0.0f, 0.0f);   // 止まっている時の基準向き（決定論のため固定）
    thread_local std::vector<f32> pat;
    pat.clear();
    pat.push_back(0.0f); pat.push_back(0.0f);   // ゼロ速度も必ず候補に入れる
    for (i32 j = 0; j < nr; ++j)
    {
        const f32 r = static_cast<f32>(nr - j) / static_cast<f32>(nr);
        const f32 base = (j % 2) ? da * 0.5f : 0.0f;
        for (i32 k = 0; k < nd; ++k)
        {
            const f32 a = base + da * static_cast<f32>(k);
            const f32 ca = std::cos(a), sa = std::sin(a);
            pat.push_back((ddir[0] * ca - ddir[2] * sa) * r);
            pat.push_back((ddir[0] * sa + ddir[2] * ca) * r);
        }
    }

    // ---- 粗い → 細かい の順に絞り込む ----
    f32 cr = vmax * (1.0f - prm.velBias);
    f32 res[3] = { dvel[0] * prm.velBias, 0.0f, dvel[2] * prm.velBias };
    i32 ns = 0;
    for (i32 k = 0; k < depth; ++k)
    {
        f32 minPenalty = FLT_MAX;
        f32 bvel[3] = { 0, 0, 0 };
        for (size_t i = 0; i + 1 < pat.size(); i += 2)
        {
            const f32 vcand[3] = { res[0] + pat[i] * cr, 0.0f, res[2] + pat[i + 1] * cr };
            if (vcand[0] * vcand[0] + vcand[2] * vcand[2] > (vmax + 0.001f) * (vmax + 0.001f)) continue;
            const f32 penalty = ProcessSample(vcand, pos, rad, vel, dvel, minPenalty, pcs, pss, prm, invVmax);
            ++ns;
            if (penalty < minPenalty)
            {
                minPenalty = penalty;
                Copy3(bvel, vcand);
            }
        }
        Copy3(res, bvel);
        cr *= 0.5f;
    }
    Copy3(outVel, res);
    return ns;
}

// ---------------------------------------------------------------------------
// NavCrowd
// ---------------------------------------------------------------------------
namespace
{
void SearchExt(const NavAgentParams& p, f32 ext[3])
{
    const f32 r = (std::max)(2.0f, p.radius * 4.0f);
    ext[0] = r;
    ext[1] = (std::max)(p.height * 2.0f, 4.0f);
    ext[2] = r;
}
} // namespace

i32 NavCrowd::AddAgent(const f32 pos[3], const NavAgentParams& params)
{
    if (!m_nav || m_nav->Empty()) return -1;
    f32 ext[3];
    SearchExt(params, ext);
    f32 np[3];
    const i32 poly = m_nav->FindNearestPoly(pos, ext, np);
    if (poly < 0) return -1;

    // 空きは若い番号から使う（同じ操作列なら同じ番号＝決定論）
    i32 idx = -1;
    for (i32 i = 0; i < static_cast<i32>(m_agents.size()); ++i)
        if (!m_agents[static_cast<size_t>(i)].active) { idx = i; break; }
    if (idx < 0)
    {
        idx = static_cast<i32>(m_agents.size());
        m_agents.emplace_back();
    }
    NavCrowdAgent& ag = m_agents[static_cast<size_t>(idx)];
    ag = NavCrowdAgent{};
    ag.active = true;
    ag.params = params;
    ag.corridor.Reset(poly, np, m_nav->Generation());
    Copy3(ag.npos, np);
    return idx;
}

void NavCrowd::RemoveAgent(i32 idx)
{
    if (idx < 0 || idx >= static_cast<i32>(m_agents.size())) return;
    m_agents[static_cast<size_t>(idx)] = NavCrowdAgent{};
}

void NavCrowd::Clear()
{
    m_agents.clear();
    m_stats = Stats{};
}

void NavCrowd::SetParams(i32 idx, const NavAgentParams& params)
{
    if (idx < 0 || idx >= static_cast<i32>(m_agents.size()) || !m_agents[static_cast<size_t>(idx)].active) return;
    m_agents[static_cast<size_t>(idx)].params = params;
}

const NavCrowdAgent* NavCrowd::Agent(i32 idx) const
{
    if (idx < 0 || idx >= static_cast<i32>(m_agents.size())) return nullptr;
    const NavCrowdAgent& ag = m_agents[static_cast<size_t>(idx)];
    return ag.active ? &ag : nullptr;
}

i32 NavCrowd::ActiveCount() const
{
    i32 n = 0;
    for (const NavCrowdAgent& ag : m_agents) if (ag.active) ++n;
    return n;
}

f32 NavCrowd::QueryRange(const NavCrowdAgent& ag) const
{
    return ag.params.collisionQueryRange > 0.0f ? ag.params.collisionQueryRange : ag.params.radius * 12.0f;
}

void NavCrowd::Plan(NavCrowdAgent& ag)
{
    f32 ext[3];
    SearchExt(ag.params, ext);
    if (!ag.corridor.IsValid(*m_nav))
    {
        f32 np[3];
        const i32 poly = m_nav->FindNearestPoly(ag.npos, ext, np);
        if (poly < 0) { ag.targetState = NavMoveState::Failed; return; }
        ag.corridor.Reset(poly, np, m_nav->Generation());
        Copy3(ag.npos, np);
    }
    const NavPathStatus st = ag.corridor.Plan(*m_nav, ag.targetPos, ext);
    ag.targetState = (st == NavPathStatus::Failed) ? NavMoveState::Failed : NavMoveState::Valid;
    ag.partial = (st == NavPathStatus::Partial);
    ag.sincePlan = 0.0f;
    ++m_stats.plans;
}

bool NavCrowd::RequestMoveTarget(i32 idx, const f32 pos[3])
{
    if (!Agent(idx) || !m_nav) return false;
    NavCrowdAgent& ag = m_agents[static_cast<size_t>(idx)];
    // 近くへ動いただけなら A* をやり直さず、通路の末尾を滑らせる（追いかけで毎回 A* を張らない）
    constexpr f32 kAdjustDist = 1.5f;
    if (ag.targetState == NavMoveState::Valid && !ag.partial && ag.corridor.IsValid(*m_nav) &&
        Dist2D(pos, ag.corridor.Target()) < kAdjustDist)
    {
        Copy3(ag.targetPos, pos);
        if (ag.corridor.MoveTargetPosition(*m_nav, pos))
        {
            ++m_stats.adjusts;
            return true;
        }
    }
    Copy3(ag.targetPos, pos);
    ag.targetState = NavMoveState::Pending;
    return true;
}

bool NavCrowd::RequestMoveVelocity(i32 idx, const f32 vel[3])
{
    if (!Agent(idx)) return false;
    NavCrowdAgent& ag = m_agents[static_cast<size_t>(idx)];
    Copy3(ag.targetPos, vel);
    ag.targetState = NavMoveState::Velocity;
    return true;
}

bool NavCrowd::ResetMoveTarget(i32 idx)
{
    if (!Agent(idx)) return false;
    NavCrowdAgent& ag = m_agents[static_cast<size_t>(idx)];
    ag.targetState = NavMoveState::None;
    Set3(ag.targetPos, 0, 0, 0);
    Set3(ag.dvel, 0, 0, 0);
    ag.partial = false;
    ag.corners.clear();
    ag.cornerFlags.clear();
    return true;
}

bool NavCrowd::Teleport(i32 idx, const f32 pos[3])
{
    if (!Agent(idx) || !m_nav) return false;
    NavCrowdAgent& ag = m_agents[static_cast<size_t>(idx)];
    f32 ext[3];
    SearchExt(ag.params, ext);
    f32 np[3];
    const i32 poly = m_nav->FindNearestPoly(pos, ext, np);
    if (poly < 0) return false;
    ag.corridor.Reset(poly, np, m_nav->Generation());
    Copy3(ag.npos, np);
    Set3(ag.vel, 0, 0, 0);
    Set3(ag.nvel, 0, 0, 0);
    if (ag.targetState == NavMoveState::Valid || ag.targetState == NavMoveState::Failed)
        ag.targetState = NavMoveState::Pending;
    return true;
}

f32 NavCrowd::DistanceToGoal(i32 idx) const
{
    const NavCrowdAgent* ag = Agent(idx);
    if (!ag || ag->targetState != NavMoveState::Valid) return 0.0f;
    if (!ag->cornerFlags.empty() && (ag->cornerFlags.back() & kStraightPathEnd))
    {
        const size_t n = ag->corners.size() / 3;
        return Dist2D(ag->npos, &ag->corners[(n - 1) * 3]);
    }
    return Dist2D(ag->npos, ag->corridor.Target());
}

bool NavCrowd::Arrived(i32 idx, f32 tol) const
{
    const NavCrowdAgent* ag = Agent(idx);
    if (!ag || ag->targetState != NavMoveState::Valid) return false;
    return Dist2D(ag->npos, ag->corridor.Target()) <= tol;
}

void NavCrowd::Update(f32 dt)
{
    if (!m_nav || m_nav->Empty() || dt <= 0.0f) return;

    thread_local std::vector<i32> act;
    act.clear();
    for (i32 i = 0; i < static_cast<i32>(m_agents.size()); ++i)
        if (m_agents[static_cast<size_t>(i)].active) act.push_back(i);
    if (act.empty()) return;
    auto A = [&](i32 i) -> NavCrowdAgent& { return m_agents[static_cast<size_t>(i)]; };

    // ---- 1. 通路の検証（焼き直しで無効になったら位置を探し直す）と定期的な張り直し ----
    for (const i32 i : act)
    {
        NavCrowdAgent& ag = A(i);
        if (!ag.corridor.IsValid(*m_nav))
        {
            f32 ext[3];
            SearchExt(ag.params, ext);
            f32 np[3];
            const i32 poly = m_nav->FindNearestPoly(ag.npos, ext, np);
            if (poly >= 0)
            {
                ag.corridor.Reset(poly, np, m_nav->Generation());
                Copy3(ag.npos, np);
            }
            if (ag.targetState == NavMoveState::Valid) ag.targetState = NavMoveState::Pending;
        }
        ag.sincePlan += dt;
        // 届かない目標は 1 秒ごとに張り直す（扉が開いた等で届くようになるかもしれない）
        if (ag.targetState == NavMoveState::Valid && ag.partial && ag.sincePlan > 1.0f)
            ag.targetState = NavMoveState::Pending;
        // 末尾を滑らせ続けた通路は遠回りになりうるので 2 秒ごとに A* で引き直す
        if (ag.targetState == NavMoveState::Valid && ag.sincePlan > 2.0f &&
            Dist2D(ag.targetPos, ag.corridor.Target()) > 0.01f)
            ag.targetState = NavMoveState::Pending;
    }

    // ---- 2. 経路の要求を処理 ----
    for (const i32 i : act)
        if (A(i).targetState == NavMoveState::Pending) Plan(A(i));

    // ---- 3. 近傍（距離の近い順、同距離は番号順）----
    for (const i32 i : act)
    {
        NavCrowdAgent& ag = A(i);
        ag.neis.clear();
        const f32 range = QueryRange(ag);
        for (const i32 j : act)
        {
            if (j == i) continue;
            const NavCrowdAgent& o = A(j);
            const f32 dy = o.npos[1] - ag.npos[1];
            if (dy >= ag.params.height || dy <= -ag.params.height) continue;
            const f32 d = Dist2D(ag.npos, o.npos);
            if (d > range) continue;
            auto it = std::upper_bound(ag.neis.begin(), ag.neis.end(), d,
                                       [](f32 v, const NavCrowdAgent::Nei& n) { return v < n.dist; });
            ag.neis.insert(it, { j, d });
            if (static_cast<i32>(ag.neis.size()) > kMaxNeighbours) ag.neis.pop_back();
        }
    }

    // ---- 4. 角と周りの壁 ----
    for (const i32 i : act)
    {
        NavCrowdAgent& ag = A(i);
        m_nav->FindLocalWalls(ag.corridor.FirstPoly(), ag.npos, QueryRange(ag), ag.walls, kMaxWalls);
        if (ag.targetState == NavMoveState::Valid)
        {
            ag.corridor.FindCorners(*m_nav, ag.corners, &ag.cornerFlags, kMaxCorners, ag.params.wallMargin);
            const i32 nc = static_cast<i32>(ag.corners.size() / 3);
            if (ag.params.optimizeVisibility && nc > 0)
            {
                const f32* tgt = &ag.corners[static_cast<size_t>((std::min)(1, nc - 1)) * 3];
                const f32 range = ag.params.pathOptimizationRange > 0.0f ? ag.params.pathOptimizationRange
                                                                         : ag.params.radius * 30.0f;
                ag.corridor.OptimizePathVisibility(*m_nav, tgt, range);
            }
        }
        else
        {
            ag.corners.clear();
            ag.cornerFlags.clear();
        }
    }

    // ---- 5. 操舵（望む速度）----
    for (const i32 i : act)
    {
        NavCrowdAgent& ag = A(i);
        f32 dvel[3] = { 0, 0, 0 };
        if (ag.targetState == NavMoveState::Velocity)
        {
            Copy3(dvel, ag.targetPos);
            dvel[1] = 0.0f;
            ag.desiredSpeed = Len2D(dvel);
        }
        else if (ag.targetState == NavMoveState::Valid && !ag.corners.empty())
        {
            const i32 nc = static_cast<i32>(ag.corners.size() / 3);
            f32 dir[3];
            const f32* p0 = &ag.corners[0];
            if (ag.params.anticipateTurns)
            {
                // 次の次の角を混ぜて先読みで曲がる（Detour の calcSmoothSteerDirection）
                const f32* p1 = &ag.corners[static_cast<size_t>((std::min)(1, nc - 1)) * 3];
                f32 dir0[3] = { p0[0] - ag.npos[0], 0.0f, p0[2] - ag.npos[2] };
                f32 dir1[3] = { p1[0] - ag.npos[0], 0.0f, p1[2] - ag.npos[2] };
                const f32 len0 = Len2D(dir0);
                const f32 len1 = Len2D(dir1);
                if (len1 > 0.001f) { dir1[0] /= len1; dir1[2] /= len1; }
                Set3(dir, dir0[0] - dir1[0] * len0 * 0.5f, 0.0f, dir0[2] - dir1[2] * len0 * 0.5f);
            }
            else
            {
                Set3(dir, p0[0] - ag.npos[0], 0.0f, p0[2] - ag.npos[2]);
            }
            Normalize2D(dir);
            // 終点の手前で減速
            const f32 slow = ag.params.slowDownRadius > 0.0f ? ag.params.slowDownRadius : ag.params.radius * 2.0f;
            f32 toGoal = slow;
            if (ag.cornerFlags.back() & kStraightPathEnd)
                toGoal = (std::min)(Dist2D(ag.npos, &ag.corners[static_cast<size_t>(nc - 1) * 3]), slow);
            const f32 speedScale = slow > 0.0f ? toGoal / slow : 1.0f;
            ag.desiredSpeed = ag.params.maxSpeed;
            Set3(dvel, dir[0] * ag.desiredSpeed * speedScale, 0.0f, dir[2] * ag.desiredSpeed * speedScale);
        }
        else
        {
            ag.desiredSpeed = 0.0f;
        }

        // 分離（近すぎる相手から離れる）
        if (ag.params.separation && ag.params.separationWeight > 0.0f &&
            (ag.targetState == NavMoveState::Valid || ag.targetState == NavMoveState::Velocity))
        {
            const f32 sepDist = QueryRange(ag);
            const f32 invSep = 1.0f / sepDist;
            f32 w = 0.0f;
            f32 disp[3] = { 0, 0, 0 };
            for (const auto& n : ag.neis)
            {
                const NavCrowdAgent& o = A(n.idx);
                const f32 diff[3] = { ag.npos[0] - o.npos[0], 0.0f, ag.npos[2] - o.npos[2] };
                const f32 d2 = diff[0] * diff[0] + diff[2] * diff[2];
                if (d2 < 0.00001f || d2 > sepDist * sepDist) continue;
                const f32 dist = std::sqrt(d2);
                const f32 weight = ag.params.separationWeight * (1.0f - (dist * invSep) * (dist * invSep));
                disp[0] += diff[0] * (weight / dist);
                disp[2] += diff[2] * (weight / dist);
                w += 1.0f;
            }
            if (w > 0.0001f)
            {
                dvel[0] += disp[0] / w;
                dvel[2] += disp[2] / w;
                const f32 sp2 = dvel[0] * dvel[0] + dvel[2] * dvel[2];
                const f32 want2 = ag.desiredSpeed * ag.desiredSpeed;
                if (sp2 > want2 && sp2 > 0.0f)
                {
                    dvel[0] *= want2 / sp2;
                    dvel[2] *= want2 / sp2;
                }
            }
        }

        // 壁の余白（wallMargin）: 余白の内側では壁へ向かう成分を削り、通路側へ押し返す
        if (ag.params.wallMargin > 0.0f && ag.desiredSpeed > 0.0f)
        {
            for (size_t s = 0; s + 5 < ag.walls.size(); s += 6)
            {
                const f32* seg = &ag.walls[s];
                f32 n[3];
                WallNormal(seg, n);
                const f32 rel[3] = { ag.npos[0] - seg[0], 0.0f, ag.npos[2] - seg[2] };
                if (Dot2D(rel, n) < 0.0f) continue;   // 壁の裏側（別の通路）
                f32 t = 0.0f;
                const f32 d = std::sqrt(DistPtSegSqr2D(ag.npos, seg, seg + 3, t));
                if (d >= ag.params.wallMargin) continue;
                const f32 into = -Dot2D(dvel, n);
                if (into > 0.0f) { dvel[0] += n[0] * into; dvel[2] += n[2] * into; }
                const f32 push = (ag.params.wallMargin - d) / ag.params.wallMargin * ag.desiredSpeed * 0.5f;
                dvel[0] += n[0] * push;
                dvel[2] += n[2] * push;
            }
            const f32 sp = Len2D(dvel);
            if (sp > ag.desiredSpeed && sp > 0.0f)
            {
                dvel[0] *= ag.desiredSpeed / sp;
                dvel[2] *= ag.desiredSpeed / sp;
            }
        }
        Copy3(ag.dvel, dvel);
    }

    // ---- 6. 速度の計画（周りのエージェントと壁を避ける）----
    thread_local std::vector<NavAvoidCircle> circles;
    thread_local std::vector<f32> segs;
    for (const i32 i : act)
    {
        NavCrowdAgent& ag = A(i);
        const bool moving = (ag.targetState == NavMoveState::Valid || ag.targetState == NavMoveState::Velocity);
        if (ag.params.obstacleAvoidance && moving && ag.desiredSpeed > 0.0f)
        {
            circles.clear();
            segs.clear();
            for (const auto& n : ag.neis)
            {
                const NavCrowdAgent& o = A(n.idx);
                NavAvoidCircle c;
                Copy3(c.p, o.npos);
                Copy3(c.vel, o.vel);
                Copy3(c.dvel, o.dvel);
                c.rad = o.params.radius;
                circles.push_back(c);
            }
            for (size_t s = 0; s + 5 < ag.walls.size(); s += 6)
            {
                const f32* seg = &ag.walls[s];
                f32 n[3];
                WallNormal(seg, n);
                const f32 rel[3] = { ag.npos[0] - seg[0], 0.0f, ag.npos[2] - seg[2] };
                if (Dot2D(rel, n) < 0.0f) continue;   // 裏向きの壁は無視（Detour の triArea 判定と同じ意味）
                segs.insert(segs.end(), seg, seg + 6);
            }
            m_stats.samples += SampleVelocityAdaptive(ag.npos, ag.params.radius, ag.desiredSpeed, ag.vel, ag.dvel,
                                                      circles, segs, NavAvoidPreset(ag.params.avoidanceQuality),
                                                      ag.nvel);
        }
        else
        {
            Copy3(ag.nvel, ag.dvel);
        }
    }

    // ---- 7. 積分（加速度制限）----
    for (const i32 i : act)
    {
        NavCrowdAgent& ag = A(i);
        const f32 maxDelta = ag.params.maxAccel * dt;
        f32 dv[3] = { ag.nvel[0] - ag.vel[0], 0.0f, ag.nvel[2] - ag.vel[2] };
        const f32 ds = Len2D(dv);
        if (ds > maxDelta && ds > 0.0f) { dv[0] *= maxDelta / ds; dv[2] *= maxDelta / ds; }
        ag.vel[0] += dv[0];
        ag.vel[2] += dv[2];
        ag.vel[1] = 0.0f;
        if (Len2D(ag.vel) > 0.0001f)
        {
            ag.npos[0] += ag.vel[0] * dt;
            ag.npos[2] += ag.vel[2] * dt;
        }
        else
        {
            Set3(ag.vel, 0, 0, 0);
        }
    }

    // ---- 8. 重なりの押し出し（Detour と同じ 4 反復・係数 0.7）----
    constexpr f32 kResolveFactor = 0.7f;
    for (i32 iter = 0; iter < 4; ++iter)
    {
        for (const i32 i : act)
        {
            NavCrowdAgent& ag = A(i);
            Set3(ag.disp, 0, 0, 0);
            f32 w = 0.0f;
            for (const auto& n : ag.neis)
            {
                const NavCrowdAgent& o = A(n.idx);
                f32 diff[3] = { ag.npos[0] - o.npos[0], 0.0f, ag.npos[2] - o.npos[2] };
                f32 dist = diff[0] * diff[0] + diff[2] * diff[2];
                const f32 rr = ag.params.radius + o.params.radius;
                if (dist > rr * rr) continue;
                dist = std::sqrt(dist);
                f32 pen = rr - dist;
                if (dist < 0.0001f)
                {
                    // ぴったり重なった: 番号で向きを分けて左右へ（決定論）
                    if (i > n.idx) Set3(diff, -ag.dvel[2], 0.0f, ag.dvel[0]);
                    else           Set3(diff, ag.dvel[2], 0.0f, -ag.dvel[0]);
                    if (Len2D(diff) < 1e-6f) Set3(diff, (i > n.idx) ? 1.0f : -1.0f, 0.0f, 0.0f);
                    pen = 0.01f;
                }
                else
                {
                    pen = (1.0f / dist) * (pen * 0.5f) * kResolveFactor;
                }
                ag.disp[0] += diff[0] * pen;
                ag.disp[2] += diff[2] * pen;
                w += 1.0f;
            }
            if (w > 0.0001f) { ag.disp[0] /= w; ag.disp[2] /= w; }
        }
        for (const i32 i : act)
        {
            NavCrowdAgent& ag = A(i);
            ag.npos[0] += ag.disp[0];
            ag.npos[2] += ag.disp[2];
        }
    }

    // ---- 9. ナビメッシュの上へ滑らせて確定（ここで外へは出ない）----
    for (const i32 i : act)
    {
        NavCrowdAgent& ag = A(i);
        ag.corridor.MovePosition(*m_nav, ag.npos);
        Copy3(ag.npos, ag.corridor.Pos());
        if (ag.targetState == NavMoveState::None || ag.targetState == NavMoveState::Velocity)
        {
            ag.corridor.Reset(ag.corridor.FirstPoly(), ag.npos, m_nav->Generation());
            ag.partial = false;
        }
    }
}

} // namespace nav
} // namespace dx12e
