// ============================================================================
// ナビメッシュ クエリ: 最近傍ポリゴン / 高さ / A* / ファネル / レイキャスト / 滑り移動
// ============================================================================
// - FindPath は A*（ポータル中点をノード位置に使う）で通るポリゴン列を出し、
//   そのあと Simple Stupid Funnel Algorithm で「通路の中の最短折れ線」へ引き直す。
//   これをやらないと経路がポリゴン中心を数珠つなぎにした不自然な形になる。
//   A* とファネルは FindPolyPath / FindStraightPath に分けてあり、通路（NavCorridor）が
//   それぞれを単独で使う（毎フレームのファネルだけを引き直して、A* は必要な時だけ）。
// - RaycastEx / MoveAlongSurface は「AABB ではない精密な当たり判定」の実体。
//   壁の実際の辺に対して交差を取り、当たったら壁に沿って滑らせる。
//
// ★2026-09 に直した欠陥（MikuChase.lua がコメントで回避策を書いていたもの）:
//   【A】FindNearestPoly が「点を含まないポリゴン」を返すことがあった。
//        高さサンプル格子の縁のセルは「その領域の先頭ポリゴン」で代用して登録されていて、
//        それをそのまま返していた。始点を含まないポリゴンからレイを撃つと 1 歩目の交差判定が
//        失敗し、壁が 9m 先にあっても hit=false になっていた（Raycast の偽陰性の正体）。
//        → ポリゴンの AABB を粗いバケットに登録し、候補の中から【点を含むもの】を選ぶ。
//   【B】FindNearestPoly の outPos が隣のセルの中心へ飛んでいた（nav.sample のガタつき）。
//        → そのポリゴン上の最近点を返す。
//   【C】Raycast の false が「通れた / 始点が外 / 打ち切り」の 3 つに潰れていた。
//        → RaycastEx が NavRayStatus で区別して返す。打ち切り上限も 256 → ポリゴン総数。
//   【D】MoveAlongSurface の着地点が辺の真上になり、次の呼び出しで「始点が外」になっていた。
//        → Detour の moveAlongSurface と同じ方式（始点から届くポリゴンだけを辿って
//          終点に最も近い点を取る）に置き換え、着地点を辺から kEdgeInset だけ内側へ寄せる。
// ============================================================================

#include "nav/NavTypes.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <queue>

