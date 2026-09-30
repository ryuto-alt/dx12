#include "renderer/foliage/FoliageScatter.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace dx12e::foliage
{
namespace
{
// 自前 RNG（プラットフォーム非依存・決定論）。
struct Rng
{
    u32 s;
    explicit Rng(u32 seed) : s(HashU32(seed ^ 0x9e3779b9u) + 0x85ebca6bu) {}
    u32 Next() { s += 0x9e3779b9u; return HashU32(s); }
    f32 F() { return static_cast<f32>(Next() >> 8) * (1.0f / 16777216.0f); }   // [0,1)
};

struct P2 { f32 x, z; };

u64 CellKey(i32 cx, i32 cz) { return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cz); }

bool InRegion(const ScatterRegion& r, f32 x, f32 z)
{
    if (r.circle)
    {
        const f32 dx = x - r.cx, dz = z - r.cz;
        return dx * dx + dz * dz <= r.radius * r.radius;
    }
    return x >= r.x0 && x <= r.x1 && z >= r.z0 && z <= r.z1;
}

// 領域の外接矩形
void BoundsOf(const ScatterRegion& r, f32& x0, f32& z0, f32& x1, f32& z1)
{
    if (r.circle) { x0 = r.cx - r.radius; z0 = r.cz - r.radius; x1 = r.cx + r.radius; z1 = r.cz + r.radius; }
    else          { x0 = r.x0; z0 = r.z0; x1 = r.x1; z1 = r.z1; }
}

// ---- Bridson のポアソンディスク（領域の外接矩形内。円は後で内外判定）----
void BridsonPoisson(f32 x0, f32 z0, f32 x1, f32 z1, f32 r, Rng& rng, u32 maxPts, std::vector<P2>& out)
{
    const f32 w = x1 - x0, h = z1 - z0;
    if (!(w > 0.0f) || !(h > 0.0f) || !(r > 1e-4f)) return;
    const f32 cell = r / 1.41421356f;
    const i32 gw = std::max(1, static_cast<i32>(std::ceil(w / cell)));
    const i32 gh = std::max(1, static_cast<i32>(std::ceil(h / cell)));
    // 巨大な格子（数億セル）を作らない保険
    if (static_cast<u64>(gw) * static_cast<u64>(gh) > 400000000ull) return;
    std::vector<i32> grid(static_cast<size_t>(gw) * gh, -1);
    std::vector<u32> active;
    auto cellOf = [&](f32 x, f32 z, i32& cx, i32& cz)
    {
        cx = std::clamp(static_cast<i32>((x - x0) / cell), 0, gw - 1);
        cz = std::clamp(static_cast<i32>((z - z0) / cell), 0, gh - 1);
    };
    auto add = [&](f32 x, f32 z)
    {
        i32 cx, cz; cellOf(x, z, cx, cz);
        grid[static_cast<size_t>(cz) * gw + cx] = static_cast<i32>(out.size());
        active.push_back(static_cast<u32>(out.size()));
        out.push_back({x, z});
    };
    add(x0 + rng.F() * w, z0 + rng.F() * h);
    const f32 r2 = r * r;
    while (!active.empty() && out.size() < maxPts)
    {
        // 乱択（決定論の RNG で選ぶ）
        const u32 ai = rng.Next() % static_cast<u32>(active.size());
        const P2 base = out[active[ai]];
        bool found = false;
        for (int k = 0; k < 30; ++k)
        {
            const f32 ang = rng.F() * kTwoPi;
            const f32 rad = r * (1.0f + rng.F());
            const f32 nx = base.x + std::cos(ang) * rad, nz = base.z + std::sin(ang) * rad;
            if (nx < x0 || nx >= x1 || nz < z0 || nz >= z1) continue;
            i32 cx, cz; cellOf(nx, nz, cx, cz);
            bool ok = true;
            for (i32 j = std::max(0, cz - 2); j <= std::min(gh - 1, cz + 2) && ok; ++j)
                for (i32 i = std::max(0, cx - 2); i <= std::min(gw - 1, cx + 2); ++i)
                {
                    const i32 gi = grid[static_cast<size_t>(j) * gw + i];
                    if (gi < 0) continue;
                    const f32 dx = out[gi].x - nx, dz = out[gi].z - nz;
                    if (dx * dx + dz * dz < r2) { ok = false; break; }
                }
            if (!ok) continue;
            add(nx, nz);
            found = true;
            break;
        }
        if (!found)
        {
            active[ai] = active.back();
            active.pop_back();
        }
    }
}

// ---- 一様ランダム（ジッタ格子: 1 セル 1 点。密度に忠実で偏りが少ない）----
void JitterGrid(f32 x0, f32 z0, f32 x1, f32 z1, f32 density, Rng& rng, u32 maxPts, std::vector<P2>& out)
{
    if (!(density > 1e-6f)) return;
    const f32 cell = 1.0f / std::sqrt(density);
    const f32 w = x1 - x0, h = z1 - z0;
    if (!(w > 0.0f) || !(h > 0.0f)) return;
    const i32 gw = std::max(1, static_cast<i32>(std::ceil(w / cell)));
    const i32 gh = std::max(1, static_cast<i32>(std::ceil(h / cell)));
    if (static_cast<u64>(gw) * static_cast<u64>(gh) > 400000000ull) return;
    for (i32 j = 0; j < gh; ++j)
        for (i32 i = 0; i < gw; ++i)
        {
            const f32 x = x0 + (static_cast<f32>(i) + rng.F()) * cell;
            const f32 z = z0 + (static_cast<f32>(j) + rng.F()) * cell;
            if (x >= x1 || z >= z1) continue;
            out.push_back({x, z});
            if (out.size() >= maxPts) return;
        }
}
} // namespace

