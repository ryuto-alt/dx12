// ============================================================================
// ナビメッシュ 段階 6: 輪郭の抽出・単純化・穴の橋渡し
// ============================================================================
// 領域の境界を左手法則で 1 周してギザギザの生輪郭を得たあと、
// Douglas-Peucker で maxSimplificationError 以内まで頂点を間引く。
// ★ここを省くとボクセルの階段がそのまま残り、斜めの壁沿いで経路がガタつく。
//
// 領域の中に柱などがあると輪郭は「外輪郭 + 穴」になる。穴を放置すると
// ポリゴン化で柱が塗り潰されて AI が柱をすり抜けるので、必ず橋渡しして
// 1 本の輪郭へ縫い合わせる。
// ============================================================================

#include "nav/NavTypes.h"

#include <algorithm>
#include <cmath>

namespace dx12e
{
namespace nav
{

namespace
{

inline i32 Prev(i32 i, i32 n) { return i - 1 >= 0 ? i - 1 : n - 1; }
inline i32 Next(i32 i, i32 n) { return i + 1 < n ? i + 1 : 0; }

inline i32 Area2(const i32* a, const i32* b, const i32* c)
{
    return (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
}
inline bool Left(const i32* a, const i32* b, const i32* c)      { return Area2(a, b, c) < 0; }
inline bool LeftOn(const i32* a, const i32* b, const i32* c)     { return Area2(a, b, c) <= 0; }
inline bool Collinear(const i32* a, const i32* b, const i32* c)  { return Area2(a, b, c) == 0; }
inline bool XorB(bool x, bool y) { return !x ^ !y; }
inline bool VEqual(const i32* a, const i32* b) { return a[0] == b[0] && a[2] == b[2]; }

bool IntersectProp(const i32* a, const i32* b, const i32* c, const i32* d)
{
    if (Collinear(a, b, c) || Collinear(a, b, d) ||
        Collinear(c, d, a) || Collinear(c, d, b)) return false;
    return XorB(Left(a, b, c), Left(a, b, d)) && XorB(Left(c, d, a), Left(c, d, b));
}

bool Between(const i32* a, const i32* b, const i32* c)
{
    if (!Collinear(a, b, c)) return false;
    if (a[0] != b[0]) return ((a[0] <= c[0]) && (c[0] <= b[0])) || ((a[0] >= c[0]) && (c[0] >= b[0]));
    return ((a[2] <= c[2]) && (c[2] <= b[2])) || ((a[2] >= c[2]) && (c[2] >= b[2]));
}

bool Intersect(const i32* a, const i32* b, const i32* c, const i32* d)
{
    if (IntersectProp(a, b, c, d)) return true;
    return Between(a, b, c) || Between(a, b, d) || Between(c, d, a) || Between(c, d, b);
}

bool InCone(i32 i, i32 n, const i32* verts, const i32* pj)
{
    const i32* pi   = &verts[i * 4];
    const i32* pi1  = &verts[Next(i, n) * 4];
    const i32* pin1 = &verts[Prev(i, n) * 4];
    if (LeftOn(pin1, pi, pi1))
        return Left(pi, pj, pin1) && Left(pj, pi, pi1);
    return !(LeftOn(pi, pj, pi1) && LeftOn(pj, pi, pin1));
}

f32 DistancePtSeg(i32 x, i32 z, i32 px, i32 pz, i32 qx, i32 qz)
{
    const f32 pqx = static_cast<f32>(qx - px);
    const f32 pqz = static_cast<f32>(qz - pz);
    f32 dx = static_cast<f32>(x - px);
    f32 dz = static_cast<f32>(z - pz);
    const f32 d = pqx * pqx + pqz * pqz;
    f32 t = pqx * dx + pqz * dz;
    if (d > 0) t /= d;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    dx = static_cast<f32>(px) + t * pqx - static_cast<f32>(x);
    dz = static_cast<f32>(pz) + t * pqz - static_cast<f32>(z);
    return dx * dx + dz * dz;
}

// 輪郭の角の高さ。周囲 4 セルの最大値を採る（面が合流する角で床が抜けないように）
i32 GetCornerHeight(i32 x, i32 z, u32 i, i32 dir,
                    const NavCompactHeightfield& chf, bool& isBorderVertex)
{
    const NavCompactSpan& s = chf.spans[i];
    i32 ch = static_cast<i32>(s.y);
    const i32 dirp = (dir + 1) & 0x3;

    u32 regs[4] = { 0, 0, 0, 0 };
    regs[0] = static_cast<u32>(chf.spans[i].reg) | (static_cast<u32>(chf.areas[i]) << 16);

    if (GetCon(s, dir) != kNotConnected)
    {
        const i32 ax = x + kDirOffsetX[dir];
        const i32 az = z + kDirOffsetZ[dir];
        const u32 ai = chf.cells[ax + az * chf.w].index + GetCon(s, dir);
        const NavCompactSpan& as = chf.spans[ai];
        ch = (std::max)(ch, static_cast<i32>(as.y));
        regs[1] = static_cast<u32>(as.reg) | (static_cast<u32>(chf.areas[ai]) << 16);
        if (GetCon(as, dirp) != kNotConnected)
        {
            const i32 ax2 = ax + kDirOffsetX[dirp];
            const i32 az2 = az + kDirOffsetZ[dirp];
            const u32 ai2 = chf.cells[ax2 + az2 * chf.w].index + GetCon(as, dirp);
            const NavCompactSpan& as2 = chf.spans[ai2];
            ch = (std::max)(ch, static_cast<i32>(as2.y));
            regs[2] = static_cast<u32>(as2.reg) | (static_cast<u32>(chf.areas[ai2]) << 16);
        }
    }
    if (GetCon(s, dirp) != kNotConnected)
    {
        const i32 ax = x + kDirOffsetX[dirp];
        const i32 az = z + kDirOffsetZ[dirp];
        const u32 ai = chf.cells[ax + az * chf.w].index + GetCon(s, dirp);
        const NavCompactSpan& as = chf.spans[ai];
        ch = (std::max)(ch, static_cast<i32>(as.y));
        regs[3] = static_cast<u32>(as.reg) | (static_cast<u32>(chf.areas[ai]) << 16);
        if (GetCon(as, dir) != kNotConnected)
        {
            const i32 ax2 = ax + kDirOffsetX[dir];
            const i32 az2 = az + kDirOffsetZ[dir];
            const u32 ai2 = chf.cells[ax2 + az2 * chf.w].index + GetCon(as, dir);
            const NavCompactSpan& as2 = chf.spans[ai2];
            ch = (std::max)(ch, static_cast<i32>(as2.y));
            regs[2] = static_cast<u32>(as2.reg) | (static_cast<u32>(chf.areas[ai2]) << 16);
        }
    }

    for (i32 j = 0; j < 4; ++j)
    {
        const i32 a = j, b = (j + 1) & 0x3, c = (j + 2) & 0x3, d = (j + 3) & 0x3;
        const bool twoSameExts = (regs[a] & regs[b] & kBorderRegion) != 0 && regs[a] == regs[b];
        const bool twoInts     = ((regs[c] | regs[d]) & kBorderRegion) == 0;
        const bool intsSameArea = (regs[c] >> 16) == (regs[d] >> 16);
        const bool noZeros = regs[a] && regs[b] && regs[c] && regs[d];
        if (twoSameExts && twoInts && intsSameArea && noZeros) { isBorderVertex = true; break; }
    }
    return ch;
}

// 生輪郭を 1 周する（左手法則）
void WalkContourRaw(i32 x, i32 z, u32 i, const NavCompactHeightfield& chf,
                    std::vector<u8>& flags, std::vector<i32>& points)
{
    i32 dir = 0;
    while ((flags[i] & (1 << dir)) == 0) ++dir;

    const i32 startDir = dir;
    const u32 starti = i;
    const u8  area = chf.areas[i];

    i32 iter = 0;
    while (++iter < 40000)
    {
        if (flags[i] & (1 << dir))
        {
            bool isBorderVertex = false;
            bool isAreaBorder   = false;
            i32 px = x;
            const i32 py = GetCornerHeight(x, z, i, dir, chf, isBorderVertex);
            i32 pz = z;
            switch (dir)
            {
                case 0: pz++; break;
                case 1: px++; pz++; break;
                case 2: px++; break;
                default: break;
            }
            i32 r = 0;
            const NavCompactSpan& s = chf.spans[i];
            if (GetCon(s, dir) != kNotConnected)
            {
                const i32 ax = x + kDirOffsetX[dir];
                const i32 az = z + kDirOffsetZ[dir];
                const u32 ai = chf.cells[ax + az * chf.w].index + GetCon(s, dir);
                r = static_cast<i32>(chf.spans[ai].reg);
                if (area != chf.areas[ai]) isAreaBorder = true;
            }
            if (isBorderVertex) r |= static_cast<i32>(kBorderVertex);
            if (isAreaBorder)   r |= static_cast<i32>(kAreaBorder);
            points.push_back(px); points.push_back(py); points.push_back(pz); points.push_back(r);

            flags[i] = static_cast<u8>(flags[i] & ~(1 << dir));
            dir = (dir + 1) & 0x3;              // 時計回り
        }
        else
        {
            const i32 nx = x + kDirOffsetX[dir];
            const i32 nz = z + kDirOffsetZ[dir];
            const NavCompactSpan& s = chf.spans[i];
            if (GetCon(s, dir) == kNotConnected) return;   // 起きないはず
            const u32 ni = chf.cells[nx + nz * chf.w].index + GetCon(s, dir);
            x = nx; z = nz; i = ni;
            dir = (dir + 3) & 0x3;              // 反時計回り
        }
        if (starti == i && startDir == dir) break;
    }
}

void InsertPoint(std::vector<i32>& simplified, i32 at, const i32* pt, i32 rawIndex)
{
    simplified.insert(simplified.begin() + static_cast<i64>(at) * 4, { pt[0], pt[1], pt[2], rawIndex });
}

void SimplifyContour(const std::vector<i32>& points, std::vector<i32>& simplified,
                     f32 maxError, i32 maxEdgeLen)
{
    simplified.clear();
    const i32 pn = static_cast<i32>(points.size() / 4);
    if (pn < 3) return;

    bool hasConnections = false;
    for (i32 i = 0; i < pn; ++i)
        if ((points[i * 4 + 3] & static_cast<i32>(kContourRegMask)) != 0) { hasConnections = true; break; }

    if (hasConnections)
    {
        // 他の領域へ抜けるポータルの切れ目は必ず頂点として残す
        for (i32 i = 0; i < pn; ++i)
        {
            const i32 ii = (i + 1) % pn;
            const bool diffRegs = (points[i * 4 + 3] & static_cast<i32>(kContourRegMask)) !=
                                  (points[ii * 4 + 3] & static_cast<i32>(kContourRegMask));
            const bool areaBorders = (points[i * 4 + 3] & static_cast<i32>(kAreaBorder)) !=
                                     (points[ii * 4 + 3] & static_cast<i32>(kAreaBorder));
            if (diffRegs || areaBorders)
            {
                simplified.push_back(points[i * 4 + 0]);
                simplified.push_back(points[i * 4 + 1]);
                simplified.push_back(points[i * 4 + 2]);
                simplified.push_back(i);
            }
        }
    }

    if (simplified.empty())
    {
        // 接続が無い（完全に孤立した島）→ 左下と右上を種にする
        i32 llx = points[0], lly = points[1], llz = points[2], lli = 0;
        i32 urx = points[0], ury = points[1], urz = points[2], uri = 0;
        for (i32 i = 0; i < pn; ++i)
        {
            const i32 x = points[i * 4 + 0], y = points[i * 4 + 1], z = points[i * 4 + 2];
            if (x < llx || (x == llx && z < llz)) { llx = x; lly = y; llz = z; lli = i; }
            if (x > urx || (x == urx && z > urz)) { urx = x; ury = y; urz = z; uri = i; }
        }
        simplified.push_back(llx); simplified.push_back(lly); simplified.push_back(llz); simplified.push_back(lli);
        simplified.push_back(urx); simplified.push_back(ury); simplified.push_back(urz); simplified.push_back(uri);
    }

    // Douglas-Peucker（誤差は xz 平面で測る）
    for (i32 i = 0; i < static_cast<i32>(simplified.size() / 4);)
    {
        const i32 sn = static_cast<i32>(simplified.size() / 4);
        const i32 ii = (i + 1) % sn;

        i32 ax = simplified[i * 4 + 0], az = simplified[i * 4 + 2];
        const i32 ai = simplified[i * 4 + 3];
        i32 bx = simplified[ii * 4 + 0], bz = simplified[ii * 4 + 2];
        const i32 bi = simplified[ii * 4 + 3];

        f32 maxd = 0.0f;
        i32 maxi = -1, ci = 0, cinc = 0, endi = 0;

        // 走査方向を辞書順で固定＝逆向きに辿っても同じ誤差になる
        if (bx > ax || (bx == ax && bz > az)) { cinc = 1;      ci = (ai + cinc) % pn; endi = bi; }
        else                                  { cinc = pn - 1; ci = (bi + cinc) % pn; endi = ai;
                                                std::swap(ax, bx); std::swap(az, bz); }

        if ((points[ci * 4 + 3] & static_cast<i32>(kContourRegMask)) == 0 ||
            (points[ci * 4 + 3] & static_cast<i32>(kAreaBorder)))
        {
            while (ci != endi)
            {
                const f32 d = DistancePtSeg(points[ci * 4 + 0], points[ci * 4 + 2], ax, az, bx, bz);
                if (d > maxd) { maxd = d; maxi = ci; }
                ci = (ci + cinc) % pn;
            }
        }

        if (maxi != -1 && maxd > maxError * maxError)
            InsertPoint(simplified, i + 1, &points[static_cast<size_t>(maxi) * 4], maxi);
        else
            ++i;
    }

    // 長すぎる辺を割る（大きなポリゴンで高さが直線近似になりすぎるのを防ぐ）
    if (maxEdgeLen > 0)
    {
        for (i32 i = 0; i < static_cast<i32>(simplified.size() / 4);)
        {
            const i32 sn = static_cast<i32>(simplified.size() / 4);
            const i32 ii = (i + 1) % sn;
            const i32 ax = simplified[i * 4 + 0], az = simplified[i * 4 + 2];
            const i32 ai = simplified[i * 4 + 3];
            const i32 bx = simplified[ii * 4 + 0], bz = simplified[ii * 4 + 2];
            const i32 bi = simplified[ii * 4 + 3];

            i32 maxi = -1;
            const i32 ci = (ai + 1) % pn;
            // 外周（隣接領域が無い＝壁）と、エリア境界だけ割る
            const bool tess = ((points[ci * 4 + 3] & static_cast<i32>(kContourRegMask)) == 0) ||
                              ((points[ci * 4 + 3] & static_cast<i32>(kAreaBorder)) != 0);
            if (tess)
            {
                const i32 dx = bx - ax, dz = bz - az;
                if (dx * dx + dz * dz > maxEdgeLen * maxEdgeLen)
                {
                    const i32 n = bi < ai ? (bi + pn - ai) : (bi - ai);
                    if (n > 1)
                    {
                        if (bx > ax || (bx == ax && bz > az)) maxi = (ai + n / 2) % pn;
                        else                                   maxi = (ai + (n + 1) / 2) % pn;
                    }
                }
            }
            if (maxi != -1) InsertPoint(simplified, i + 1, &points[static_cast<size_t>(maxi) * 4], maxi);
            else ++i;
        }
    }

    // 隣接領域 ID は「次の生点」から、境界頂点フラグは「自分の生点」から取る
    for (i32 i = 0, sn = static_cast<i32>(simplified.size() / 4); i < sn; ++i)
    {
        const i32 bi = simplified[i * 4 + 3];
        const i32 ai = (bi + 1) % pn;
        simplified[i * 4 + 3] =
            (points[ai * 4 + 3] & static_cast<i32>(kContourRegMask | kAreaBorder)) |
            (points[bi * 4 + 3] & static_cast<i32>(kBorderVertex));
    }
}

void RemoveDegenerateSegments(std::vector<i32>& simplified)
{
    i32 npts = static_cast<i32>(simplified.size() / 4);
    for (i32 i = 0; i < npts; ++i)
    {
        const i32 ni = Next(i, npts);
        if (simplified[i * 4 + 0] == simplified[ni * 4 + 0] &&
            simplified[i * 4 + 2] == simplified[ni * 4 + 2])
        {
            simplified.erase(simplified.begin() + static_cast<i64>(i) * 4,
                             simplified.begin() + static_cast<i64>(i) * 4 + 4);
            --npts; --i;
        }
    }
}

i32 CalcAreaOfPolygon2D(const i32* verts, i32 nverts)
{
    i32 area = 0;
    for (i32 i = 0, j = nverts - 1; i < nverts; j = i++)
    {
        const i32* vi = &verts[i * 4];
        const i32* vj = &verts[j * 4];
        area += vi[0] * vj[2] - vj[0] * vi[2];
    }
    return (area + 1) / 2;
}

struct ContourHole { i32 minx = 0, minz = 0, leftmost = 0; NavContour* contour = nullptr; };
struct ContourRegion { NavContour* outline = nullptr; std::vector<ContourHole> holes; };
struct PotentialDiagonal { i32 vert = 0; i32 dist = 0; };

void FindLeftMostVertex(const NavContour& c, i32& minx, i32& minz, i32& leftmost)
{
    const i32 nv = static_cast<i32>(c.verts.size() / 4);
    minx = c.verts[0]; minz = c.verts[2]; leftmost = 0;
    for (i32 i = 1; i < nv; ++i)
    {
        const i32 x = c.verts[i * 4 + 0], z = c.verts[i * 4 + 2];
        if (x < minx || (x == minx && z < minz)) { minx = x; minz = z; leftmost = i; }
    }
}

bool IntersectSegContour(const i32* d0, const i32* d1, i32 i, i32 n, const i32* verts)
{
    for (i32 k = 0; k < n; ++k)
    {
        const i32 k1 = Next(k, n);
        if (i == k || i == k1) continue;
        const i32* p0 = &verts[k * 4];
        const i32* p1 = &verts[k1 * 4];
        if (VEqual(d0, p0) || VEqual(d1, p0) || VEqual(d0, p1) || VEqual(d1, p1)) continue;
        if (Intersect(d0, d1, p0, p1)) return true;
    }
    return false;
}

bool MergeContours(NavContour& ca, NavContour& cb, i32 ia, i32 ib)
{
    const i32 na = static_cast<i32>(ca.verts.size() / 4);
    const i32 nb = static_cast<i32>(cb.verts.size() / 4);
    if (na < 3 || nb < 3) return false;

    std::vector<i32> verts;
    verts.reserve(static_cast<size_t>(na + nb + 2) * 4);
    for (i32 i = 0; i <= na; ++i)
    {
        const i32 s = ((ia + i) % na) * 4;
        verts.insert(verts.end(), { ca.verts[s], ca.verts[s + 1], ca.verts[s + 2], ca.verts[s + 3] });
    }
    for (i32 i = 0; i <= nb; ++i)
    {
        const i32 s = ((ib + i) % nb) * 4;
        verts.insert(verts.end(), { cb.verts[s], cb.verts[s + 1], cb.verts[s + 2], cb.verts[s + 3] });
    }
    ca.verts.swap(verts);
    cb.verts.clear();
    return true;
}

void MergeRegionHoles(ContourRegion& region)
{
    for (ContourHole& hole : region.holes)
        FindLeftMostVertex(*hole.contour, hole.minx, hole.minz, hole.leftmost);

    std::sort(region.holes.begin(), region.holes.end(),
              [](const ContourHole& a, const ContourHole& b)
              {
                  if (a.minx == b.minx) return a.minz < b.minz;
                  return a.minx < b.minx;
              });

    NavContour* outline = region.outline;
    std::vector<PotentialDiagonal> diags;

    for (size_t hi = 0; hi < region.holes.size(); ++hi)
    {
        NavContour* hole = region.holes[hi].contour;
        const i32 hn = static_cast<i32>(hole->verts.size() / 4);
        if (hn < 3) continue;

        i32 index = -1;
        i32 bestVertex = region.holes[hi].leftmost;
        for (i32 iter = 0; iter < hn; ++iter)
        {
            diags.clear();
            const i32* corner = &hole->verts[static_cast<size_t>(bestVertex) * 4];
            const i32 on = static_cast<i32>(outline->verts.size() / 4);
            for (i32 j = 0; j < on; ++j)
            {
                if (!InCone(j, on, outline->verts.data(), corner)) continue;
                const i32 dx = outline->verts[j * 4 + 0] - corner[0];
                const i32 dz = outline->verts[j * 4 + 2] - corner[2];
                diags.push_back({ j, dx * dx + dz * dz });
            }
            std::sort(diags.begin(), diags.end(),
                      [](const PotentialDiagonal& a, const PotentialDiagonal& b) { return a.dist < b.dist; });

            index = -1;
            for (const PotentialDiagonal& d : diags)
            {
                const i32* pt = &outline->verts[static_cast<size_t>(d.vert) * 4];
                bool intersect = IntersectSegContour(pt, corner, d.vert, on, outline->verts.data());
                for (size_t k = hi; k < region.holes.size() && !intersect; ++k)
                {
                    const NavContour* hc = region.holes[k].contour;
                    if (hc->verts.empty()) continue;
                    intersect = IntersectSegContour(pt, corner, -1,
                                                    static_cast<i32>(hc->verts.size() / 4), hc->verts.data());
                }
                if (!intersect) { index = d.vert; break; }
            }
            if (index != -1) break;
            bestVertex = (bestVertex + 1) % hn;
        }
        if (index == -1) continue;              // 橋渡し不能（極端に潰れた穴）→ 諦めて捨てる
        MergeContours(*outline, *hole, index, bestVertex);
    }
}

} // namespace

// ---------------------------------------------------------------------------
bool BuildContours(const NavCompactHeightfield& chf, f32 maxError, i32 maxEdgeLen,
                   NavContourSet& cset, std::string& err)
{
    const i32 w = chf.w, h = chf.h;
    cset.conts.clear();
    for (i32 k = 0; k < 3; ++k) { cset.bmin[k] = chf.bmin[k]; cset.bmax[k] = chf.bmax[k]; }
    cset.cs = chf.cs; cset.ch = chf.ch;
    cset.w = w; cset.h = h; cset.borderSize = chf.borderSize;
    cset.maxError = maxError;

    if (chf.maxRegions == 0) { err = "領域が 1 つも無い"; return false; }

    // 各 span の「隣が別領域である辺」にビットを立てる
    std::vector<u8> flags(static_cast<size_t>(chf.spanCount), 0);
    for (i32 z = 0; z < h; ++z)
        for (i32 x = 0; x < w; ++x)
        {
            const NavCompactCell& c = chf.cells[x + z * w];
            for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
            {
                u8 res = 0;
                const NavCompactSpan& s = chf.spans[i];
                if (!s.reg || (s.reg & kBorderRegion)) { flags[i] = 0; continue; }
                for (i32 dir = 0; dir < 4; ++dir)
                {
                    u16 r = 0;
                    if (GetCon(s, dir) != kNotConnected)
                    {
                        const i32 ax = x + kDirOffsetX[dir];
                        const i32 az = z + kDirOffsetZ[dir];
                        const u32 ai = chf.cells[ax + az * w].index + GetCon(s, dir);
                        r = chf.spans[ai].reg;
                    }
                    if (r == s.reg) res = static_cast<u8>(res | (1 << dir));
                }
                flags[i] = static_cast<u8>(res ^ 0xf);   // 「繋がっていない辺」に反転
            }
        }

    std::vector<i32> raw, simplified;
    for (i32 z = 0; z < h; ++z)
        for (i32 x = 0; x < w; ++x)
        {
            const NavCompactCell& c = chf.cells[x + z * w];
            for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
            {
                if (flags[i] == 0 || flags[i] == 0xf) { flags[i] = 0; continue; }
                const u16 reg = chf.spans[i].reg;
                if (!reg || (reg & kBorderRegion)) continue;
                const u8 area = chf.areas[i];

                raw.clear(); simplified.clear();
                WalkContourRaw(x, z, i, chf, flags, raw);
                SimplifyContour(raw, simplified, maxError, maxEdgeLen);
                RemoveDegenerateSegments(simplified);

                if (simplified.size() / 4 >= 3)
                {
                    NavContour cont;
                    cont.verts = simplified;
                    cont.rverts = raw;
                    cont.reg = reg;
                    cont.area = area;
                    cset.conts.push_back(std::move(cont));
                }
            }
        }

    if (cset.conts.empty()) { err = "輪郭が 1 本も取れなかった"; return false; }

    // ---- 穴（時計回り = 負面積）を外輪郭へ橋渡し ----
    const i32 nconts = static_cast<i32>(cset.conts.size());
    std::vector<i8> winding(static_cast<size_t>(nconts), 1);
    i32 nholes = 0;
    for (i32 i = 0; i < nconts; ++i)
    {
        NavContour& c = cset.conts[static_cast<size_t>(i)];
        const i32 nv = static_cast<i32>(c.verts.size() / 4);
        winding[static_cast<size_t>(i)] =
            static_cast<i8>(CalcAreaOfPolygon2D(c.verts.data(), nv) < 0 ? -1 : 1);
        if (winding[static_cast<size_t>(i)] < 0) ++nholes;
    }

    if (nholes > 0)
    {
        const i32 nregions = static_cast<i32>(chf.maxRegions) + 1;
        std::vector<ContourRegion> regions(static_cast<size_t>(nregions));
        for (i32 i = 0; i < nconts; ++i)
        {
            NavContour& c = cset.conts[static_cast<size_t>(i)];
            if (c.reg >= static_cast<u16>(nregions)) continue;
            ContourRegion& r = regions[c.reg];
            if (winding[static_cast<size_t>(i)] > 0)
            {
                // 同じ領域に外輪郭が 2 本 = 領域が分断されている（分割の不整合）。
                // 面積の大きい方を外輪郭として採り、もう一方は穴として扱う。
                if (r.outline)
                {
                    const i32 aOld = std::abs(CalcAreaOfPolygon2D(r.outline->verts.data(),
                                     static_cast<i32>(r.outline->verts.size() / 4)));
                    const i32 aNew = std::abs(CalcAreaOfPolygon2D(c.verts.data(), static_cast<i32>(c.verts.size() / 4)));
                    if (aNew > aOld) { r.holes.push_back({ 0, 0, 0, r.outline }); r.outline = &c; }
                    else             { r.holes.push_back({ 0, 0, 0, &c }); }
                }
                else r.outline = &c;
            }
            else r.holes.push_back({ 0, 0, 0, &c });
        }
        for (ContourRegion& r : regions)
        {
            if (r.holes.empty()) continue;
            if (!r.outline) continue;     // 外輪郭が無い穴だけの領域 → 落とす
            MergeRegionHoles(r);
        }
        // 統合されて空になった輪郭を除去
        cset.conts.erase(std::remove_if(cset.conts.begin(), cset.conts.end(),
                                        [](const NavContour& c) { return c.verts.size() / 4 < 3; }),
                         cset.conts.end());
    }

    return !cset.conts.empty();
}

} // namespace nav
} // namespace dx12e
