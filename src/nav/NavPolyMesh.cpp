// ============================================================================
// ナビメッシュ 段階 7: 輪郭 → 凸ポリゴンメッシュ
// ============================================================================
// 輪郭を耳刈り(ear clipping)で三角形へ割り、そのあと「共有辺を持ち、併合しても
// 凸のままで、頂点数が nvp 以下」という条件を満たす対を貪欲に併合する。
// 三角形のままでも動くが、ポリゴンが大きいほど A* のノードが減り、ファネルの
// 通り道も広がって経路が素直になる。
// ============================================================================

#include "nav/NavTypes.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace dx12e
{
namespace nav
{

namespace
{

constexpr i32 kVertexBucketCount = 1 << 12;
constexpr u32 kRemovableFlag = 0x80000000u;
constexpr u32 kIndexMask     = 0x0fffffffu;

inline i32 Prev(i32 i, i32 n) { return i - 1 >= 0 ? i - 1 : n - 1; }
inline i32 Next(i32 i, i32 n) { return i + 1 < n ? i + 1 : 0; }

inline i32 Area2(const i32* a, const i32* b, const i32* c)
{
    return (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
}
inline bool XorB(bool x, bool y) { return !x ^ !y; }
inline bool Left(const i32* a, const i32* b, const i32* c)     { return Area2(a, b, c) < 0; }
inline bool LeftOn(const i32* a, const i32* b, const i32* c)    { return Area2(a, b, c) <= 0; }
inline bool Collinear(const i32* a, const i32* b, const i32* c) { return Area2(a, b, c) == 0; }

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
inline bool VEqual(const i32* a, const i32* b) { return a[0] == b[0] && a[2] == b[2]; }

bool InCone(i32 i, i32 j, i32 n, const i32* verts, const i32* indices)
{
    const i32* pi   = &verts[(indices[i] & kIndexMask) * 4];
    const i32* pj   = &verts[(indices[j] & kIndexMask) * 4];
    const i32* pi1  = &verts[(indices[Next(i, n)] & kIndexMask) * 4];
    const i32* pin1 = &verts[(indices[Prev(i, n)] & kIndexMask) * 4];
    if (LeftOn(pin1, pi, pi1)) return Left(pi, pj, pin1) && Left(pj, pi, pi1);
    return !(LeftOn(pi, pj, pi1) && LeftOn(pj, pi, pin1));
}

// 輪郭が自己重複しているとき用の緩い版（Recast と同じ救済策）
bool InConeLoose(i32 i, i32 j, i32 n, const i32* verts, const i32* indices)
{
    const i32* pi   = &verts[(indices[i] & kIndexMask) * 4];
    const i32* pj   = &verts[(indices[j] & kIndexMask) * 4];
    const i32* pi1  = &verts[(indices[Next(i, n)] & kIndexMask) * 4];
    const i32* pin1 = &verts[(indices[Prev(i, n)] & kIndexMask) * 4];
    if (LeftOn(pin1, pi, pi1)) return LeftOn(pi, pj, pin1) && LeftOn(pj, pi, pi1);
    return !(LeftOn(pi, pj, pi1) && LeftOn(pj, pi, pin1));
}

bool Diagonalie(i32 i, i32 j, i32 n, const i32* verts, const i32* indices)
{
    const i32* d0 = &verts[(indices[i] & kIndexMask) * 4];
    const i32* d1 = &verts[(indices[j] & kIndexMask) * 4];
    for (i32 k = 0; k < n; ++k)
    {
        const i32 k1 = Next(k, n);
        if (!((k == i) || (k1 == i) || (k == j) || (k1 == j)))
        {
            const i32* p0 = &verts[(indices[k] & kIndexMask) * 4];
            const i32* p1 = &verts[(indices[k1] & kIndexMask) * 4];
            if (VEqual(d0, p0) || VEqual(d1, p0) || VEqual(d0, p1) || VEqual(d1, p1)) continue;
            if (Intersect(d0, d1, p0, p1)) return false;
        }
    }
    return true;
}

bool DiagonalieLoose(i32 i, i32 j, i32 n, const i32* verts, const i32* indices)
{
    const i32* d0 = &verts[(indices[i] & kIndexMask) * 4];
    const i32* d1 = &verts[(indices[j] & kIndexMask) * 4];
    for (i32 k = 0; k < n; ++k)
    {
        const i32 k1 = Next(k, n);
        if (!((k == i) || (k1 == i) || (k == j) || (k1 == j)))
        {
            const i32* p0 = &verts[(indices[k] & kIndexMask) * 4];
            const i32* p1 = &verts[(indices[k1] & kIndexMask) * 4];
            if (VEqual(d0, p0) || VEqual(d1, p0) || VEqual(d0, p1) || VEqual(d1, p1)) continue;
            if (IntersectProp(d0, d1, p0, p1)) return false;
        }
    }
    return true;
}

bool Diagonal(i32 i, i32 j, i32 n, const i32* verts, const i32* indices)
{
    return InCone(i, j, n, verts, indices) && Diagonalie(i, j, n, verts, indices);
}
bool DiagonalLoose(i32 i, i32 j, i32 n, const i32* verts, const i32* indices)
{
    return InConeLoose(i, j, n, verts, indices) && DiagonalieLoose(i, j, n, verts, indices);
}

// 耳刈り三角形分割。戻り値が負なら「重複輪郭を緩い判定で救済した」印。
i32 Triangulate(i32 n, const i32* verts, i32* indices, std::vector<i32>& tris)
{
    tris.clear();
    i32 ntris = 0;

    for (i32 i = 0; i < n; ++i)
    {
        const i32 i1 = Next(i, n);
        const i32 i2 = Next(i1, n);
        if (Diagonal(i, i2, n, verts, indices))
            indices[i1] = static_cast<i32>(static_cast<u32>(indices[i1]) | kRemovableFlag);
    }

    while (n > 3)
    {
        i32 minLen = -1, mini = -1;
        for (i32 i = 0; i < n; ++i)
        {
            const i32 i1 = Next(i, n);
            if (static_cast<u32>(indices[i1]) & kRemovableFlag)
            {
                const i32* p0 = &verts[(indices[i] & kIndexMask) * 4];
                const i32* p2 = &verts[(indices[Next(i1, n)] & kIndexMask) * 4];
                const i32 dx = p2[0] - p0[0];
                const i32 dz = p2[2] - p0[2];
                const i32 len = dx * dx + dz * dz;
                if (minLen < 0 || len < minLen) { minLen = len; mini = i; }
            }
        }

        if (mini == -1)
        {
            for (i32 i = 0; i < n; ++i)
            {
                const i32 i1 = Next(i, n);
                const i32 i2 = Next(i1, n);
                if (DiagonalLoose(i, i2, n, verts, indices))
                {
                    const i32* p0 = &verts[(indices[i] & kIndexMask) * 4];
                    const i32* p2 = &verts[(indices[Next(i2, n)] & kIndexMask) * 4];
                    const i32 dx = p2[0] - p0[0];
                    const i32 dz = p2[2] - p0[2];
                    const i32 len = dx * dx + dz * dz;
                    if (minLen < 0 || len < minLen) { minLen = len; mini = i; }
                }
            }
            if (mini == -1) return -ntris;   // どうにもならない輪郭
        }

        i32 i  = mini;
        i32 i1 = Next(i, n);
        const i32 i2 = Next(i1, n);
        tris.push_back(indices[i]  & static_cast<i32>(kIndexMask));
        tris.push_back(indices[i1] & static_cast<i32>(kIndexMask));
        tris.push_back(indices[i2] & static_cast<i32>(kIndexMask));
        ++ntris;

        --n;
        for (i32 k = i1; k < n; ++k) indices[k] = indices[k + 1];

        if (i1 >= n) i1 = 0;
        i = Prev(i1, n);
        if (Diagonal(Prev(i, n), i1, n, verts, indices))
            indices[i] = static_cast<i32>(static_cast<u32>(indices[i]) | kRemovableFlag);
        else
            indices[i] = static_cast<i32>(static_cast<u32>(indices[i]) & kIndexMask);
        if (Diagonal(i, Next(i1, n), n, verts, indices))
            indices[i1] = static_cast<i32>(static_cast<u32>(indices[i1]) | kRemovableFlag);
        else
            indices[i1] = static_cast<i32>(static_cast<u32>(indices[i1]) & kIndexMask);
    }

    tris.push_back(indices[0] & static_cast<i32>(kIndexMask));
    tris.push_back(indices[1] & static_cast<i32>(kIndexMask));
    tris.push_back(indices[2] & static_cast<i32>(kIndexMask));
    ++ntris;
    return ntris;
}

inline i32 ComputeVertexHash(i32 x, i32 y, i32 z)
{
    const u32 h1 = 0x8da6b343, h2 = 0xd8163841, h3 = 0xcb1ab31f;
    const u32 n = h1 * static_cast<u32>(x) + h2 * static_cast<u32>(y) + h3 * static_cast<u32>(z);
    return static_cast<i32>(n & (kVertexBucketCount - 1));
}

u16 AddVertex(u16 x, u16 y, u16 z, std::vector<u16>& verts,
              std::vector<i32>& firstVert, std::vector<i32>& nextVert, i32& nv)
{
    const i32 bucket = ComputeVertexHash(x, 0, z);
    i32 i = firstVert[static_cast<size_t>(bucket)];
    while (i != -1)
    {
        const u16* v = &verts[static_cast<size_t>(i) * 3];
        if (v[0] == x && std::abs(static_cast<i32>(v[1]) - static_cast<i32>(y)) <= 2 && v[2] == z)
            return static_cast<u16>(i);
        i = nextVert[static_cast<size_t>(i)];
    }
    i = nv++;
    verts[static_cast<size_t>(i) * 3 + 0] = x;
    verts[static_cast<size_t>(i) * 3 + 1] = y;
    verts[static_cast<size_t>(i) * 3 + 2] = z;
    nextVert[static_cast<size_t>(i)] = firstVert[static_cast<size_t>(bucket)];
    firstVert[static_cast<size_t>(bucket)] = i;
    return static_cast<u16>(i);
}

i32 CountPolyVerts(const u16* p, i32 nvp)
{
    for (i32 i = 0; i < nvp; ++i) if (p[i] == kMeshNullIdx) return i;
    return nvp;
}

inline bool ULeft(const u16* a, const u16* b, const u16* c)
{
    return (static_cast<i32>(b[0]) - static_cast<i32>(a[0])) *
           (static_cast<i32>(c[2]) - static_cast<i32>(a[2])) -
           (static_cast<i32>(c[0]) - static_cast<i32>(a[0])) *
           (static_cast<i32>(b[2]) - static_cast<i32>(a[2])) < 0;
}

i32 GetPolyMergeValue(const u16* pa, const u16* pb, const std::vector<u16>& verts,
                      i32& ea, i32& eb, i32 nvp)
{
    const i32 na = CountPolyVerts(pa, nvp);
    const i32 nb = CountPolyVerts(pb, nvp);
    if (na + nb - 2 > nvp) return -1;

    ea = -1; eb = -1;
    for (i32 i = 0; i < na && ea == -1; ++i)
    {
        u16 va0 = pa[i], va1 = pa[(i + 1) % na];
        if (va0 > va1) std::swap(va0, va1);
        for (i32 j = 0; j < nb; ++j)
        {
            u16 vb0 = pb[j], vb1 = pb[(j + 1) % nb];
            if (vb0 > vb1) std::swap(vb0, vb1);
            if (va0 == vb0 && va1 == vb1) { ea = i; eb = j; break; }
        }
    }
    if (ea == -1 || eb == -1) return -1;

    u16 va = pa[(ea + na - 1) % na];
    u16 vb = pa[(ea + 2) % na];
    u16 vc = pb[(eb + 2) % nb];
    if (!ULeft(&verts[static_cast<size_t>(va) * 3], &verts[static_cast<size_t>(vb) * 3],
               &verts[static_cast<size_t>(vc) * 3])) return -1;

    va = pb[(eb + nb - 1) % nb];
    vb = pb[(eb + 2) % nb];
    vc = pa[(ea + 2) % na];
    if (!ULeft(&verts[static_cast<size_t>(va) * 3], &verts[static_cast<size_t>(vb) * 3],
               &verts[static_cast<size_t>(vc) * 3])) return -1;

    va = pa[ea]; vb = pa[(ea + 1) % na];
    const i32 dx = static_cast<i32>(verts[static_cast<size_t>(va) * 3 + 0]) -
                   static_cast<i32>(verts[static_cast<size_t>(vb) * 3 + 0]);
    const i32 dz = static_cast<i32>(verts[static_cast<size_t>(va) * 3 + 2]) -
                   static_cast<i32>(verts[static_cast<size_t>(vb) * 3 + 2]);
    return dx * dx + dz * dz;
}

void MergePolyVerts(u16* pa, const u16* pb, i32 ea, i32 eb, u16* tmp, i32 nvp)
{
    const i32 na = CountPolyVerts(pa, nvp);
    const i32 nb = CountPolyVerts(pb, nvp);
    for (i32 i = 0; i < nvp; ++i) tmp[i] = kMeshNullIdx;
    i32 n = 0;
    for (i32 i = 0; i < na - 1; ++i) tmp[n++] = pa[(ea + 1 + i) % na];
    for (i32 i = 0; i < nb - 1; ++i) tmp[n++] = pb[(eb + 1 + i) % nb];
    std::memcpy(pa, tmp, sizeof(u16) * static_cast<size_t>(nvp));
}

// 辺（頂点対）をキーにしてポリゴン同士を突き合わせる。
// Recast の自前ハッシュより素直で、結果は同じ。
void BuildMeshAdjacency(std::vector<u16>& polys, i32 npolys, i32 nvp)
{
    std::unordered_map<u64, std::pair<i32, i32>> edgeMap;
    edgeMap.reserve(static_cast<size_t>(npolys) * 3);

    for (i32 p = 0; p < npolys; ++p)
    {
        u16* poly = &polys[static_cast<size_t>(p) * nvp * 2];
        const i32 nv = CountPolyVerts(poly, nvp);
        for (i32 e = 0; e < nv; ++e)
        {
            const u16 v0 = poly[e];
            const u16 v1 = poly[(e + 1) % nv];
            const u64 key = (v0 < v1) ? ((static_cast<u64>(v0) << 16) | v1)
                                      : ((static_cast<u64>(v1) << 16) | v0);
            auto it = edgeMap.find(key);
            if (it == edgeMap.end())
            {
                edgeMap.emplace(key, std::make_pair(p, e));
            }
            else
            {
                const i32 op = it->second.first;
                const i32 oe = it->second.second;
                if (op == p) continue;   // 同一ポリゴン内の重複辺（退化）は無視
                poly[nvp + e] = static_cast<u16>(op);
                polys[static_cast<size_t>(op) * nvp * 2 + nvp + oe] = static_cast<u16>(p);
                edgeMap.erase(it);       // 3 枚目が来ても無視される（非多様体対策）
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
bool BuildPolyMesh(const NavContourSet& cset, i32 nvp, NavPolyMeshRaw& pm, std::string& err)
{
    nvp = std::clamp(nvp, 3, 12);
    pm.nvp = nvp;
    pm.cs = cset.cs; pm.ch = cset.ch;
    for (i32 k = 0; k < 3; ++k) { pm.bmin[k] = cset.bmin[k]; pm.bmax[k] = cset.bmax[k]; }
    pm.borderSize = cset.borderSize;

    i32 maxVertices = 0, maxTris = 0, maxVertsPerCont = 0;
    for (const NavContour& c : cset.conts)
    {
        const i32 nv = static_cast<i32>(c.verts.size() / 4);
        if (nv < 3) continue;
        maxVertices += nv;
        maxTris     += nv - 2;
        maxVertsPerCont = (std::max)(maxVertsPerCont, nv);
    }
    if (maxVertices == 0) { err = "ポリゴン化できる輪郭が無い"; return false; }
    if (maxVertices >= 0xfffe) { err = "頂点数が上限(65534)を超えた。cellSize を上げること"; return false; }

    pm.verts.assign(static_cast<size_t>(maxVertices) * 3, 0);
    pm.polys.assign(static_cast<size_t>(maxTris) * nvp * 2, kMeshNullIdx);
    pm.regs.assign(static_cast<size_t>(maxTris), 0);
    pm.areas.assign(static_cast<size_t>(maxTris), 0);
    pm.nverts = 0;
    pm.npolys = 0;

    std::vector<i32> nextVert(static_cast<size_t>(maxVertices), 0);
    std::vector<i32> firstVert(kVertexBucketCount, -1);
    std::vector<i32> indices(static_cast<size_t>(maxVertsPerCont));
    std::vector<i32> tris;
    std::vector<u16> polys(static_cast<size_t>(maxVertsPerCont + 1) * nvp, kMeshNullIdx);
    u16* tmpPoly = &polys[static_cast<size_t>(maxVertsPerCont) * nvp];

    i32 degenerate = 0;
    for (const NavContour& cont : cset.conts)
    {
        const i32 nv = static_cast<i32>(cont.verts.size() / 4);
        if (nv < 3) continue;

        for (i32 j = 0; j < nv; ++j) indices[static_cast<size_t>(j)] = j;
        i32 ntris = Triangulate(nv, cont.verts.data(), indices.data(), tris);
        if (ntris <= 0) { ++degenerate; ntris = -ntris; }
        if (ntris <= 0) continue;

        // 頂点をグローバル配列へ（近い高さは同一頂点として溶接）
        for (i32 j = 0; j < nv; ++j)
        {
            const i32* v = &cont.verts[static_cast<size_t>(j) * 4];
            indices[static_cast<size_t>(j)] =
                AddVertex(static_cast<u16>(v[0]), static_cast<u16>(v[1]), static_cast<u16>(v[2]),
                          pm.verts, firstVert, nextVert, pm.nverts);
        }

        i32 npolys = 0;
        std::fill(polys.begin(), polys.begin() + static_cast<i64>(maxVertsPerCont) * nvp, kMeshNullIdx);
        for (i32 j = 0; j < ntris; ++j)
        {
            const i32 t0 = tris[static_cast<size_t>(j) * 3 + 0];
            const i32 t1 = tris[static_cast<size_t>(j) * 3 + 1];
            const i32 t2 = tris[static_cast<size_t>(j) * 3 + 2];
            if (t0 == t1 || t0 == t2 || t1 == t2) continue;
            u16* p = &polys[static_cast<size_t>(npolys) * nvp];
            p[0] = static_cast<u16>(indices[static_cast<size_t>(t0)]);
            p[1] = static_cast<u16>(indices[static_cast<size_t>(t1)]);
            p[2] = static_cast<u16>(indices[static_cast<size_t>(t2)]);
            ++npolys;
        }
        if (npolys == 0) continue;

        if (nvp > 3)
        {
            for (;;)
            {
                i32 bestVal = 0, bestPa = 0, bestPb = 0, bestEa = 0, bestEb = 0;
                for (i32 j = 0; j < npolys - 1; ++j)
                {
                    const u16* pj = &polys[static_cast<size_t>(j) * nvp];
                    for (i32 k = j + 1; k < npolys; ++k)
                    {
                        const u16* pk = &polys[static_cast<size_t>(k) * nvp];
                        i32 ea = 0, eb = 0;
                        const i32 v = GetPolyMergeValue(pj, pk, pm.verts, ea, eb, nvp);
                        if (v > bestVal) { bestVal = v; bestPa = j; bestPb = k; bestEa = ea; bestEb = eb; }
                    }
                }
                if (bestVal <= 0) break;
                u16* pa = &polys[static_cast<size_t>(bestPa) * nvp];
                u16* pb = &polys[static_cast<size_t>(bestPb) * nvp];
                MergePolyVerts(pa, pb, bestEa, bestEb, tmpPoly, nvp);
                u16* last = &polys[static_cast<size_t>(npolys - 1) * nvp];
                if (pb != last) std::memcpy(pb, last, sizeof(u16) * static_cast<size_t>(nvp));
                --npolys;
            }
        }

        for (i32 j = 0; j < npolys; ++j)
        {
            if (pm.npolys >= maxTris) { err = "ポリゴン数が見積もりを超えた"; return false; }
            u16* dst = &pm.polys[static_cast<size_t>(pm.npolys) * nvp * 2];
            const u16* src = &polys[static_cast<size_t>(j) * nvp];
            for (i32 k = 0; k < nvp; ++k) dst[k] = src[k];
            pm.regs[static_cast<size_t>(pm.npolys)]  = cont.reg;
            pm.areas[static_cast<size_t>(pm.npolys)] = cont.area;
            ++pm.npolys;
        }
    }

    if (pm.npolys == 0) { err = "ポリゴンが 1 枚も作れなかった"; return false; }

    pm.verts.resize(static_cast<size_t>(pm.nverts) * 3);
    pm.polys.resize(static_cast<size_t>(pm.npolys) * nvp * 2);
    pm.regs.resize(static_cast<size_t>(pm.npolys));
    pm.areas.resize(static_cast<size_t>(pm.npolys));

    BuildMeshAdjacency(pm.polys, pm.npolys, nvp);

    if (degenerate > 0)
        err = "警告: 自己交差した輪郭を " + std::to_string(degenerate) + " 本、緩い判定で救済した";
    return true;
}

} // namespace nav
} // namespace dx12e