ScatterStats ScatterPoisson(const ScatterParams& p, const ScatterRegion& region, const SurfaceFn& surface,
                            std::vector<FoliageInstance>& out, const std::vector<FoliageInstance>* existing)
{
    ScatterStats st;
    f32 x0, z0, x1, z1;
    BoundsOf(region, x0, z0, x1, z1);
    st.area = region.circle ? kPi * region.radius * region.radius : std::max(0.0f, (x1 - x0) * (z1 - z0));
    if (!(st.area > 0.0f) || !surface) return st;
    Rng rng(p.seed);

    // 1) 候補（外接矩形上）
    std::vector<P2> pts;
    const u32 candCap = std::max<u32>(p.maxCount * 4u, 1024u);
    f32 keepFrac = 1.0f;
    if (p.minSpacing > 1e-4f)
    {
        BridsonPoisson(x0, z0, x1, z1, p.minSpacing, rng, candCap, pts);
        // 最大充填の密度（実測）に対して density が小さければ間引く
        const f32 boxArea = std::max((x1 - x0) * (z1 - z0), 1e-6f);
        const f32 packed = static_cast<f32>(pts.size()) / boxArea;
        if (packed > 1e-6f) keepFrac = std::min(1.0f, p.density / packed);
    }
    else
    {
        JitterGrid(x0, z0, x1, z1, p.density, rng, candCap, pts);
    }
    st.candidates = static_cast<u32>(pts.size());

    // 既存インスタンスとの最小間隔用の格子
    std::unordered_map<u64, std::vector<u32>> exGrid;
    const f32 sp = p.minSpacing;
    if (existing && sp > 1e-4f)
    {
        for (u32 i = 0; i < existing->size(); ++i)
        {
            const FoliageInstance& e = (*existing)[i];
            if (e.px < x0 - sp || e.px > x1 + sp || e.pz < z0 - sp || e.pz > z1 + sp) continue;
            exGrid[CellKey(static_cast<i32>(std::floor(e.px / sp)), static_cast<i32>(std::floor(e.pz / sp)))].push_back(i);
        }
    }

    // variant の累積重み
    u32 vc = std::clamp(p.variantCount, 1u, kMaxVariants);
    f32 wsum = 0.0f;
    for (u32 v = 0; v < vc; ++v) wsum += std::max(p.variantWeight[v], 0.0f);
    if (!(wsum > 0.0f)) { wsum = 1.0f; }

    const f32 minSlope = std::cos(std::min(p.maxSlopeDeg, 90.0f) * kPi / 180.0f);   // ny の下限
    const f32 maxSlope = std::cos(std::max(p.minSlopeDeg, 0.0f) * kPi / 180.0f);    // ny の上限

    for (const P2& c : pts)
    {
        if (st.accepted >= p.maxCount) break;
        if (!InRegion(region, c.x, c.z)) continue;
        // 乱数は「候補ごとに固定個数」引く（採否で消費数が変わると以降の結果が変わってしまうため）
        const f32 rDens = rng.F(), rSplat = rng.F(), rScale = rng.F(), rYaw = rng.F(), rTiltA = rng.F(), rTiltB = rng.F(),
                  rCol1 = rng.F(), rCol2 = rng.F(), rWind = rng.F(), rVar = rng.F();
        SurfaceSample s;
        if (!surface(c.x, c.z, s)) { ++st.rejectedSurface; continue; }
        if (s.y < p.minHeight || s.y > p.maxHeight) { ++st.rejectedHeight; continue; }
        {
            const f32 nl = std::sqrt(s.nx * s.nx + s.ny * s.ny + s.nz * s.nz);
            const f32 ny = nl > 1e-6f ? s.ny / nl : 1.0f;
            if (ny < minSlope || ny > maxSlope + 1e-6f) { ++st.rejectedSlope; continue; }
        }
        if (p.splatLayer >= 0 && p.splatLayer < 4)
        {
            const f32 wgt = s.splat[p.splatLayer];
            if (wgt < p.splatThreshold || rSplat > std::pow(std::max(wgt, 0.0f), std::max(p.splatPower, 1e-3f)))
            { ++st.rejectedSplat; continue; }
        }
        if (rDens >= keepFrac) { ++st.rejectedDensity; continue; }
        if (existing && sp > 1e-4f)
        {
            const i32 cx = static_cast<i32>(std::floor(c.x / sp)), cz = static_cast<i32>(std::floor(c.z / sp));
            bool ok = true;
            for (i32 j = cz - 1; j <= cz + 1 && ok; ++j)
                for (i32 i = cx - 1; i <= cx + 1 && ok; ++i)
                {
                    auto it = exGrid.find(CellKey(i, j));
                    if (it == exGrid.end()) continue;
                    for (u32 ei : it->second)
                    {
                        const FoliageInstance& e = (*existing)[ei];
                        const f32 dx = e.px - c.x, dz = e.pz - c.z;
                        if (dx * dx + dz * dz < sp * sp) { ok = false; break; }
                    }
                }
            if (!ok) { ++st.rejectedSpacing; continue; }
        }

        FoliageInstance in;
        in.px = c.x; in.py = s.y; in.pz = c.z;
        const f32 scale = p.scaleMin + (p.scaleMax - p.scaleMin) * rScale;
        const f32 yaw = rYaw * kTwoPi * std::clamp(p.yawRandom, 0.0f, 1.0f);
        in.rotScale = PackYawScale(yaw, scale);
        // 傾き: 法線への追従 + ランダム
        f32 tx = 0.0f, tz = 0.0f;
        if (p.tiltAlign > 0.0f)
        {
            const f32 nl = std::max(std::sqrt(s.nx * s.nx + s.ny * s.ny + s.nz * s.nz), 1e-6f);
            const f32 nx = s.nx / nl, nz = s.nz / nl;
            tx = std::asin(std::clamp(nz, -1.0f, 1.0f));
            const f32 cxx = std::max(std::cos(tx), 1e-4f);
            tz = std::asin(std::clamp(-nx / cxx, -1.0f, 1.0f));
            tx *= p.tiltAlign; tz *= p.tiltAlign;
        }
        if (p.tiltRandomDeg > 0.0f)
        {
            const f32 rad = p.tiltRandomDeg * kPi / 180.0f;
            tx += (rTiltA * 2.0f - 1.0f) * rad;
            tz += (rTiltB * 2.0f - 1.0f) * rad;
        }
        in.tilt = PackTilt(tx, tz);
        {
            const f32 v = 1.0f - p.colorVariation * rCol1;
            const f32 rr = v * (1.0f - 0.3f * p.colorVariation * rCol2);
            const f32 bb = v * (1.0f - 0.6f * p.colorVariation * rCol2);
            in.color = PackColor8(rr, v, bb, 1.0f);
        }
        u32 type = 0;
        if (vc > 1)
        {
            f32 t = rVar * wsum, acc = 0.0f;
            for (u32 v = 0; v < vc; ++v)
            {
                acc += std::max(p.variantWeight[v], 0.0f);
                type = v;
                if (t < acc) break;
            }
        }
        in.seedType = PackSeedType(rng.Next(), type);
        {
            const f32 ws = 1.0f + p.windVariation * (rWind * 2.0f - 1.0f);
            const u32 q = static_cast<u32>(std::clamp(ws * 128.0f + 0.5f, 1.0f, 255.0f));
            in.params = q;
        }
        out.push_back(in);
        ++st.accepted;
        if (st.accepted >= p.maxCount) break;
    }
    return st;
}