namespace dx12e
{
namespace nav
{

namespace
{
constexpr f32 kHScale = 0.999f;
// moveAlong の着地点を辺から内側へ寄せる量（m）。★0 にすると【D】が再発する
constexpr f32 kEdgeInset = 0.005f;

inline f32 Dist2D(const f32* a, const f32* b)
{
    const f32 dx = b[0] - a[0], dz = b[2] - a[2];
    return std::sqrt(dx * dx + dz * dz);
}
inline f32 Dist3D(const f32* a, const f32* b)
{
    const f32 dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
// 2D 外積（xz）。> 0 で c は a→b の右（時計回り）側。
inline f32 TriArea2(const f32* a, const f32* b, const f32* c)
{
    const f32 ax = b[0] - a[0], az = b[2] - a[2];
    const f32 bx = c[0] - a[0], bz = c[2] - a[2];
    return bx * az - ax * bz;
}
inline bool VEqual2D(const f32* a, const f32* b)
{
    const f32 dx = a[0] - b[0], dz = a[2] - b[2];
    return (dx * dx + dz * dz) < 0.001f * 0.001f;
}
inline f32 Perp2D(const f32* u, const f32* v) { return u[2] * v[0] - u[0] * v[2]; }
inline void Copy3(f32* d, const f32* s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }

// 点 pt と線分 p-q の xz 距離の二乗。t に線分上の位置（0..1）を返す。
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

// 線分 p0-p1 と凸ポリゴンの交差区間 [tmin, tmax]。
// ★数値誤差の許容を入れてある（始点がちょうど辺の上にあると旧実装は false を返していた）。
bool IntersectSegmentPoly2D(const f32* p0, const f32* p1,
                            const f32* verts, const u32* idx, u32 nverts,
                            f32& tmin, f32& tmax, i32& segMin, i32& segMax)
{
    constexpr f32 kEps = 1e-8f;
    constexpr f32 kSlackT = 1e-5f;
    tmin = 0.0f; tmax = 1.0f; segMin = -1; segMax = -1;
    const f32 dir[3] = { p1[0] - p0[0], 0.0f, p1[2] - p0[2] };

    for (u32 i = 0, j = nverts - 1; i < nverts; j = i++)
    {
        const f32* vi = &verts[idx[i] * 3];
        const f32* vj = &verts[idx[j] * 3];
        const f32 edge[3] = { vi[0] - vj[0], 0.0f, vi[2] - vj[2] };
        const f32 diff[3] = { p0[0] - vj[0], 0.0f, p0[2] - vj[2] };
        const f32 n = Perp2D(edge, diff);
        const f32 d = Perp2D(dir, edge);
        if (std::fabs(d) < kEps)
        {
            // 辺と平行。外側にあれば交わらない（辺の長さに比例した僅かな誤差は許す）
            const f32 elen = std::sqrt(edge[0] * edge[0] + edge[2] * edge[2]);
            if (n < -1e-5f * (elen + 1.0f)) return false;
            continue;
        }
        const f32 t = n / d;
        if (d < 0.0f)
        {
            if (t > tmin) { tmin = t; segMin = static_cast<i32>(j); if (tmin > tmax + kSlackT) return false; }
        }
        else
        {
            if (t < tmax) { tmax = t; segMax = static_cast<i32>(j); if (tmax < tmin - kSlackT) return false; }
        }
    }
    if (tmax < tmin) tmax = tmin;
    return true;
}

// ポータル辺 (va, vb) を「進行方向から見た左右」へ割り当てる。
// from はポータルの手前のポリゴンの【中心】（凸なので必ず内側＝向きが安定する）。
void OrderPortal(const f32* from, const f32* va, const f32* vb, f32* outLeft, f32* outRight)
{
    if (TriArea2(from, va, vb) > 0.0f)
    {
        Copy3(outLeft, va);
        Copy3(outRight, vb);
    }
    else
    {
        Copy3(outLeft, vb);
        Copy3(outRight, va);
    }
}
} // namespace

const char* NavRayStatusName(NavRayStatus s)
{
    switch (s)
    {
    case NavRayStatus::Clear:        return "clear";
    case NavRayStatus::Hit:          return "hit";
    case NavRayStatus::StartOffMesh: return "startOffMesh";
    case NavRayStatus::Truncated:    return "truncated";
    }
    return "unknown";
}

const char* NavPathStatusName(NavPathStatus s)
{
    switch (s)
    {
    case NavPathStatus::Failed:   return "failed";
    case NavPathStatus::Complete: return "complete";
    case NavPathStatus::Partial:  return "partial";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 幾何の小道具
// ---------------------------------------------------------------------------
namespace
{
// ポリゴンの巻き方向（xz の符号付き面積の符号）。+1 / -1
f32 PolyWinding(const f32* verts, const u32* idx, u32 nv)
{
    f32 area = 0.0f;
    for (u32 i = 0, j = nv - 1; i < nv; j = i++)
    {
        const f32* vi = &verts[idx[i] * 3];
        const f32* vj = &verts[idx[j] * 3];
        area += vj[0] * vi[2] - vi[0] * vj[2];
    }
    return area >= 0.0f ? 1.0f : -1.0f;
}

// 各辺までの符号付き距離（内側が正）の最小値と、その辺番号。
// neis を渡すと壁の辺（隣が無い辺）だけを見る（壁が無ければ FLT_MAX）。
f32 MinSignedEdgeDist(const f32* verts, const u32* idx, u32 nv, const f32* pos, i32& outEdge,
                      const u32* neis = nullptr)
{
    const f32 w = PolyWinding(verts, idx, nv);
    f32 best = FLT_MAX;
    outEdge = -1;
    for (u32 i = 0, j = nv - 1; i < nv; j = i++)
    {
        if (neis && neis[j] != 0xffffffffu) continue;
        const f32* vi = &verts[idx[i] * 3];
        const f32* vj = &verts[idx[j] * 3];
        const f32 ex = vi[0] - vj[0], ez = vi[2] - vj[2];
        const f32 len = std::sqrt(ex * ex + ez * ez);
        if (len < 1e-6f) continue;
        // (-ez, ex) 側が cross > 0。巻き方向を掛けて「内側が正」にそろえる
        const f32 cross = ex * (pos[2] - vj[2]) - ez * (pos[0] - vj[0]);
        const f32 sd = w * cross / len;
        if (sd < best) { best = sd; outEdge = static_cast<i32>(j); }
    }
    return best;
}
} // namespace

bool NavMesh::PointInPoly(i32 poly, const f32 pos[3], f32 tol) const
{
    if (!IsValidPoly(poly)) return false;
    const NavPoly& p = m_polys[static_cast<size_t>(poly)];
    i32 e = -1;
    return MinSignedEdgeDist(m_verts.data(), &m_polyVerts[p.firstVert], p.vertCount, pos, e) >= -tol;
}

void NavMesh::EdgeInwardNormal(i32 poly, u32 edge, f32 outN[3]) const
{
    outN[0] = outN[1] = outN[2] = 0.0f;
    if (!IsValidPoly(poly)) return;
    const NavPoly& p = m_polys[static_cast<size_t>(poly)];
    if (edge >= p.vertCount) return;
    f32 va[3], vb[3];
    GetPolyVert(p, edge, va);
    GetPolyVert(p, (edge + 1) % p.vertCount, vb);
    const f32 ex = vb[0] - va[0], ez = vb[2] - va[2];
    const f32 len = std::sqrt(ex * ex + ez * ez);
    if (len < 1e-6f) return;
    const f32 w = PolyWinding(m_verts.data(), &m_polyVerts[p.firstVert], p.vertCount);
    outN[0] = -ez / len * w;
    outN[2] =  ex / len * w;
}

f32 NavMesh::ClosestPointOnPoly(i32 poly, const f32 pos[3], f32 out[3]) const
{
    Copy3(out, pos);
    if (!IsValidPoly(poly)) return FLT_MAX;
    const NavPoly& p = m_polys[static_cast<size_t>(poly)];
    f32 dist = 0.0f;
    if (!PointInPoly(poly, pos, 0.0f))
    {
        f32 best = FLT_MAX;
        for (u32 i = 0, j = p.vertCount - 1; i < p.vertCount; j = i++)
        {
            f32 vj[3], vi[3], t = 0.0f;
            GetPolyVert(p, j, vj);
            GetPolyVert(p, i, vi);
            const f32 d = DistPtSegSqr2D(pos, vj, vi, t);
            if (d < best)
            {
                best = d;
                out[0] = vj[0] + (vi[0] - vj[0]) * t;
                out[2] = vj[2] + (vi[2] - vj[2]) * t;
            }
        }
        dist = std::sqrt(best);
    }
    f32 y = out[1];
    if (GetHeightAt(out, poly, y)) out[1] = y;
    return dist;
}

// ★見るのは【壁の辺】（隣が無い辺）からの距離だけ。ポータル（隣のポリゴンへ続く辺）の上に
//   載っていても両側のポリゴンの内側なので問題にならない。守るのは「ポリゴンの内側」と
//   「壁から eps 以上」の 2 つ。
// ★鋭角の頂点の近くでは、片方の壁から垂直に押すともう片方の壁を突き抜ける（内向き法線の内積が負）。
//   垂直押しで収まらなければ中心へ向かう線分上の「条件を満たす一番手前の点」を二分探索で取る
//   （条件はどちらも凹関数の上位集合＝区間なので二分探索できる）。
void NavMesh::NudgeInside(i32 poly, f32 pos[3], f32 eps) const
{
    if (!IsValidPoly(poly)) return;
    const NavPoly& p = m_polys[static_cast<size_t>(poly)];
    const f32* verts = m_verts.data();
    const u32* idx = &m_polyVerts[p.firstVert];
    const u32* neis = &m_neis[p.firstNei];
    const u32 nv = p.vertCount;

    auto inside = [&](const f32* q) { i32 e = -1; return MinSignedEdgeDist(verts, idx, nv, q, e) >= 0.0f; };
    auto wallClear = [&](const f32* q, i32& e) { return MinSignedEdgeDist(verts, idx, nv, q, e, neis); };

    // 目標の余白: eps。ただし中心の余白の半分を超えない（細いポリゴンで無理をしない）
    i32 e = -1;
    const f32 centerClear = wallClear(p.center, e);
    const f32 target = (centerClear == FLT_MAX) ? 0.0f : (std::min)(eps, centerClear * 0.5f);
    auto ok = [&](const f32* q) { i32 ee = -1; return inside(q) && wallClear(q, ee) >= target * 0.999f; };
    if (ok(pos)) return;

    // 1) 一番近い壁から垂直に押す（普通の辺ぎわはこれで収まる）
    f32 q[3] = { pos[0], pos[1], pos[2] };
    for (i32 it = 0; it < 3; ++it)
    {
        const f32 d = wallClear(q, e);
        if (d >= target || e < 0) break;
        f32 n[3];
        EdgeInwardNormal(poly, static_cast<u32>(e), n);
        q[0] += n[0] * (target - d);
        q[2] += n[2] * (target - d);
    }
    if (ok(q)) { pos[0] = q[0]; pos[2] = q[2]; return; }

    // 2) 中心へ向かう線分上で、条件を満たす一番手前の点
    f32 lo = 0.0f, hi = 1.0f;
    for (i32 it = 0; it < 24; ++it)
    {
        const f32 mid = (lo + hi) * 0.5f;
        const f32 m[3] = { pos[0] + (p.center[0] - pos[0]) * mid, pos[1], pos[2] + (p.center[2] - pos[2]) * mid };
        if (ok(m)) hi = mid; else lo = mid;
    }
    pos[0] += (p.center[0] - pos[0]) * hi;
    pos[2] += (p.center[2] - pos[2]) * hi;
}

// 着地点を壁から eps だけ垂直に押し出す（MoveAlongSurface の着地処理）。
// 押した点が同じポリゴンの内側ならそれ、ポータルの向こうの隣ポリゴンに入ったならそちらを採る。
// どちらでもなければ（壁の端を回り込む頂点ちょうど等）何もしない＝辺の上のまま返す。
// ★ここで中心の方へ引き戻すと、細い三角形が扇状に集まる頂点（壁の端）で角を回れずに
//   その場で足踏みする（通路の歩行テストで実際に起きた）。辺の上に載っていても、今の
//   FindNearestPoly / RaycastEx / MoveAlongSurface は誤差を許すので害は無い。
bool NavMesh::NudgeFromWalls(i32& poly, f32 pos[3], f32 eps) const
{
    if (!IsValidPoly(poly)) return false;
    const NavPoly& p = m_polys[static_cast<size_t>(poly)];
    const f32* verts = m_verts.data();
    const u32* neis = &m_neis[p.firstNei];
    auto wallClear = [&](i32 pi, const f32* q)
    {
        const NavPoly& pp = m_polys[static_cast<size_t>(pi)];
        i32 e = -1;
        return MinSignedEdgeDist(verts, &m_polyVerts[pp.firstVert], pp.vertCount, q, e, &m_neis[pp.firstNei]);
    };
    if (PointInPoly(poly, pos, 0.0f) && wallClear(poly, pos) >= eps) return true;

    f32 q[3] = { pos[0], pos[1], pos[2] };
    for (i32 it = 0; it < 3; ++it)
    {
        i32 e = -1;
        const f32 d = MinSignedEdgeDist(verts, &m_polyVerts[p.firstVert], p.vertCount, q, e, neis);
        if (d >= eps || e < 0) break;
        f32 n[3];
        EdgeInwardNormal(poly, static_cast<u32>(e), n);
        q[0] += n[0] * (eps - d);
        q[2] += n[2] * (eps - d);
    }
    if (PointInPoly(poly, q, 0.0f) && wallClear(poly, q) >= eps * 0.999f)
    {
        pos[0] = q[0];
        pos[2] = q[2];
        return true;
    }
    for (u32 k = 0; k < p.vertCount; ++k)
    {
        if (neis[k] == 0xffffffffu) continue;
        const i32 np = static_cast<i32>(neis[k]);
        if (!IsValidPoly(np) || !PointInPoly(np, q, 0.0f)) continue;
        if (wallClear(np, q) < eps * 0.999f) continue;
        poly = np;
        pos[0] = q[0];
        pos[2] = q[2];
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
i32 NavMesh::FindNearestPoly(const f32 pos[3], const f32 ext[3], f32 outPos[3]) const
{
    if (m_polys.empty() || m_bw <= 0) return -1;

    const f32 rxz = (std::max)(ext[0], ext[2]);
    const i32 x0 = static_cast<i32>(std::floor((pos[0] - ext[0] - m_bucketOrigin[0]) / m_bucketSize));
    const i32 z0 = static_cast<i32>(std::floor((pos[2] - ext[2] - m_bucketOrigin[1]) / m_bucketSize));
    const i32 x1 = static_cast<i32>(std::floor((pos[0] + ext[0] - m_bucketOrigin[0]) / m_bucketSize));
    const i32 z1 = static_cast<i32>(std::floor((pos[2] + ext[2] - m_bucketOrigin[1]) / m_bucketSize));
    if (x1 < 0 || z1 < 0 || x0 >= m_bw || z0 >= m_bh) return -1;

    // 候補（重複あり）→ 並べて一意に。ポリゴン番号順に評価する＝同点のときの選び方が決定論的
    thread_local std::vector<u32> cand;
    cand.clear();
    for (i32 z = (std::max)(0, z0); z <= (std::min)(m_bh - 1, z1); ++z)
        for (i32 x = (std::max)(0, x0); x <= (std::min)(m_bw - 1, x1); ++x)
        {
            const size_t b = static_cast<size_t>(x + z * m_bw);
            for (u32 k = m_bucketStart[b]; k < m_bucketStart[b + 1]; ++k)
            {
                const u32 p = m_bucketPolys[k];
                const f32* bb = &m_polyBounds[static_cast<size_t>(p) * 6];
                if (bb[0] > pos[0] + ext[0] || bb[3] < pos[0] - ext[0]) continue;
                if (bb[2] > pos[2] + ext[2] || bb[5] < pos[2] - ext[2]) continue;
                if (bb[1] > pos[1] + ext[1] || bb[4] < pos[1] - ext[1]) continue;
                cand.push_back(p);
            }
        }
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

    i32 best = -1;
    f32 bestScore = FLT_MAX;
    f32 bestPos[3]{ pos[0], pos[1], pos[2] };
    for (const u32 pu : cand)
    {
        const i32 p = static_cast<i32>(pu);
        f32 cp[3];
        const f32 dxz = ClosestPointOnPoly(p, pos, cp);   // cp[1] はナビ面の高さ
        if (dxz > rxz) continue;
        const f32 dy = std::fabs(cp[1] - pos[1]);
        if (dy > ext[1]) continue;
        // 高さのズレを優先して評価する（真上/真下の階を取り違えないため）
        const f32 score = dy * 4.0f + dxz;
        if (score < bestScore)
        {
            bestScore = score;
            best = p;
            Copy3(bestPos, cp);
        }
    }
    if (best < 0) return -1;
    if (outPos) Copy3(outPos, bestPos);
    return best;
}

// ---------------------------------------------------------------------------
bool NavMesh::GetHeightAt(const f32 pos[3], i32 poly, f32& outY) const
{
    if (poly < 0 || poly >= static_cast<i32>(m_polys.size())) return false;

    // 4 近傍セルの高さサンプルを双線形で混ぜる＝坂道が階段状にならない
    const f32 fx = (pos[0] - m_bmin[0]) / m_cs - 0.5f;
    const f32 fz = (pos[2] - m_bmin[2]) / m_cs - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(fx));
    const i32 z0 = static_cast<i32>(std::floor(fz));
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 tz = fz - static_cast<f32>(z0);

    f32 acc = 0.0f, wsum = 0.0f;
    for (i32 dz = 0; dz < 2; ++dz)
        for (i32 dx = 0; dx < 2; ++dx)
        {
            const i32 x = x0 + dx, z = z0 + dz;
            if (x < 0 || z < 0 || x >= m_gw || z >= m_gh) continue;
            const NavGridCell& gc = m_grid[static_cast<size_t>(x) + static_cast<size_t>(z) * m_gw];
            if (gc.first < 0) continue;
            for (i32 s = gc.first; s < gc.first + gc.count; ++s)
            {
                if (m_samples[static_cast<size_t>(s)].poly != static_cast<u16>(poly)) continue;
                const f32 w = (dx ? tx : 1.0f - tx) * (dz ? tz : 1.0f - tz);
                acc += w * (m_bmin[1] + static_cast<f32>(m_samples[static_cast<size_t>(s)].y) * m_ch);
                wsum += w;
                break;
            }
        }
    if (wsum > 1e-6f) { outY = acc / wsum; return true; }

    // 格子に無い（ポリゴンの縁）→ ポリゴンの扇状三角形分割で重心座標補間
    const NavPoly& p = m_polys[static_cast<size_t>(poly)];
    f32 v0[3], v1[3], v2[3];
    GetPolyVert(p, 0, v0);
    for (u32 i = 1; i + 1 < p.vertCount; ++i)
    {
        GetPolyVert(p, i, v1);
        GetPolyVert(p, i + 1, v2);
        const f32 d = (v1[2] - v2[2]) * (v0[0] - v2[0]) + (v2[0] - v1[0]) * (v0[2] - v2[2]);
        if (std::fabs(d) < 1e-9f) continue;
        const f32 a = ((v1[2] - v2[2]) * (pos[0] - v2[0]) + (v2[0] - v1[0]) * (pos[2] - v2[2])) / d;
        const f32 b = ((v2[2] - v0[2]) * (pos[0] - v2[0]) + (v0[0] - v2[0]) * (pos[2] - v2[2])) / d;
        const f32 c = 1.0f - a - b;
        if (a >= -0.01f && b >= -0.01f && c >= -0.01f)
        {
            outY = a * v0[1] + b * v1[1] + c * v2[1];
            return true;
        }
    }
    outY = p.center[1];
    return true;
}

// ---------------------------------------------------------------------------
namespace
{
struct AStarNode
{
    i32 poly = -1;
    i32 parent = -1;
    f32 cost = 0.0f, total = 0.0f;
    f32 pos[3]{ 0, 0, 0 };
};
struct OpenEntry
{
    f32 total; i32 node;
    // 小さい順。同点はノード番号の小さい方（＝先に見つけた方）を先に出す＝決定論的
    bool operator<(const OpenEntry& o) const
    {
        if (total != o.total) return total > o.total;
        return node > o.node;
    }
};
} // namespace

NavPathStatus NavMesh::FindPolyPath(i32 startPoly, i32 endPoly, const f32 sPos[3], const f32 ePos[3],
                                    std::vector<i32>& outPolys, i32 maxNodes) const
{
    outPolys.clear();
    if (!IsValidPoly(startPoly) || !IsValidPoly(endPoly)) return NavPathStatus::Failed;
    if (startPoly == endPoly)
    {
        outPolys.push_back(startPoly);
        return NavPathStatus::Complete;
    }

    const i32 npoly = static_cast<i32>(m_polys.size());
    const i32 nodeLimit = maxNodes > 0 ? maxNodes : 200000;
    std::vector<i32> nodeIdx(static_cast<size_t>(npoly), -1);
    std::vector<u8>  closed(static_cast<size_t>(npoly), 0);
    std::vector<AStarNode> nodes;
    nodes.reserve(256);
    std::priority_queue<OpenEntry> open;

    {
        AStarNode n;
        n.poly = startPoly; n.parent = -1; n.cost = 0.0f;
        Copy3(n.pos, sPos);
        n.total = Dist3D(sPos, ePos) * kHScale;
        nodeIdx[static_cast<size_t>(startPoly)] = 0;
        nodes.push_back(n);
        open.push({ n.total, 0 });
    }

    i32 lastBest = 0;
    f32 lastBestH = nodes[0].total;
    bool found = false;
    i32 guard = 0;

    while (!open.empty())
    {
        if (++guard > nodeLimit) break;
        const OpenEntry top = open.top(); open.pop();
        const i32 ni = top.node;
        const i32 pi = nodes[static_cast<size_t>(ni)].poly;
        if (closed[static_cast<size_t>(pi)]) continue;
        closed[static_cast<size_t>(pi)] = 1;

        if (pi == endPoly) { lastBest = ni; found = true; break; }

        const NavPoly& poly = m_polys[static_cast<size_t>(pi)];
        for (u32 e = 0; e < poly.vertCount; ++e)
        {
            const u32 nei = m_neis[poly.firstNei + e];
            if (nei == 0xffffffffu) continue;
            const i32 np = static_cast<i32>(nei);
            if (np < 0 || np >= npoly || closed[static_cast<size_t>(np)]) continue;

            // ポータル辺の中点をノード位置にする（ポリゴン中心より経路が短くなる）
            f32 va[3], vb[3];
            GetPolyVert(poly, e, va);
            GetPolyVert(poly, (e + 1) % poly.vertCount, vb);
            const f32 mid[3] = { (va[0] + vb[0]) * 0.5f, (va[1] + vb[1]) * 0.5f, (va[2] + vb[2]) * 0.5f };

            const f32 cost = nodes[static_cast<size_t>(ni)].cost + Dist3D(nodes[static_cast<size_t>(ni)].pos, mid);
            const f32 h = Dist3D(mid, ePos) * kHScale;
            const f32 total = cost + h;

            i32 idx = nodeIdx[static_cast<size_t>(np)];
            if (idx == -1)
            {
                AStarNode n;
                n.poly = np; n.parent = ni; n.cost = cost; n.total = total;
                Copy3(n.pos, mid);
                idx = static_cast<i32>(nodes.size());
                nodes.push_back(n);
                nodeIdx[static_cast<size_t>(np)] = idx;
            }
            else
            {
                if (total >= nodes[static_cast<size_t>(idx)].total) continue;
                AStarNode& n = nodes[static_cast<size_t>(idx)];
                n.parent = ni; n.cost = cost; n.total = total;
                Copy3(n.pos, mid);
            }
            open.push({ total, idx });
            if (h < lastBestH) { lastBestH = h; lastBest = idx; }
        }
    }

    // ---- ポリゴン列を復元 ----
    for (i32 n = lastBest; n != -1; n = nodes[static_cast<size_t>(n)].parent)
        outPolys.push_back(nodes[static_cast<size_t>(n)].poly);
    std::reverse(outPolys.begin(), outPolys.end());
    if (outPolys.empty()) return NavPathStatus::Failed;
    return found ? NavPathStatus::Complete : NavPathStatus::Partial;
}

i32 NavMesh::FindStraightPath(const f32 startPos[3], const f32 endPos[3], const i32* polys, i32 npolys,
                              std::vector<f32>& outPts, std::vector<u8>* outFlags, i32 maxPoints) const
{
    outPts.clear();
    if (outFlags) outFlags->clear();
    if (npolys <= 0 || maxPoints <= 0) return 0;

    // ---- ポータル列を作る ----
    thread_local std::vector<f32> portals;   // left(3) + right(3) の並び
    portals.clear();
    portals.reserve((static_cast<size_t>(npolys) + 1) * 6);
    portals.insert(portals.end(), { startPos[0], startPos[1], startPos[2], startPos[0], startPos[1], startPos[2] });

    f32 goal[3];
    Copy3(goal, endPos);
    for (i32 k = 0; k + 1 < npolys; ++k)
    {
        if (!IsValidPoly(polys[k]) || !IsValidPoly(polys[k + 1])) break;
        const NavPoly& a = m_polys[static_cast<size_t>(polys[k])];
        const i32 bIdx = polys[k + 1];
        i32 edge = -1;
        for (u32 e = 0; e < a.vertCount; ++e)
            if (m_neis[a.firstNei + e] == static_cast<u32>(bIdx)) { edge = static_cast<i32>(e); break; }
        if (edge < 0)
        {
            // 通路が途切れている（焼き直し直後など）。繋がっている所までで打ち切る
            ClosestPointOnPoly(polys[k], endPos, goal);
            break;
        }

        f32 va[3], vb[3], l[3], r[3];
        GetPolyVert(a, static_cast<u32>(edge), va);
        GetPolyVert(a, static_cast<u32>((edge + 1) % static_cast<i32>(a.vertCount)), vb);
        OrderPortal(a.center, va, vb, l, r);
        portals.insert(portals.end(), { l[0], l[1], l[2], r[0], r[1], r[2] });
    }
    portals.insert(portals.end(), { goal[0], goal[1], goal[2], goal[0], goal[1], goal[2] });

    // ---- Simple Stupid Funnel Algorithm ----
    const i32 nportals = static_cast<i32>(portals.size() / 6);
    std::vector<f32>& pts = outPts;
    pts.reserve(static_cast<size_t>(maxPoints) * 3);

    f32 apex[3], pleft[3], pright[3];
    Copy3(apex, &portals[0]);
    Copy3(pleft, &portals[0]);
    Copy3(pright, &portals[3]);
    i32 apexIdx = 0, leftIdx = 0, rightIdx = 0;
    pts.insert(pts.end(), { apex[0], apex[1], apex[2] });
    if (outFlags) outFlags->push_back(kStraightPathStart);

    bool leading = true;
    for (i32 i = 1; i < nportals && static_cast<i32>(pts.size() / 3) < maxPoints; ++i)
    {
        const f32* left  = &portals[static_cast<size_t>(i) * 6 + 0];
        const f32* right = &portals[static_cast<size_t>(i) * 6 + 3];

        // 始点がポータルの上に載っているなら、そのポータルは通過済みとして飛ばす（Detour と同じ）。
        // ★これが無いと、始点と終点が同じ辺の上にある時（対角線で割った四角の上を斜めに歩く等）に
        //   左右の判定が退化し、ファネルが遠い頂点を角として吐いて大回りする。
        if (leading)
        {
            f32 t = 0.0f;
            if (i < nportals - 1 && DistPtSegSqr2D(apex, left, right, t) < 1e-6f) continue;
            leading = false;
        }

        if (TriArea2(apex, pright, right) <= 0.0f)
        {
            if (VEqual2D(apex, pright) || TriArea2(apex, pleft, right) > 0.0f)
            {
                Copy3(pright, right);
                rightIdx = i;
            }
            else
            {
                pts.insert(pts.end(), { pleft[0], pleft[1], pleft[2] });
                if (outFlags) outFlags->push_back(0);
                Copy3(apex, pleft);
                apexIdx = leftIdx;
                Copy3(pleft, apex);
                Copy3(pright, apex);
                leftIdx = apexIdx; rightIdx = apexIdx;
                i = apexIdx;
                continue;
            }
        }
        if (TriArea2(apex, pleft, left) >= 0.0f)
        {
            if (VEqual2D(apex, pleft) || TriArea2(apex, pright, left) < 0.0f)
            {
                Copy3(pleft, left);
                leftIdx = i;
            }
            else
            {
                pts.insert(pts.end(), { pright[0], pright[1], pright[2] });
                if (outFlags) outFlags->push_back(0);
                Copy3(apex, pright);
                apexIdx = rightIdx;
                Copy3(pleft, apex);
                Copy3(pright, apex);
                leftIdx = apexIdx; rightIdx = apexIdx;
                i = apexIdx;
                continue;
            }
        }
    }
    // 終点を足す。★ファネルが既に終点を吐いている場合は重複させない
    //（同じ座標が 2 つ並ぶと、追従する側が「距離 0 の区間」を踏んで止まる）。
    if (static_cast<i32>(pts.size() / 3) < maxPoints &&
        (pts.size() < 3 || !VEqual2D(&pts[pts.size() - 3], goal)))
    {
        pts.insert(pts.end(), { goal[0], goal[1], goal[2] });
        if (outFlags) outFlags->push_back(kStraightPathEnd);
    }
    else if (outFlags && !outFlags->empty() && VEqual2D(&pts[pts.size() - 3], goal))
    {
        outFlags->back() |= kStraightPathEnd;
    }

    // ---- 各点の高さをナビメッシュから引き直す（坂道で足が浮かないように）----
    const f32 hext[3] = { m_cs * 2.0f, m_cfg.agentHeight * 2.0f, m_cs * 2.0f };
    for (size_t k = 0; k + 2 < pts.size(); k += 3)
    {
        f32 tmp[3];
        const i32 pp = FindNearestPoly(&pts[k], hext, tmp);
        if (pp >= 0)
        {
            f32 y = pts[k + 1];
            if (GetHeightAt(&pts[k], pp, y)) pts[k + 1] = y;
        }
    }
    return static_cast<i32>(pts.size() / 3);
}

i32 NavMesh::FindPath(const f32 start[3], const f32 end[3], const f32 ext[3],
                      std::vector<f32>& outPath, i32 maxPoints) const
{
    outPath.clear();
    if (m_polys.empty()) return 0;

    f32 sPos[3], ePos[3];
    const i32 startPoly = FindNearestPoly(start, ext, sPos);
    const i32 endPoly   = FindNearestPoly(end,   ext, ePos);
    if (startPoly < 0 || endPoly < 0) return 0;

    if (startPoly == endPoly)
    {
        outPath = { sPos[0], sPos[1], sPos[2], ePos[0], ePos[1], ePos[2] };
        return 2;
    }

    std::vector<i32> polys;
    const NavPathStatus st = FindPolyPath(startPoly, endPoly, sPos, ePos, polys);
    if (st == NavPathStatus::Failed || polys.empty()) return 0;

    // 到達できなかった場合は「一番近いところまで」を返す（追いかけゲームでは必須）。
    // ★旧実装は最後のポリゴンの【重心】を返していた。今は最後のポリゴン上で目的地に一番近い点。
    f32 goal[3] = { ePos[0], ePos[1], ePos[2] };
    if (st == NavPathStatus::Partial) ClosestPointOnPoly(polys.back(), ePos, goal);

    return FindStraightPath(sPos, goal, polys.data(), static_cast<i32>(polys.size()),
                            outPath, nullptr, maxPoints);
}

// ---------------------------------------------------------------------------
NavRaycastResult NavMesh::RaycastEx(const f32 start[3], const f32 end[3], i32 startPoly,
                                    std::vector<i32>* visited, i32 maxVisit) const
{
    NavRaycastResult r;
    Copy3(r.hitPos, start);
    if (!IsValidPoly(startPoly)) { r.status = NavRayStatus::StartOffMesh; return r; }

    // 始点がポリゴンの内側か。誤差ぶん（セル半分）外れているだけなら内側へ寄せて撃つ。
    f32 s[3];
    Copy3(s, start);
    if (!PointInPoly(startPoly, s, 0.0f))
    {
        f32 cp[3];
        const f32 d = ClosestPointOnPoly(startPoly, s, cp);
        if (d > (std::max)(m_cs * 0.5f, 0.01f))
        {
            r.status = NavRayStatus::StartOffMesh;
            r.lastPoly = startPoly;
            return r;
        }
        cp[1] = s[1];
        NudgeInside(startPoly, cp, 1e-4f);
        Copy3(s, cp);
    }

    const i32 limit = maxVisit > 0 ? maxVisit : static_cast<i32>(m_polys.size()) + 1;
    i32 cur = startPoly;
    f32 tEnter = 0.0f;
    auto lerpTo = [&](f32 t, f32 out[3])
    {
        out[0] = s[0] + (end[0] - s[0]) * t;
        out[1] = s[1] + (end[1] - s[1]) * t;
        out[2] = s[2] + (end[2] - s[2]) * t;
    };

    for (i32 iter = 0; ; ++iter)
    {
        if (iter >= limit)
        {
            r.status = NavRayStatus::Truncated;
            r.t = tEnter;
            lerpTo(tEnter, r.hitPos);
            r.lastPoly = cur;
            return r;
        }
        if (visited) visited->push_back(cur);
        r.visitedCount = iter + 1;
        r.lastPoly = cur;

        const NavPoly& p = m_polys[static_cast<size_t>(cur)];
        f32 tmin = 0.0f, tmax = 1.0f;
        i32 segMin = -1, segMax = -1;
        if (!IntersectSegmentPoly2D(s, end, m_verts.data(), &m_polyVerts[p.firstVert],
                                    p.vertCount, tmin, tmax, segMin, segMax))
        {
            // 隣へ渡った直後に数値誤差で交差が取れなかった（頂点ちょうどを通った等）。
            // Detour と同じく「そこまで進めた所で当たった」と扱う（通れたとは言わない）
            r.status = NavRayStatus::Hit;
            r.t = tEnter;
            lerpTo(tEnter, r.hitPos);
            return r;
        }
        if (segMax == -1)
        {
            // 終点はこのポリゴン内
            r.status = NavRayStatus::Clear;
            r.t = 1.0f;
            Copy3(r.hitPos, end);
            return r;
        }

        const u32 nei = m_neis[p.firstNei + static_cast<u32>(segMax)];
        if (nei == 0xffffffffu)
        {
            // 壁に当たった
            r.status = NavRayStatus::Hit;
            r.t = tmax;
            lerpTo(tmax, r.hitPos);
            EdgeInwardNormal(cur, static_cast<u32>(segMax), r.hitNormal);
            return r;
        }
        tEnter = tmax;
        cur = static_cast<i32>(nei);
    }
}

bool NavMesh::Raycast(const f32 start[3], const f32 end[3], i32 startPoly,
                      f32& outT, f32 outHitNormal[3], f32 outHitPos[3]) const
{
    const NavRaycastResult r = RaycastEx(start, end, startPoly);
    const bool hit = (r.status == NavRayStatus::Hit);
    outT = hit ? r.t : 1.0f;
    if (outHitNormal) Copy3(outHitNormal, r.hitNormal);
    if (outHitPos) { if (hit) Copy3(outHitPos, r.hitPos); else Copy3(outHitPos, end); }
    return hit;
}

// ---------------------------------------------------------------------------
// 滑り移動（Detour の moveAlongSurface と同じ方式）
//   始点を中心に「始点と終点の中点を中心・距離の半分を半径」の円に掛かるポリゴンだけを
//   幅優先で辿り、終点を含むポリゴンがあればそこ、無ければ辿った範囲の【壁の辺の上で終点に
//   一番近い点】を取る。壁を突き抜けた先のポリゴンは辿らない（隣接でしか繋がらない）ので、
//   旧実装のように「交差判定に失敗して目標へワープ」することが原理的に無い。
// ---------------------------------------------------------------------------
bool NavMesh::MoveAlongSurface(const f32 start[3], const f32 end[3], i32 startPoly,
                               f32 outPos[3], i32& outPoly, std::vector<i32>* visited) const
{
    outPoly = startPoly;
    Copy3(outPos, start);
    if (visited) visited->clear();
    if (!IsValidPoly(startPoly)) return false;

    constexpr i32 kMaxNodes = 128;
    struct Node { i32 poly; i32 parent; };
    thread_local std::vector<Node> nodes;
    nodes.clear();
    nodes.push_back({ startPoly, -1 });

    const f32 searchPos[3] = { (start[0] + end[0]) * 0.5f, (start[1] + end[1]) * 0.5f, (start[2] + end[2]) * 0.5f };
    const f32 searchRad = Dist2D(start, end) * 0.5f + 0.001f;
    const f32 searchRadSqr = searchRad * searchRad;

    f32 bestPos[3];
    Copy3(bestPos, start);
    f32 bestDist = FLT_MAX;
    i32 bestNode = 0;
    bool reachedEnd = false;

    auto isVisited = [&](i32 poly)
    {
        for (const Node& n : nodes) if (n.poly == poly) return true;
        return false;
    };

    for (size_t head = 0; head < nodes.size(); ++head)
    {
        const i32 cur = nodes[head].poly;
        const NavPoly& p = m_polys[static_cast<size_t>(cur)];

        // 終点がこのポリゴン内なら終わり
        if (PointInPoly(cur, end, 0.0f))
        {
            bestNode = static_cast<i32>(head);
            Copy3(bestPos, end);
            reachedEnd = true;
            break;
        }

        for (u32 i = 0, j = p.vertCount - 1; i < p.vertCount; j = i++)
        {
            f32 vj[3], vi[3];
            GetPolyVert(p, j, vj);
            GetPolyVert(p, i, vi);
            const u32 nei = m_neis[p.firstNei + j];
            f32 tseg = 0.0f;
            if (nei == 0xffffffffu)
            {
                // 壁: 終点に一番近い辺上の点を候補にする
                const f32 d = DistPtSegSqr2D(end, vj, vi, tseg);
                if (d < bestDist)
                {
                    bestDist = d;
                    bestPos[0] = vj[0] + (vi[0] - vj[0]) * tseg;
                    bestPos[1] = vj[1] + (vi[1] - vj[1]) * tseg;
                    bestPos[2] = vj[2] + (vi[2] - vj[2]) * tseg;
                    bestNode = static_cast<i32>(head);
                }
            }
            else
            {
                const i32 np = static_cast<i32>(nei);
                if (!IsValidPoly(np) || isVisited(np)) continue;
                // 探索円に掛からないポータルの先は辿らない
                if (DistPtSegSqr2D(searchPos, vj, vi, tseg) > searchRadSqr) continue;
                if (static_cast<i32>(nodes.size()) >= kMaxNodes) continue;
                nodes.push_back({ np, static_cast<i32>(head) });
            }
        }
    }

    // 通ったポリゴン列（始点 → 着地点）
    if (visited)
    {
        for (i32 n = bestNode; n != -1; n = nodes[static_cast<size_t>(n)].parent)
            visited->push_back(nodes[static_cast<size_t>(n)].poly);
        std::reverse(visited->begin(), visited->end());
    }

    outPoly = nodes[static_cast<size_t>(bestNode)].poly;
    Copy3(outPos, bestPos);
    // 【D】着地点を壁の辺ちょうどに載せない（壁ぎわに着地した時・誤差で縁に乗った時）
    (void)reachedEnd;
    {
        i32 np = outPoly;
        if (NudgeFromWalls(np, outPos, kEdgeInset) && np != outPoly)
        {
            if (visited) visited->push_back(np);   // 隣へ出た（隣接なので通路の鎖は切れない）
            outPoly = np;
        }
    }
    f32 y = outPos[1];
    if (GetHeightAt(outPos, outPoly, y)) outPos[1] = y;
    return true;
}

// ---------------------------------------------------------------------------
// 周囲の壁（群衆回避用）
// ---------------------------------------------------------------------------
i32 NavMesh::FindLocalWalls(i32 startPoly, const f32 pos[3], f32 radius, std::vector<f32>& outSegs,
                            i32 maxSegs) const
{
    outSegs.clear();
    if (!IsValidPoly(startPoly) || maxSegs <= 0) return 0;

    constexpr i32 kMaxNodes = 64;
    thread_local std::vector<i32> polys;
    polys.clear();
    polys.push_back(startPoly);
    const f32 r2 = radius * radius;

    struct Seg { f32 d; i32 order; f32 p[6]; };
    thread_local std::vector<Seg> segs;
    segs.clear();

    for (size_t head = 0; head < polys.size(); ++head)
    {
        const i32 cur = polys[head];
        const NavPoly& p = m_polys[static_cast<size_t>(cur)];
        for (u32 i = 0, j = p.vertCount - 1; i < p.vertCount; j = i++)
        {
            f32 vj[3], vi[3], t = 0.0f;
            GetPolyVert(p, j, vj);
            GetPolyVert(p, i, vi);
            const f32 d = DistPtSegSqr2D(pos, vj, vi, t);
            if (d > r2) continue;
            const u32 nei = m_neis[p.firstNei + j];
            if (nei == 0xffffffffu)
            {
                // 向きをそろえる: p→q の左手（(-dz, dx) の側）が通路の内側になるように並べる
                f32 n[3];
                EdgeInwardNormal(cur, j, n);
                const f32 lx = -(vi[2] - vj[2]), lz = vi[0] - vj[0];
                if (lx * n[0] + lz * n[2] >= 0.0f)
                    segs.push_back({ d, static_cast<i32>(segs.size()),
                                     { vj[0], vj[1], vj[2], vi[0], vi[1], vi[2] } });
                else
                    segs.push_back({ d, static_cast<i32>(segs.size()),
                                     { vi[0], vi[1], vi[2], vj[0], vj[1], vj[2] } });
            }
            else
            {
                const i32 np = static_cast<i32>(nei);
                if (!IsValidPoly(np) || static_cast<i32>(polys.size()) >= kMaxNodes) continue;
                if (std::find(polys.begin(), polys.end(), np) != polys.end()) continue;
                polys.push_back(np);
            }
        }
    }
    // 近い順（同距離は見つけた順）＝決定論的
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b)
    {
        if (a.d != b.d) return a.d < b.d;
        return a.order < b.order;
    });
    const i32 n = (std::min)(maxSegs, static_cast<i32>(segs.size()));
    outSegs.reserve(static_cast<size_t>(n) * 6);
    for (i32 k = 0; k < n; ++k)
        outSegs.insert(outSegs.end(), segs[static_cast<size_t>(k)].p, segs[static_cast<size_t>(k)].p + 6);
    return n;
}

} // namespace nav
} // namespace dx12e
