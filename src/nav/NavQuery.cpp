// ============================================================================
// ナビメッシュ クエリ: 最近傍ポリゴン / 高さ / A* / ファネル / レイキャスト / 滑り移動
// ============================================================================
// - FindPath は A*（ポータル中点をノード位置に使う）で通るポリゴン列を出し、
//   そのあと Simple Stupid Funnel Algorithm で「通路の中の最短折れ線」へ引き直す。
//   これをやらないと経路がポリゴン中心を数珠つなぎにした不自然な形になる。
// - Raycast / MoveAlongSurface は「AABB ではない精密な当たり判定」の実体。
//   壁の実際の辺に対して交差を取り、当たったら壁に沿って滑らせる。
// ============================================================================

#include "nav/NavTypes.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace dx12e
{
namespace nav
{

namespace
{
constexpr f32 kHScale = 0.999f;

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

bool IntersectSegmentPoly2D(const f32* p0, const f32* p1,
                            const f32* verts, const u32* idx, u32 nverts,
                            f32& tmin, f32& tmax, i32& segMin, i32& segMax)
{
    constexpr f32 kEps = 1e-8f;
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
        if (std::fabs(d) < kEps) { if (n < 0.0f) return false; continue; }
        const f32 t = n / d;
        if (d < 0.0f)
        {
            if (t > tmin) { tmin = t; segMin = static_cast<i32>(j); if (tmin > tmax) return false; }
        }
        else
        {
            if (t < tmax) { tmax = t; segMax = static_cast<i32>(j); if (tmax < tmin) return false; }
        }
    }
    return true;
}

// ポータル辺 (va, vb) を「進行方向から見た左右」へ割り当てる。
// ポリゴンの巻き方向に依存しないよう、直前の位置 from を基準に判定する。
void OrderPortal(const f32* from, const f32* va, const f32* vb, f32* outLeft, f32* outRight)
{
    if (TriArea2(from, va, vb) > 0.0f)
    {
        outLeft[0] = va[0]; outLeft[1] = va[1]; outLeft[2] = va[2];
        outRight[0] = vb[0]; outRight[1] = vb[1]; outRight[2] = vb[2];
    }
    else
    {
        outLeft[0] = vb[0]; outLeft[1] = vb[1]; outLeft[2] = vb[2];
        outRight[0] = va[0]; outRight[1] = va[1]; outRight[2] = va[2];
    }
}
} // namespace

// ---------------------------------------------------------------------------
i32 NavMesh::FindNearestPoly(const f32 pos[3], const f32 ext[3], f32 outPos[3]) const
{
    if (m_polys.empty() || m_gw <= 0) return -1;

    const i32 rx = (std::max)(0, static_cast<i32>(std::ceil(ext[0] / m_cs)));
    const i32 rz = (std::max)(0, static_cast<i32>(std::ceil(ext[2] / m_cs)));
    i32 cx = 0, cz = 0;
    CellIndex(pos[0], pos[2], cx, cz);   // 範囲外でも cx/cz は入る

    f32 bestD = FLT_MAX;
    i32 bestPoly = -1;
    f32 bestY = pos[1];
    i32 bestCx = cx, bestCz = cz;

    for (i32 z = cz - rz; z <= cz + rz; ++z)
    {
        if (z < 0 || z >= m_gh) continue;
        for (i32 x = cx - rx; x <= cx + rx; ++x)
        {
            if (x < 0 || x >= m_gw) continue;
            const NavGridCell& gc = m_grid[static_cast<size_t>(x) + static_cast<size_t>(z) * m_gw];
            if (gc.first < 0) continue;

            // セル中心とのxz距離（同じセルなら 0）
            const f32 ccx = m_bmin[0] + (static_cast<f32>(x) + 0.5f) * m_cs;
            const f32 ccz = m_bmin[2] + (static_cast<f32>(z) + 0.5f) * m_cs;
            const f32 dxz = (x == cx && z == cz)
                          ? 0.0f
                          : std::sqrt((ccx - pos[0]) * (ccx - pos[0]) + (ccz - pos[2]) * (ccz - pos[2]));
            if (dxz > (std::max)(ext[0], ext[2])) continue;

            for (i32 s = gc.first; s < gc.first + gc.count; ++s)
            {
                const NavHeightSample& hs = m_samples[static_cast<size_t>(s)];
                const f32 sy = m_bmin[1] + static_cast<f32>(hs.y) * m_ch;
                const f32 dy = std::fabs(sy - pos[1]);
                if (dy > ext[1]) continue;
                // 高さのズレを優先して評価する（真上/真下の階を取り違えないため）
                const f32 d = dy * 4.0f + dxz;
                if (d < bestD)
                {
                    bestD = d; bestPoly = hs.poly; bestY = sy; bestCx = x; bestCz = z;
                }
            }
        }
    }

    if (bestPoly < 0) return -1;
    if (outPos)
    {
        if (bestCx == cx && bestCz == cz) { outPos[0] = pos[0]; outPos[2] = pos[2]; }
        else
        {
            outPos[0] = m_bmin[0] + (static_cast<f32>(bestCx) + 0.5f) * m_cs;
            outPos[2] = m_bmin[2] + (static_cast<f32>(bestCz) + 0.5f) * m_cs;
        }
        outPos[1] = bestY;
    }
    return bestPoly;
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
    bool operator<(const OpenEntry& o) const { return total > o.total; }   // 小さい順
};
} // namespace

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

    const i32 npoly = static_cast<i32>(m_polys.size());
    std::vector<i32> nodeIdx(static_cast<size_t>(npoly), -1);
    std::vector<u8>  closed(static_cast<size_t>(npoly), 0);
    std::vector<AStarNode> nodes;
    nodes.reserve(256);
    std::priority_queue<OpenEntry> open;

    {
        AStarNode n;
        n.poly = startPoly; n.parent = -1; n.cost = 0.0f;
        n.pos[0] = sPos[0]; n.pos[1] = sPos[1]; n.pos[2] = sPos[2];
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
        if (++guard > 200000) break;
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
                n.pos[0] = mid[0]; n.pos[1] = mid[1]; n.pos[2] = mid[2];
                idx = static_cast<i32>(nodes.size());
                nodes.push_back(n);
                nodeIdx[static_cast<size_t>(np)] = idx;
            }
            else
            {
                if (total >= nodes[static_cast<size_t>(idx)].total) continue;
                AStarNode& n = nodes[static_cast<size_t>(idx)];
                n.parent = ni; n.cost = cost; n.total = total;
                n.pos[0] = mid[0]; n.pos[1] = mid[1]; n.pos[2] = mid[2];
            }
            open.push({ total, idx });
            if (h < lastBestH) { lastBestH = h; lastBest = idx; }
        }
    }

    // ---- ポリゴン列を復元 ----
    std::vector<i32> polyPath;
    for (i32 n = lastBest; n != -1; n = nodes[static_cast<size_t>(n)].parent)
        polyPath.push_back(nodes[static_cast<size_t>(n)].poly);
    std::reverse(polyPath.begin(), polyPath.end());
    if (polyPath.empty()) return 0;

    // 到達できなかった場合は「一番近いところまで」を返す（追いかけゲームでは必須）
    f32 goal[3] = { ePos[0], ePos[1], ePos[2] };
    if (!found)
    {
        const NavPoly& lp = m_polys[static_cast<size_t>(polyPath.back())];
        goal[0] = lp.center[0]; goal[1] = lp.center[1]; goal[2] = lp.center[2];
    }

    // ---- ポータル列を作る ----
    std::vector<f32> portals;   // left(3) + right(3) の並び
    portals.reserve((polyPath.size() + 1) * 6);
    portals.insert(portals.end(), { sPos[0], sPos[1], sPos[2], sPos[0], sPos[1], sPos[2] });

    f32 from[3] = { sPos[0], sPos[1], sPos[2] };
    for (size_t k = 0; k + 1 < polyPath.size(); ++k)
    {
        const NavPoly& a = m_polys[static_cast<size_t>(polyPath[k])];
        const i32 bIdx = polyPath[k + 1];
        i32 edge = -1;
        for (u32 e = 0; e < a.vertCount; ++e)
            if (m_neis[a.firstNei + e] == static_cast<u32>(bIdx)) { edge = static_cast<i32>(e); break; }
        if (edge < 0) continue;

        f32 va[3], vb[3], l[3], r[3];
        GetPolyVert(a, static_cast<u32>(edge), va);
        GetPolyVert(a, static_cast<u32>((edge + 1) % static_cast<i32>(a.vertCount)), vb);
        OrderPortal(from, va, vb, l, r);
        portals.insert(portals.end(), { l[0], l[1], l[2], r[0], r[1], r[2] });
        from[0] = (va[0] + vb[0]) * 0.5f;
        from[1] = (va[1] + vb[1]) * 0.5f;
        from[2] = (va[2] + vb[2]) * 0.5f;
    }
    portals.insert(portals.end(), { goal[0], goal[1], goal[2], goal[0], goal[1], goal[2] });

    // ---- Simple Stupid Funnel Algorithm ----
    const i32 nportals = static_cast<i32>(portals.size() / 6);
    std::vector<f32> pts;
    pts.reserve(static_cast<size_t>(maxPoints) * 3);

    f32 apex[3]  = { portals[0], portals[1], portals[2] };
    f32 pleft[3] = { portals[0], portals[1], portals[2] };
    f32 pright[3]= { portals[3], portals[4], portals[5] };
    i32 apexIdx = 0, leftIdx = 0, rightIdx = 0;
    pts.insert(pts.end(), { apex[0], apex[1], apex[2] });

    for (i32 i = 1; i < nportals && static_cast<i32>(pts.size() / 3) < maxPoints; ++i)
    {
        const f32* left  = &portals[static_cast<size_t>(i) * 6 + 0];
        const f32* right = &portals[static_cast<size_t>(i) * 6 + 3];

        if (TriArea2(apex, pright, right) <= 0.0f)
        {
            if (VEqual2D(apex, pright) || TriArea2(apex, pleft, right) > 0.0f)
            {
                pright[0] = right[0]; pright[1] = right[1]; pright[2] = right[2];
                rightIdx = i;
            }
            else
            {
                pts.insert(pts.end(), { pleft[0], pleft[1], pleft[2] });
                apex[0] = pleft[0]; apex[1] = pleft[1]; apex[2] = pleft[2];
                apexIdx = leftIdx;
                pleft[0] = apex[0]; pleft[1] = apex[1]; pleft[2] = apex[2];
                pright[0] = apex[0]; pright[1] = apex[1]; pright[2] = apex[2];
                leftIdx = apexIdx; rightIdx = apexIdx;
                i = apexIdx;
                continue;
            }
        }
        if (TriArea2(apex, pleft, left) >= 0.0f)
        {
            if (VEqual2D(apex, pleft) || TriArea2(apex, pright, left) < 0.0f)
            {
                pleft[0] = left[0]; pleft[1] = left[1]; pleft[2] = left[2];
                leftIdx = i;
            }
            else
            {
                pts.insert(pts.end(), { pright[0], pright[1], pright[2] });
                apex[0] = pright[0]; apex[1] = pright[1]; apex[2] = pright[2];
                apexIdx = rightIdx;
                pleft[0] = apex[0]; pleft[1] = apex[1]; pleft[2] = apex[2];
                pright[0] = apex[0]; pright[1] = apex[1]; pright[2] = apex[2];
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
        pts.insert(pts.end(), { goal[0], goal[1], goal[2] });

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

    outPath.swap(pts);
    return static_cast<i32>(outPath.size() / 3);
}

// ---------------------------------------------------------------------------
bool NavMesh::Raycast(const f32 start[3], const f32 end[3], i32 startPoly,
                      f32& outT, f32 outHitNormal[3], f32 outHitPos[3]) const
{
    outT = 1.0f;
    if (outHitNormal) { outHitNormal[0] = outHitNormal[1] = outHitNormal[2] = 0.0f; }
    if (outHitPos) { outHitPos[0] = end[0]; outHitPos[1] = end[1]; outHitPos[2] = end[2]; }
    if (startPoly < 0 || startPoly >= static_cast<i32>(m_polys.size())) return false;

    i32 cur = startPoly;
    for (i32 iter = 0; iter < 256; ++iter)
    {
        const NavPoly& p = m_polys[static_cast<size_t>(cur)];
        f32 tmin = 0.0f, tmax = 1.0f;
        i32 segMin = -1, segMax = -1;
        if (!IntersectSegmentPoly2D(start, end, m_verts.data(), &m_polyVerts[p.firstVert],
                                    p.vertCount, tmin, tmax, segMin, segMax))
            return false;                            // ポリゴンの外から始まった
        if (segMax == -1) { outT = 1.0f; return false; }   // 終点はこのポリゴン内

        const u32 nei = m_neis[p.firstNei + static_cast<u32>(segMax)];
        if (nei == 0xffffffffu)
        {
            // 壁に当たった
            outT = tmax;
            f32 va[3], vb[3];
            GetPolyVert(p, static_cast<u32>(segMax), va);
            GetPolyVert(p, static_cast<u32>((segMax + 1) % static_cast<i32>(p.vertCount)), vb);
            const f32 dx = vb[0] - va[0], dz = vb[2] - va[2];
            const f32 len = std::sqrt(dx * dx + dz * dz);
            if (outHitNormal && len > 1e-8f)
            { outHitNormal[0] = dz / len; outHitNormal[1] = 0.0f; outHitNormal[2] = -dx / len; }
            if (outHitPos)
            {
                outHitPos[0] = start[0] + (end[0] - start[0]) * tmax;
                outHitPos[1] = start[1] + (end[1] - start[1]) * tmax;
                outHitPos[2] = start[2] + (end[2] - start[2]) * tmax;
            }
            return true;
        }
        cur = static_cast<i32>(nei);
    }
    return false;
}

bool NavMesh::MoveAlongSurface(const f32 start[3], const f32 end[3], i32 startPoly,
                               f32 outPos[3], i32& outPoly) const
{
    outPoly = startPoly;
    outPos[0] = start[0]; outPos[1] = start[1]; outPos[2] = start[2];
    if (startPoly < 0 || startPoly >= static_cast<i32>(m_polys.size())) return false;

    f32 from[3] = { start[0], start[1], start[2] };
    f32 to[3]   = { end[0], end[1], end[2] };

    // 壁に当たったら壁方向へ投影して滑る。2 回まで（角に嵌るのを防ぐ）
    for (i32 iter = 0; iter < 2; ++iter)
    {
        f32 t = 1.0f, n[3] = { 0, 0, 0 }, hit[3];
        i32 poly = startPoly;
        {
            const f32 ext[3] = { m_cs * 2.0f, m_cfg.agentHeight, m_cs * 2.0f };
            f32 tmp[3];
            const i32 pp = FindNearestPoly(from, ext, tmp);
            if (pp >= 0) poly = pp;
        }
        if (!Raycast(from, to, poly, t, n, hit))
        {
            outPos[0] = to[0]; outPos[2] = to[2];
            const f32 ext[3] = { m_cs * 2.0f, m_cfg.agentHeight, m_cs * 2.0f };
            f32 tmp[3];
            const i32 pp = FindNearestPoly(outPos, ext, tmp);
            if (pp >= 0) { outPoly = pp; f32 y; if (GetHeightAt(outPos, pp, y)) outPos[1] = y; }
            return true;
        }

        // 壁の手前で止め、残りの移動を壁方向へ落とす
        const f32 back = 0.01f;
        from[0] = hit[0] - (to[0] - from[0]) * back;
        from[2] = hit[2] - (to[2] - from[2]) * back;
        const f32 remX = to[0] - hit[0];
        const f32 remZ = to[2] - hit[2];
        const f32 dot = remX * n[0] + remZ * n[2];
        to[0] = hit[0] + (remX - n[0] * dot);
        to[2] = hit[2] + (remZ - n[2] * dot);
    }

    outPos[0] = from[0]; outPos[2] = from[2];
    const f32 ext[3] = { m_cs * 2.0f, m_cfg.agentHeight, m_cs * 2.0f };
    f32 tmp[3];
    const i32 pp = FindNearestPoly(outPos, ext, tmp);
    if (pp >= 0) { outPoly = pp; f32 y; if (GetHeightAt(outPos, pp, y)) outPos[1] = y; }
    return true;
}

} // namespace nav
} // namespace dx12e