u32 EraseInCircle(std::vector<FoliageInstance>& inst, f32 cx, f32 cz, f32 radius, f32 fraction, u32 seed)
{
    const f32 r2 = radius * radius;
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    size_t w = 0;
    u32 removed = 0;
    for (size_t i = 0; i < inst.size(); ++i)
    {
        const FoliageInstance& in = inst[i];
        const f32 dx = in.px - cx, dz = in.pz - cz;
        bool kill = false;
        if (dx * dx + dz * dz <= r2)
        {
            // 位置とシードから決める（同じ操作を繰り返すと同じものが消える = 決定論）
            const f32 h = Hash01(HashU32(in.seedType ^ seed) + HashU32(static_cast<u32>(in.rotScale)));
            kill = h < fraction;
        }
        if (kill) ++removed;
        else inst[w++] = in;
    }
    inst.resize(w);
    return removed;
}

f32 MinPairDistanceXZ(const std::vector<FoliageInstance>& inst, f32 searchRadius)
{
    if (inst.size() < 2 || !(searchRadius > 0.0f)) return 1e30f;
    std::unordered_map<u64, std::vector<u32>> grid;
    for (u32 i = 0; i < inst.size(); ++i)
        grid[CellKey(static_cast<i32>(std::floor(inst[i].px / searchRadius)),
                     static_cast<i32>(std::floor(inst[i].pz / searchRadius)))].push_back(i);
    f32 best = 1e30f;
    for (u32 i = 0; i < inst.size(); ++i)
    {
        const i32 cx = static_cast<i32>(std::floor(inst[i].px / searchRadius));
        const i32 cz = static_cast<i32>(std::floor(inst[i].pz / searchRadius));
        for (i32 j = cz - 1; j <= cz + 1; ++j)
            for (i32 k = cx - 1; k <= cx + 1; ++k)
            {
                auto it = grid.find(CellKey(k, j));
                if (it == grid.end()) continue;
                for (u32 o : it->second)
                {
                    if (o <= i) continue;
                    const f32 dx = inst[o].px - inst[i].px, dz = inst[o].pz - inst[i].pz;
                    best = std::min(best, std::sqrt(dx * dx + dz * dz));
                }
            }
    }
    return best;
}

} // namespace dx12e::foliage
