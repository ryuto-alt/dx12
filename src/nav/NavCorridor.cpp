// ============================================================================
// 通路（パス・コリドー）。考え方は NavCorridor.h の冒頭を参照。
// マージの規則は Detour（recastnavigation, zlib ライセンス）の DetourPathCorridor.cpp と同じ。
// ============================================================================

#include "nav/NavCorridor.h"

#include <algorithm>
#include <cmath>

namespace dx12e
{
namespace nav
{

namespace
{
inline void Copy3(f32* d, const f32* s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
inline f32 Dist2D(const f32* a, const f32* b)
{
    const f32 dx = b[0] - a[0], dz = b[2] - a[2];
    return std::sqrt(dx * dx + dz * dz);
}
} // namespace

// ---------------------------------------------------------------------------
// マージ
// ---------------------------------------------------------------------------
void MergeCorridorStartMoved(std::vector<i32>& path, const std::vector<i32>& visited)
{
    if (visited.empty()) return;
    // 通路と visited の両方にある、通路の一番先のポリゴンを探す
    i32 furthestPath = -1, furthestVisited = -1;
    for (i32 i = static_cast<i32>(path.size()) - 1; i >= 0; --i)
    {
        bool found = false;
        for (i32 j = static_cast<i32>(visited.size()) - 1; j >= 0; --j)
        {
            if (path[static_cast<size_t>(i)] == visited[static_cast<size_t>(j)])
            {
                furthestPath = i;
                furthestVisited = j;
                found = true;
            }
        }
        if (found) break;
    }
    // 共通が無い＝通路の外へ押し出された。着地点のポリゴンを先頭に足すしかない
    if (furthestPath == -1 || furthestVisited == -1)
    {
        path.insert(path.begin(), visited.back());
        return;
    }
    // 先頭 = visited を逆順（着地点 → 共通点）、そのあと通路の共通点より先
    std::vector<i32> out;
    out.reserve(visited.size() + path.size());
    for (i32 j = static_cast<i32>(visited.size()) - 1; j >= furthestVisited; --j)
        out.push_back(visited[static_cast<size_t>(j)]);
    for (size_t i = static_cast<size_t>(furthestPath) + 1; i < path.size(); ++i)
        out.push_back(path[i]);
    path.swap(out);
}

void MergeCorridorEndMoved(std::vector<i32>& path, const std::vector<i32>& visited)
{
    if (visited.empty()) return;
    // 通路の手前から見て、visited にある最初のポリゴン（visited 側は一番先のもの）
    i32 furthestPath = -1, furthestVisited = -1;
    for (i32 i = 0; i < static_cast<i32>(path.size()); ++i)
    {
        bool found = false;
        for (i32 j = static_cast<i32>(visited.size()) - 1; j >= 0; --j)
        {
            if (path[static_cast<size_t>(i)] == visited[static_cast<size_t>(j)])
            {
                furthestPath = i;
                furthestVisited = j;
                found = true;
            }
        }
        if (found) break;
    }
    if (furthestPath == -1 || furthestVisited == -1) return;
    path.resize(static_cast<size_t>(furthestPath) + 1);
    for (size_t j = static_cast<size_t>(furthestVisited) + 1; j < visited.size(); ++j)
        path.push_back(visited[j]);
}

void MergeCorridorStartShortcut(std::vector<i32>& path, const std::vector<i32>& visited)
{
    i32 furthestPath = -1, furthestVisited = -1;
    for (i32 i = static_cast<i32>(path.size()) - 1; i >= 0; --i)
    {
        bool found = false;
        for (i32 j = static_cast<i32>(visited.size()) - 1; j >= 0; --j)
        {
            if (path[static_cast<size_t>(i)] == visited[static_cast<size_t>(j)])
            {
                furthestPath = i;
                furthestVisited = j;
                found = true;
            }
        }
        if (found) break;
    }
    if (furthestPath == -1 || furthestVisited <= 0) return;
    // 先頭 = visited[0 .. furthestVisited-1]、そのあと通路の共通点から先
    std::vector<i32> out;
    out.reserve(visited.size() + path.size());
    for (i32 j = 0; j < furthestVisited; ++j) out.push_back(visited[static_cast<size_t>(j)]);
    for (size_t i = static_cast<size_t>(furthestPath); i < path.size(); ++i) out.push_back(path[i]);
    path.swap(out);
}

// ---------------------------------------------------------------------------
void NavCorridor::Reset(i32 poly, const f32 pos[3], u32 generation)
{
    Copy3(m_pos, pos);
    Copy3(m_target, pos);
    m_path.clear();
    if (poly >= 0) m_path.push_back(poly);
    m_generation = generation;
    m_status = poly >= 0 ? NavPathStatus::Complete : NavPathStatus::Failed;
    m_hasTarget = false;
}

void NavCorridor::Clear()
{
    m_path.clear();
    m_status = NavPathStatus::Failed;
    m_hasTarget = false;
    m_generation = 0;
}

bool NavCorridor::IsValid(const NavMesh& nav) const
{
    if (m_path.empty() || m_generation != nav.Generation()) return false;
    for (const i32 p : m_path) if (!nav.IsValidPoly(p)) return false;
    return true;
}

NavPathStatus NavCorridor::Plan(const NavMesh& nav, const f32 target[3], const f32 ext[3])
{
    m_hasTarget = true;
    // 始点: 今の通路の先頭が使えるならそれ（位置を探し直さない＝縁でポリゴンを取り違えない）
    i32 startPoly = -1;
    if (!m_path.empty() && m_generation == nav.Generation() && nav.IsValidPoly(m_path.front()))
        startPoly = m_path.front();
    if (startPoly < 0)
    {
        f32 p[3];
        startPoly = nav.FindNearestPoly(m_pos, ext, p);
        if (startPoly < 0)
        {
            m_path.clear();
            m_status = NavPathStatus::Failed;
            return m_status;
        }
        Copy3(m_pos, p);
    }
    m_generation = nav.Generation();

    f32 tpos[3];
    const i32 endPoly = nav.FindNearestPoly(target, ext, tpos);
    if (endPoly < 0)
    {
        m_path.assign(1, startPoly);
        Copy3(m_target, m_pos);
        m_status = NavPathStatus::Failed;
        return m_status;
    }

    m_status = nav.FindPolyPath(startPoly, endPoly, m_pos, tpos, m_path);
    if (m_status == NavPathStatus::Failed || m_path.empty())
    {
        m_path.assign(1, startPoly);
        Copy3(m_target, m_pos);
        m_status = NavPathStatus::Failed;
        return m_status;
    }
    if (m_status == NavPathStatus::Partial)
        nav.ClosestPointOnPoly(m_path.back(), tpos, m_target);
    else
        Copy3(m_target, tpos);
    return m_status;
}

i32 NavCorridor::FindCorners(const NavMesh& nav, std::vector<f32>& outCorners, std::vector<u8>* outFlags,
                             i32 maxCorners, f32 margin) const
{
    outCorners.clear();
    if (outFlags) outFlags->clear();
    if (m_path.empty() || maxCorners <= 0) return 0;

    thread_local std::vector<f32> pts;
    thread_local std::vector<u8> flags;
    const f32* goal = m_hasTarget ? m_target : m_pos;
    const i32 n = nav.FindStraightPath(m_pos, goal, m_path.data(), static_cast<i32>(m_path.size()),
                                       pts, &flags, maxCorners + 1);
    // 先頭は現在地そのもの。現在地に近すぎる角も捨てる（Detour と同じ 1cm）
    for (i32 k = 1; k < n; ++k)
    {
        const f32* c = &pts[static_cast<size_t>(k) * 3];
        const bool isEnd = (flags[static_cast<size_t>(k)] & kStraightPathEnd) != 0;
        if (!isEnd && Dist2D(c, m_pos) < 0.01f) continue;
        outCorners.insert(outCorners.end(), { c[0], c[1], c[2] });
        if (outFlags) outFlags->push_back(flags[static_cast<size_t>(k)]);
        if (static_cast<i32>(outCorners.size() / 3) >= maxCorners) break;
    }
    // 目標に着いている（角が 1 つも残っていない）なら目標点を 1 つ返す
    if (outCorners.empty() && m_hasTarget)
    {
        outCorners.insert(outCorners.end(), { goal[0], goal[1], goal[2] });
        if (outFlags) outFlags->push_back(kStraightPathEnd);
    }

    // ---- 角を壁から離す（margin）----
    // 角はポリゴンの頂点＝壁の角ちょうど（クリアランス 0）。そのまま狙うと壁を擦る。
    // 前後の区間の二等分線（＝通路の開いている側）へ margin だけずらす。
    // 狭い所で反対側の壁へ押し込まないよう、二等分線方向の空きの半分までに抑える。
    if (margin > 0.0f)
    {
        const i32 nc = static_cast<i32>(outCorners.size() / 3);
        const f32 ext[3] = { 0.5f, 2.0f, 0.5f };
        for (i32 k = 0; k < nc; ++k)
        {
            if (outFlags && ((*outFlags)[static_cast<size_t>(k)] & kStraightPathEnd)) continue;
            f32* c = &outCorners[static_cast<size_t>(k) * 3];
            const f32* prev = (k == 0) ? m_pos : &outCorners[static_cast<size_t>(k - 1) * 3];
            const f32* next = (k + 1 < nc) ? &outCorners[static_cast<size_t>(k + 1) * 3] : goal;
            f32 a[2] = { prev[0] - c[0], prev[2] - c[2] };
            f32 b[2] = { next[0] - c[0], next[2] - c[2] };
            const f32 la = std::sqrt(a[0] * a[0] + a[1] * a[1]);
            const f32 lb = std::sqrt(b[0] * b[0] + b[1] * b[1]);
            if (la < 1e-4f || lb < 1e-4f) continue;
            // 前後へ向かう 2 本の和は「曲がりの内側」＝回り込んでいる壁の方を向く。押すのはその逆
            f32 d[2] = { -(a[0] / la + b[0] / lb), -(a[1] / la + b[1] / lb) };
            const f32 ld = std::sqrt(d[0] * d[0] + d[1] * d[1]);
            if (ld < 1e-3f) continue;   // ほぼ直線（折れていない）
            d[0] /= ld; d[1] /= ld;

            // ★角は頂点ちょうど。そこから撃つと「どの辺から出たか」が退化するので、
            //   二等分線の方向へ 2cm 進めた点から空きを測る（そこが歩けなければ押さない）
            constexpr f32 kLead = 0.02f;
            const f32 s0[3] = { c[0] + d[0] * kLead, c[1], c[2] + d[1] * kLead };
            f32 cp[3];
            const i32 poly = nav.FindNearestPoly(s0, ext, cp);
            if (poly < 0 || Dist2D(cp, s0) > 0.005f) continue;
            const f32 reach = margin * 2.0f;
            const f32 probe[3] = { c[0] + d[0] * reach, c[1], c[2] + d[1] * reach };
            const NavRaycastResult r = nav.RaycastEx(s0, probe, poly);
            if (r.status == NavRayStatus::StartOffMesh || r.status == NavRayStatus::Truncated) continue;
            const f32 room = kLead + ((r.status == NavRayStatus::Hit) ? r.t : 1.0f) * (reach - kLead);
            const f32 push = (std::min)(margin, room * 0.5f);
            c[0] += d[0] * push;
            c[2] += d[1] * push;
        }
    }
    return static_cast<i32>(outCorners.size() / 3);
}

bool NavCorridor::MovePosition(const NavMesh& nav, const f32 npos[3])
{
    if (m_path.empty() || !nav.IsValidPoly(m_path.front())) return false;
    thread_local std::vector<i32> visited;
    f32 result[3];
    i32 poly = -1;
    if (!nav.MoveAlongSurface(m_pos, npos, m_path.front(), result, poly, &visited)) return false;
    MergeCorridorStartMoved(m_path, visited);
    Copy3(m_pos, result);
    return true;
}

bool NavCorridor::MoveTargetPosition(const NavMesh& nav, const f32 npos[3])
{
    if (m_path.empty() || !nav.IsValidPoly(m_path.back())) return false;
    thread_local std::vector<i32> visited;
    f32 result[3];
    i32 poly = -1;
    if (!nav.MoveAlongSurface(m_target, npos, m_path.back(), result, poly, &visited)) return false;
    MergeCorridorEndMoved(m_path, visited);
    Copy3(m_target, result);
    m_hasTarget = true;
    return Dist2D(result, npos) < 0.05f;
}

void NavCorridor::OptimizePathVisibility(const NavMesh& nav, const f32 next[3], f32 range)
{
    if (m_path.empty()) return;
    const f32 d0 = Dist2D(m_pos, next);
    if (d0 < 0.01f || range <= 0.0f) return;
    // 少し行き過ぎたところまで撃つ（Detour と同じ式）
    const f32 dist = (std::min)(d0 + 0.01f, range);
    const f32 scale = range / dist;
    const f32 goal[3] = { m_pos[0] + (next[0] - m_pos[0]) * scale, m_pos[1],
                          m_pos[2] + (next[2] - m_pos[2]) * scale };
    thread_local std::vector<i32> visited;
    visited.clear();
    const NavRaycastResult r = nav.RaycastEx(m_pos, goal, m_path.front(), &visited, 32);
    if (visited.size() > 1 && (r.status == NavRayStatus::Clear || (r.status == NavRayStatus::Hit && r.t > 0.99f)))
        MergeCorridorStartShortcut(m_path, visited);
}

f32 NavCorridor::PathLength(const NavMesh& nav) const
{
    if (m_path.empty() || !m_hasTarget) return 0.0f;
    thread_local std::vector<f32> pts;
    const i32 n = nav.FindStraightPath(m_pos, m_target, m_path.data(), static_cast<i32>(m_path.size()),
                                       pts, nullptr, 256);
    f32 len = 0.0f;
    for (i32 k = 0; k + 1 < n; ++k)
    {
        const f32* a = &pts[static_cast<size_t>(k) * 3];
        const f32* b = &pts[static_cast<size_t>(k + 1) * 3];
        len += Dist2D(a, b);
    }
    return len;
}

} // namespace nav
} // namespace dx12e
