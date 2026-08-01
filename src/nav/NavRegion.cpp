// ============================================================================
// ナビメッシュ 段階 3-5: コンパクト化 / 侵食 / 距離場 / 領域分割
// ============================================================================
// - Compact: 歩ける span だけを密に並べ替え、4 方向の隣接を「段差 walkableClimb 以内
//            かつ頭上 walkableHeight 以上」でだけ張る。ここで階段と坂道が繋がる。
// - Erode  : エージェント半径ぶん壁から削る。チャンファ距離変換（軸2 / 斜め3）で
//            近似する。半径を正しく引かないと AI が壁に埋まる経路を引く。
// - Region : 距離場の分水嶺(watershed)で「太いところから水位を下げながら」領域を育てる。
//            monotone は掃引 1 回で速いが形が細長くなる。UI で選べるようにしてある。
// ============================================================================

#include "nav/NavTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dx12e
{
namespace nav
{

#ifdef DX12_NAV_TRACE
#define NAV_TRACE(...) do { std::printf("    [trace] " __VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while (0)
#else
#define NAV_TRACE(...) do {} while (0)
#endif

namespace
{
constexpr i32 kMaxLayers = 63;   // 1 列あたりの span 上限（con の 6bit）
}

// ---------------------------------------------------------------------------
// 段階 3: コンパクトハイトフィールド
// ---------------------------------------------------------------------------
bool BuildCompactHeightfield(const NavBuildConfig& cfg, const NavHeightfield& hf,
                             NavCompactHeightfield& chf, std::string& err)
{
    const i32 walkableHeight = static_cast<i32>(std::ceil(cfg.agentHeight / cfg.cellHeight));
    const i32 walkableClimb  = static_cast<i32>(std::floor(cfg.agentMaxClimb / cfg.cellHeight));

    chf.w = hf.w; chf.h = hf.h;
    chf.walkableHeight = walkableHeight;
    chf.walkableClimb  = walkableClimb;
    chf.borderSize     = 0;
    chf.cs = hf.cs; chf.ch = hf.ch;
    for (i32 k = 0; k < 3; ++k) { chf.bmin[k] = hf.bmin[k]; chf.bmax[k] = hf.bmax[k]; }
    chf.bmax[1] = hf.bmin[1] + static_cast<f32>(0xffff) * hf.ch;

    // 歩ける span を数える。★span リストは AddSpan の統合でつなぎ替えられているので、
    //   壊れていたらここで必ず踏む。範囲外/循環は握り潰さずエラーにする。
    const i32 spanPool = static_cast<i32>(hf.spans.size());
    i32 spanCount = 0;
    for (i32 z = 0; z < hf.h; ++z)
    {
        for (i32 x = 0; x < hf.w; ++x)
        {
            i32 guard = 0;
            for (i32 si = hf.cells[static_cast<size_t>(x) + static_cast<size_t>(z) * hf.w]; si != -1;)
            {
                if (si < 0 || si >= spanPool)
                {
                    NAV_TRACE("BAD span index %d at cell(%d,%d) pool=%d", si, x, z, spanPool);
                    err = "内部エラー: span リストが壊れている";
                    return false;
                }
                if (++guard > spanPool + 1)
                {
                    NAV_TRACE("CYCLE in span list at cell(%d,%d)", x, z);
                    err = "内部エラー: span リストが循環している";
                    return false;
                }
                if (hf.spans[static_cast<size_t>(si)].area != kNullArea) ++spanCount;
                si = hf.spans[static_cast<size_t>(si)].next;
            }
        }
    }

    if (spanCount == 0) { err = "歩ける面が 1 つも無い（傾斜角・段差・エージェント設定を見直す）"; return false; }

    chf.spanCount = spanCount;
    chf.cells.assign(static_cast<size_t>(hf.w) * hf.h, NavCompactCell{});
    chf.spans.assign(spanCount, NavCompactSpan{});
    chf.areas.assign(spanCount, static_cast<u8>(kNullArea));

    u32 idx = 0;
    for (i32 z = 0; z < hf.h; ++z)
        for (i32 x = 0; x < hf.w; ++x)
        {
            const i32 head = hf.cells[x + z * hf.w];
            NavCompactCell& c = chf.cells[x + z * hf.w];
            c.index = idx; c.count = 0;
            for (i32 si = head; si != -1; si = hf.spans[si].next)
            {
                const NavSpan& s = hf.spans[si];
                if (s.area == kNullArea) continue;
                const i32 bot = static_cast<i32>(s.smax);
                const i32 top = (s.next != -1) ? static_cast<i32>(hf.spans[s.next].smin) : 0xffff;
                chf.spans[idx].y = static_cast<u16>(std::clamp(bot, 0, 0xffff));
                chf.spans[idx].h = static_cast<u16>(std::clamp(top - bot, 0, 0xffff));
                chf.areas[idx]   = s.area;
                ++idx; ++c.count;
            }
        }

    // 隣接接続。★ここが「坂道 / 階段が繋がるか」を決める唯一の場所。
    i32 tooManyLayers = 0;
    for (i32 z = 0; z < chf.h; ++z)
        for (i32 x = 0; x < chf.w; ++x)
        {
            const NavCompactCell& c = chf.cells[x + z * chf.w];
            for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
            {
                NavCompactSpan& s = chf.spans[i];
                for (i32 dir = 0; dir < 4; ++dir)
                {
                    SetCon(s, dir, kNotConnected);
                    const i32 nx = x + kDirOffsetX[dir];
                    const i32 nz = z + kDirOffsetZ[dir];
                    if (nx < 0 || nz < 0 || nx >= chf.w || nz >= chf.h) continue;

                    const NavCompactCell& nc = chf.cells[nx + nz * chf.w];
                    for (u32 k = nc.index, nk = nc.index + nc.count; k < nk; ++k)
                    {
                        const NavCompactSpan& ns = chf.spans[k];
                        const i32 bot = (std::max)(static_cast<i32>(s.y), static_cast<i32>(ns.y));
                        const i32 top = (std::min)(static_cast<i32>(s.y) + static_cast<i32>(s.h),
                                                   static_cast<i32>(ns.y) + static_cast<i32>(ns.h));
                        if ((top - bot) < walkableHeight) continue;
                        if (std::abs(static_cast<i32>(ns.y) - static_cast<i32>(s.y)) > walkableClimb) continue;

                        const i32 lidx = static_cast<i32>(k - nc.index);
                        if (lidx < 0 || lidx > kMaxLayers) { ++tooManyLayers; continue; }
                        SetCon(s, dir, static_cast<u32>(lidx));
                        break;
                    }
                }
            }
        }

    if (tooManyLayers > 0)
        err = "警告: 1 列に " + std::to_string(kMaxLayers) +
              " 層を超える場所が " + std::to_string(tooManyLayers) + " 箇所（cellHeight を上げると減る）";
    return true;
}

// ---------------------------------------------------------------------------
// 段階 4: エージェント半径ぶんの侵食
// ---------------------------------------------------------------------------
void ErodeWalkableArea(i32 radius, NavCompactHeightfield& chf)
{
    if (radius <= 0) return;
    const i32 w = chf.w, h = chf.h;
    std::vector<u8> dist(chf.spanCount, 0xff);

    // 境界（歩けない or 4 方向のどれかが繋がっていない）を 0 で種まき
    for (i32 z = 0; z < h; ++z)
        for (i32 x = 0; x < w; ++x)
        {
            const NavCompactCell& c = chf.cells[x + z * w];
            for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
            {
                if (chf.areas[i] == kNullArea) { dist[i] = 0; continue; }
                const NavCompactSpan& s = chf.spans[i];
                i32 nc = 0;
                for (i32 dir = 0; dir < 4; ++dir)
                {
                    if (GetCon(s, dir) == kNotConnected) break;
                    const i32 nx = x + kDirOffsetX[dir];
                    const i32 nz = z + kDirOffsetZ[dir];
                    const u32 ni2 = chf.cells[nx + nz * w].index + GetCon(s, dir);
                    if (chf.areas[ni2] == kNullArea) break;
                    ++nc;
                }
                if (nc != 4) dist[i] = 0;
            }
        }

    auto sweep = [&](bool forward)
    {
        const i32 d0 = forward ? 0 : 2;
        const i32 d1 = forward ? 3 : 1;
        const i32 dd0 = forward ? 3 : 1;   // d0 の隣で更に見る方向
        const i32 dd1 = forward ? 2 : 0;   // d1 の隣で更に見る方向
        for (i32 zi = 0; zi < h; ++zi)
        {
            const i32 z = forward ? zi : (h - 1 - zi);
            for (i32 xi = 0; xi < w; ++xi)
            {
                const i32 x = forward ? xi : (w - 1 - xi);
                const NavCompactCell& c = chf.cells[x + z * w];
                for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
                {
                    const NavCompactSpan& s = chf.spans[i];
                    const i32 dirs[2]  = { d0, d1 };
                    const i32 ddirs[2] = { dd0, dd1 };
                    for (i32 t = 0; t < 2; ++t)
                    {
                        const i32 dir = dirs[t];
                        if (GetCon(s, dir) == kNotConnected) continue;
                        const i32 ax = x + kDirOffsetX[dir];
                        const i32 az = z + kDirOffsetZ[dir];
                        const u32 ai = chf.cells[ax + az * w].index + GetCon(s, dir);
                        if (static_cast<i32>(dist[ai]) + 2 < static_cast<i32>(dist[i]))
                            dist[i] = static_cast<u8>(dist[ai] + 2);

                        const NavCompactSpan& as = chf.spans[ai];
                        const i32 dir2 = ddirs[t];
                        if (GetCon(as, dir2) == kNotConnected) continue;
                        const i32 bx = ax + kDirOffsetX[dir2];
                        const i32 bz = az + kDirOffsetZ[dir2];
                        const u32 bi = chf.cells[bx + bz * w].index + GetCon(as, dir2);
                        if (static_cast<i32>(dist[bi]) + 3 < static_cast<i32>(dist[i]))
                            dist[i] = static_cast<u8>(dist[bi] + 3);
                    }
                }
            }
        }
    };
    sweep(true);
    sweep(false);

    const u8 thr = static_cast<u8>((std::min)(radius * 2, 255));
    for (i32 i = 0; i < chf.spanCount; ++i)
        if (dist[i] < thr) chf.areas[i] = kNullArea;
}

// ---------------------------------------------------------------------------
// 距離場（領域分割の入力）
// ---------------------------------------------------------------------------
namespace
{
void BoxBlurDist(const NavCompactHeightfield& chf, i32 thr, const std::vector<u16>& src,
                 std::vector<u16>& dst)
{
    const i32 w = chf.w, h = chf.h;
    thr *= 2;
    for (i32 z = 0; z < h; ++z)
        for (i32 x = 0; x < w; ++x)
        {
            const NavCompactCell& c = chf.cells[x + z * w];
            for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
            {
                const NavCompactSpan& s = chf.spans[i];
                const u16 cd = src[i];
                if (cd <= thr) { dst[i] = cd; continue; }
                i32 d = cd;
                for (i32 dir = 0; dir < 4; ++dir)
                {
                    if (GetCon(s, dir) == kNotConnected) { d += cd * 2; continue; }
                    const i32 ax = x + kDirOffsetX[dir];
                    const i32 az = z + kDirOffsetZ[dir];
                    const u32 ai = chf.cells[ax + az * w].index + GetCon(s, dir);
                    d += static_cast<i32>(src[ai]);

                    const NavCompactSpan& as = chf.spans[ai];
                    const i32 dir2 = (dir + 1) & 0x3;
                    if (GetCon(as, dir2) == kNotConnected) { d += cd; continue; }
                    const i32 bx = ax + kDirOffsetX[dir2];
                    const i32 bz = az + kDirOffsetZ[dir2];
                    const u32 bi = chf.cells[bx + bz * w].index + GetCon(as, dir2);
                    d += static_cast<i32>(src[bi]);
                }
                dst[i] = static_cast<u16>((d + 5) / 9);
            }
        }
}
} // namespace

void BuildDistanceField(NavCompactHeightfield& chf)
{
    const i32 w = chf.w, h = chf.h;
    std::vector<u16> src(chf.spanCount, 0xffff);
    std::vector<u16> dst(chf.spanCount, 0);

    // 境界 = 4 方向のどれかが「未接続 or 別エリア」
    for (i32 z = 0; z < h; ++z)
        for (i32 x = 0; x < w; ++x)
        {
            const NavCompactCell& c = chf.cells[x + z * w];
            for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
            {
                const NavCompactSpan& s = chf.spans[i];
                const u8 area = chf.areas[i];
                i32 nc = 0;
                for (i32 dir = 0; dir < 4; ++dir)
                {
                    if (GetCon(s, dir) == kNotConnected) break;
                    const i32 ax = x + kDirOffsetX[dir];
                    const i32 az = z + kDirOffsetZ[dir];
                    const u32 ai = chf.cells[ax + az * w].index + GetCon(s, dir);
                    if (area != chf.areas[ai]) break;
                    ++nc;
                }
                if (nc != 4) src[i] = 0;
            }
        }

    auto sweep = [&](bool forward)
    {
        const i32 dirs[2]  = { forward ? 0 : 2, forward ? 3 : 1 };
        const i32 ddirs[2] = { forward ? 3 : 1, forward ? 2 : 0 };
        for (i32 zi = 0; zi < h; ++zi)
        {
            const i32 z = forward ? zi : (h - 1 - zi);
            for (i32 xi = 0; xi < w; ++xi)
            {
                const i32 x = forward ? xi : (w - 1 - xi);
                const NavCompactCell& c = chf.cells[x + z * w];
                for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
                {
                    const NavCompactSpan& s = chf.spans[i];
                    for (i32 t = 0; t < 2; ++t)
                    {
                        const i32 dir = dirs[t];
                        if (GetCon(s, dir) == kNotConnected) continue;
                        const i32 ax = x + kDirOffsetX[dir];
                        const i32 az = z + kDirOffsetZ[dir];
                        const u32 ai = chf.cells[ax + az * w].index + GetCon(s, dir);
                        if (src[ai] + 2 < src[i]) src[i] = static_cast<u16>(src[ai] + 2);

                        const NavCompactSpan& as = chf.spans[ai];
                        const i32 dir2 = ddirs[t];
                        if (GetCon(as, dir2) == kNotConnected) continue;
                        const i32 bx = ax + kDirOffsetX[dir2];
                        const i32 bz = az + kDirOffsetZ[dir2];
                        const u32 bi = chf.cells[bx + bz * w].index + GetCon(as, dir2);
                        if (src[bi] + 3 < src[i]) src[i] = static_cast<u16>(src[bi] + 3);
                    }
                }
            }
        }
    };
    sweep(true);
    sweep(false);

    chf.maxDistance = 0;
    for (i32 i = 0; i < chf.spanCount; ++i)
        chf.maxDistance = (std::max)(src[i], chf.maxDistance);

    BoxBlurDist(chf, 1, src, dst);
    chf.dist.swap(dst);
}

// ---------------------------------------------------------------------------
// 領域: 共通の下請け（連結性の追跡・小領域の除去・併合）
// ---------------------------------------------------------------------------
namespace
{

struct LevelStackEntry
{
    i32 x = 0, z = 0, index = -1;
};

struct RegionInfo
{
    i32  spanCount = 0;
    u16  id = 0;
    u8   areaType = 0;
    bool remap = false, visited = false, overlap = false;
    std::vector<i32> connections;
    std::vector<i32> floors;
};

bool IsSolidEdge(const NavCompactHeightfield& chf, const std::vector<u16>& srcReg,
                 i32 x, i32 z, u32 i, i32 dir)
{
    const NavCompactSpan& s = chf.spans[i];
    u16 r = 0;
    if (GetCon(s, dir) != kNotConnected)
    {
        const i32 ax = x + kDirOffsetX[dir];
        const i32 az = z + kDirOffsetZ[dir];
        const u32 ai = chf.cells[ax + az * chf.w].index + GetCon(s, dir);
        r = srcReg[ai];
    }
    return r != srcReg[i];
}

// 領域の輪郭を 1 周して、接している領域 ID を順に集める（併合の妥当性判定に使う）
void WalkRegionContour(i32 x, i32 z, u32 i, i32 dir,
                       const NavCompactHeightfield& chf, const std::vector<u16>& srcReg,
                       std::vector<i32>& cont)
{
    const i32 w = chf.w;
    const i32 startDir = dir;
    const u32 starti = i;

    u16 curReg = 0;
    {
        const NavCompactSpan& ss = chf.spans[i];
        if (GetCon(ss, dir) != kNotConnected)
        {
            const i32 ax = x + kDirOffsetX[dir];
            const i32 az = z + kDirOffsetZ[dir];
            const u32 ai = chf.cells[ax + az * w].index + GetCon(ss, dir);
            curReg = srcReg[ai];
        }
    }
    cont.push_back(curReg);

    i32 iter = 0;
    while (++iter < 40000)
    {
        const NavCompactSpan& s = chf.spans[i];
        if (IsSolidEdge(chf, srcReg, x, z, i, dir))
        {
            u16 r = 0;
            if (GetCon(s, dir) != kNotConnected)
            {
                const i32 ax = x + kDirOffsetX[dir];
                const i32 az = z + kDirOffsetZ[dir];
                const u32 ai = chf.cells[ax + az * w].index + GetCon(s, dir);
                r = srcReg[ai];
            }
            if (r != curReg) { curReg = r; cont.push_back(curReg); }
            dir = (dir + 1) & 0x3;      // 時計回りに回す
        }
        else
        {
            const i32 nx = x + kDirOffsetX[dir];
            const i32 nz = z + kDirOffsetZ[dir];
            if (GetCon(s, dir) == kNotConnected) return;  // 起きないはず
            const u32 ni = chf.cells[nx + nz * w].index + GetCon(s, dir);
            x = nx; z = nz; i = ni;
            dir = (dir + 3) & 0x3;      // 反時計回りに回す
        }
        if (starti == i && startDir == dir) break;
    }

    // 連続する重複を潰す
    if (cont.size() > 1)
    {
        for (size_t j = 0; j < cont.size();)
        {
            const size_t nj = (j + 1) % cont.size();
            if (cont[j] == cont[nj]) cont.erase(cont.begin() + static_cast<i64>(j));
            else ++j;
        }
    }
}

void RemoveAdjacentNeighbours(RegionInfo& reg)
{
    for (size_t i = 0; i < reg.connections.size() && reg.connections.size() > 1;)
    {
        const size_t ni = (i + 1) % reg.connections.size();
        if (reg.connections[i] == reg.connections[ni])
            reg.connections.erase(reg.connections.begin() + static_cast<i64>(i));
        else ++i;
    }
}

void ReplaceNeighbour(RegionInfo& reg, u16 oldId, u16 newId)
{
    bool changed = false;
    for (i32& c : reg.connections) if (c == static_cast<i32>(oldId)) { c = newId; changed = true; }
    for (i32& f : reg.floors)      if (f == static_cast<i32>(oldId)) { f = newId; }
    if (changed) RemoveAdjacentNeighbours(reg);
}

bool CanMergeWithRegion(const RegionInfo& a, const RegionInfo& b)
{
    if (a.areaType != b.areaType) return false;
    i32 n = 0;
    for (i32 c : a.connections) if (c == static_cast<i32>(b.id)) ++n;
    if (n > 1) return false;                       // 2 箇所で接する = 併合すると穴が開く
    for (i32 f : a.floors) if (f == static_cast<i32>(b.id)) return false;  // 上下に重なっている
    return true;
}

void AddUniqueFloorRegion(RegionInfo& reg, i32 n)
{
    for (i32 f : reg.floors) if (f == n) return;
    reg.floors.push_back(n);
}

bool MergeRegions(RegionInfo& a, RegionInfo& b)
{
    const u16 aid = a.id, bid = b.id;
    const std::vector<i32> acon = a.connections;
    const std::vector<i32>& bcon = b.connections;

    i32 insa = -1;
    for (size_t i = 0; i < acon.size(); ++i) if (acon[i] == static_cast<i32>(bid)) { insa = static_cast<i32>(i); break; }
    if (insa == -1) return false;
    i32 insb = -1;
    for (size_t i = 0; i < bcon.size(); ++i) if (bcon[i] == static_cast<i32>(aid)) { insb = static_cast<i32>(i); break; }
    if (insb == -1) return false;

    a.connections.clear();
    for (size_t i = 0, ni = acon.size(); i + 1 < ni; ++i)
        a.connections.push_back(acon[(static_cast<size_t>(insa) + 1 + i) % ni]);
    for (size_t i = 0, ni = bcon.size(); i + 1 < ni; ++i)
        a.connections.push_back(bcon[(static_cast<size_t>(insb) + 1 + i) % ni]);
    RemoveAdjacentNeighbours(a);

    for (i32 f : b.floors) AddUniqueFloorRegion(a, f);
    a.spanCount += b.spanCount;
    b.spanCount = 0;
    b.connections.clear();
    return true;
}

bool IsRegionConnectedToBorder(const RegionInfo& reg)
{
    for (i32 c : reg.connections) if (c == 0) return true;
    return false;
}

// 小さすぎる領域を捨て、中くらいの領域を隣へ併合する
void MergeAndFilterRegions(i32 minRegionArea, i32 mergeRegionSize, u16& maxRegionId,
                           NavCompactHeightfield& chf, std::vector<u16>& srcReg)
{
    const i32 w = chf.w, h = chf.h;
    const i32 nreg = static_cast<i32>(maxRegionId) + 1;
    std::vector<RegionInfo> regions(static_cast<size_t>(nreg));
    for (i32 i = 0; i < nreg; ++i) regions[static_cast<size_t>(i)].id = static_cast<u16>(i);

    for (i32 z = 0; z < h; ++z)
        for (i32 x = 0; x < w; ++x)
        {
            const NavCompactCell& c = chf.cells[x + z * w];
            for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
            {
                const u16 r = srcReg[i];
                if (r == 0 || r >= static_cast<u16>(nreg)) continue;
                RegionInfo& reg = regions[r];
                reg.spanCount++;

                // 同じ列の別の階（重なり）を記録
                for (u32 j = c.index; j < ni; ++j)
                {
                    if (i == j) continue;
                    const u16 floorId = srcReg[j];
                    if (floorId == 0 || floorId >= static_cast<u16>(nreg)) continue;
                    if (floorId == r) reg.overlap = true;
                    AddUniqueFloorRegion(reg, floorId);
                }

                if (!reg.connections.empty()) continue;
                reg.areaType = chf.areas[i];

                i32 ndir = -1;
                for (i32 dir = 0; dir < 4; ++dir)
                    if (IsSolidEdge(chf, srcReg, x, z, i, dir)) { ndir = dir; break; }
                if (ndir != -1)
                    WalkRegionContour(x, z, i, ndir, chf, srcReg, reg.connections);
            }
        }

    // 1) 小さすぎる連結塊を丸ごと捨てる（外周に触れていない孤島だけ）
    {
        std::vector<i32> stack, trace;
        for (i32 i = 0; i < nreg; ++i)
        {
            RegionInfo& reg = regions[static_cast<size_t>(i)];
            if (reg.id == 0 || (reg.id & kBorderRegion)) continue;
            if (reg.spanCount == 0 || reg.visited) continue;

            bool connectsToBorder = false;
            i32  spanCount = 0;
            stack.clear(); trace.clear();
            reg.visited = true;
            stack.push_back(i);
            while (!stack.empty())
            {
                const i32 ri = stack.back(); stack.pop_back();
                RegionInfo& creg = regions[static_cast<size_t>(ri)];
                spanCount += creg.spanCount;
                trace.push_back(ri);
                for (i32 cn : creg.connections)
                {
                    if (cn & static_cast<i32>(kBorderRegion)) { connectsToBorder = true; continue; }
                    if (cn <= 0 || cn >= nreg) continue;
                    RegionInfo& nreg2 = regions[static_cast<size_t>(cn)];
                    if (nreg2.visited) continue;
                    if (nreg2.id == 0 || (nreg2.id & kBorderRegion)) continue;
                    stack.push_back(nreg2.id);
                    nreg2.visited = true;
                }
            }
            if (spanCount < minRegionArea && !connectsToBorder)
                for (i32 t : trace) { regions[static_cast<size_t>(t)].spanCount = 0;
                                      regions[static_cast<size_t>(t)].id = 0; }
        }
    }

    // 2) 中くらいの領域を一番小さい隣へ併合（細切れを減らして経路を素直にする）
    i32 mergeCount = 0;
    do
    {
        mergeCount = 0;
        for (i32 i = 0; i < nreg; ++i)
        {
            RegionInfo& reg = regions[static_cast<size_t>(i)];
            if (reg.id == 0 || (reg.id & kBorderRegion)) continue;
            if (reg.overlap || reg.spanCount == 0) continue;
            if (reg.spanCount > mergeRegionSize && IsRegionConnectedToBorder(reg)) continue;

            i32 smallest = 0xfffffff;
            u16 mergeId = reg.id;
            for (i32 cn : reg.connections)
            {
                if (cn & static_cast<i32>(kBorderRegion)) continue;
                if (cn <= 0 || cn >= nreg) continue;
                RegionInfo& mreg = regions[static_cast<size_t>(cn)];
                if (mreg.id == 0 || (mreg.id & kBorderRegion) || mreg.overlap) continue;
                if (mreg.spanCount < smallest &&
                    CanMergeWithRegion(reg, mreg) && CanMergeWithRegion(mreg, reg))
                {
                    smallest = mreg.spanCount;
                    mergeId  = mreg.id;
                }
            }
            if (mergeId != reg.id)
            {
                const u16 oldId = reg.id;
                RegionInfo& target = regions[mergeId];
                if (MergeRegions(target, reg))
                {
                    for (i32 j = 0; j < nreg; ++j)
                    {
                        RegionInfo& rj = regions[static_cast<size_t>(j)];
                        if (rj.id == 0 || (rj.id & kBorderRegion)) continue;
                        if (rj.id == oldId) rj.id = mergeId;
                        ReplaceNeighbour(rj, oldId, mergeId);
                    }
                    ++mergeCount;
                }
            }
        }
    } while (mergeCount > 0);

    // 3) ID を詰め直す
    for (i32 i = 0; i < nreg; ++i)
    {
        RegionInfo& r = regions[static_cast<size_t>(i)];
        r.remap = !(r.id == 0 || (r.id & kBorderRegion));
    }
    u16 regIdGen = 0;
    for (i32 i = 0; i < nreg; ++i)
    {
        if (!regions[static_cast<size_t>(i)].remap) continue;
        const u16 oldId = regions[static_cast<size_t>(i)].id;
        const u16 newId = ++regIdGen;
        for (i32 j = i; j < nreg; ++j)
            if (regions[static_cast<size_t>(j)].id == oldId)
            { regions[static_cast<size_t>(j)].id = newId; regions[static_cast<size_t>(j)].remap = false; }
    }
    maxRegionId = regIdGen;

    for (i32 i = 0; i < chf.spanCount; ++i)
    {
        if (srcReg[i] & kBorderRegion) continue;
        if (srcReg[i] >= static_cast<u16>(nreg)) { srcReg[i] = 0; continue; }
        srcReg[i] = regions[srcReg[i]].id;
    }
}

// ---- 分水嶺の下請け --------------------------------------------------------
void SortCellsByLevel(u16 startLevel, const NavCompactHeightfield& chf,
                      const std::vector<u16>& srcReg, i32 nbStacks,
                      std::vector<LevelStackEntry>* stacks, i32 logLevelsPerStack)
{
    const i32 w = chf.w, h = chf.h;
    startLevel = static_cast<u16>(startLevel >> logLevelsPerStack);
    for (i32 j = 0; j < nbStacks; ++j) stacks[j].clear();

    for (i32 z = 0; z < h; ++z)
        for (i32 x = 0; x < w; ++x)
        {
            const NavCompactCell& c = chf.cells[x + z * w];
            for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
            {
                if (chf.areas[i] == kNullArea || srcReg[i] != 0) continue;
                const i32 level = chf.dist[i] >> logLevelsPerStack;
                i32 sId = static_cast<i32>(startLevel) - level;
                if (sId >= nbStacks) continue;
                if (sId < 0) sId = 0;
                stacks[sId].push_back({ x, z, static_cast<i32>(i) });
            }
        }
}

void AppendStacks(const std::vector<LevelStackEntry>& src, std::vector<LevelStackEntry>& dst,
                  const std::vector<u16>& srcReg)
{
    for (const LevelStackEntry& e : src)
    {
        if (e.index < 0 || srcReg[static_cast<size_t>(e.index)] != 0) continue;
        dst.push_back(e);
    }
}

struct DirtyEntry { i32 index; u16 region; u16 distance2; };

void ExpandRegions(i32 maxIter, u16 level, NavCompactHeightfield& chf,
                   std::vector<u16>& srcReg, std::vector<u16>& srcDist,
                   std::vector<LevelStackEntry>& stack, bool fillStack)
{
    const i32 w = chf.w, h = chf.h;
    if (fillStack)
    {
        stack.clear();
        for (i32 z = 0; z < h; ++z)
            for (i32 x = 0; x < w; ++x)
            {
                const NavCompactCell& c = chf.cells[x + z * w];
                for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
                    if (chf.dist[i] >= level && srcReg[i] == 0 && chf.areas[i] != kNullArea)
                        stack.push_back({ x, z, static_cast<i32>(i) });
            }
    }
    else
    {
        for (LevelStackEntry& e : stack)
            if (e.index >= 0 && srcReg[static_cast<size_t>(e.index)] != 0) e.index = -1;
    }

    std::vector<DirtyEntry> dirty;
    i32 iter = 0;
    while (!stack.empty())
    {
        size_t failed = 0;
        dirty.clear();
        for (LevelStackEntry& e : stack)
        {
            if (e.index < 0) { ++failed; continue; }
            const i32 x = e.x, z = e.z;
            const u32 i = static_cast<u32>(e.index);
            u16 r = srcReg[i];
            u16 d2 = 0xffff;
            const u8 area = chf.areas[i];
            const NavCompactSpan& s = chf.spans[i];
            for (i32 dir = 0; dir < 4; ++dir)
            {
                if (GetCon(s, dir) == kNotConnected) continue;
                const i32 ax = x + kDirOffsetX[dir];
                const i32 az = z + kDirOffsetZ[dir];
                const u32 ai = chf.cells[ax + az * w].index + GetCon(s, dir);
                if (chf.areas[ai] != area) continue;
                if (srcReg[ai] > 0 && (srcReg[ai] & kBorderRegion) == 0)
                    if (static_cast<i32>(srcDist[ai]) + 2 < static_cast<i32>(d2))
                    { r = srcReg[ai]; d2 = static_cast<u16>(srcDist[ai] + 2); }
            }
            if (r) { e.index = -1; dirty.push_back({ static_cast<i32>(i), r, d2 }); }
            else ++failed;
        }
        for (const DirtyEntry& d : dirty)
        {
            srcReg[static_cast<size_t>(d.index)]  = d.region;
            srcDist[static_cast<size_t>(d.index)] = d.distance2;
        }
        if (failed == stack.size()) break;
        if (level > 0) { ++iter; if (iter >= maxIter) break; }
    }
}

bool FloodRegion(i32 x, i32 z, u32 i, u16 level, u16 r,
                 NavCompactHeightfield& chf, std::vector<u16>& srcReg,
                 std::vector<u16>& srcDist, std::vector<LevelStackEntry>& stack)
{
    const i32 w = chf.w;
    const u8 area = chf.areas[i];

    stack.clear();
    stack.push_back({ x, z, static_cast<i32>(i) });
    srcReg[i] = r;
    srcDist[i] = 0;

    const u16 lev = static_cast<u16>(level >= 2 ? level - 2 : 0);
    i32 count = 0;

    while (!stack.empty())
    {
        const LevelStackEntry back = stack.back();
        stack.pop_back();
        const i32 cx = back.x, cz = back.z;
        const u32 ci = static_cast<u32>(back.index);
        const NavCompactSpan& cs = chf.spans[ci];

        // 隣に別の領域が既に居るなら、この位置は境界なので取らない
        u16 ar = 0;
        for (i32 dir = 0; dir < 4; ++dir)
        {
            if (GetCon(cs, dir) == kNotConnected) continue;
            const i32 ax = cx + kDirOffsetX[dir];
            const i32 az = cz + kDirOffsetZ[dir];
            const u32 ai = chf.cells[ax + az * w].index + GetCon(cs, dir);
            if (chf.areas[ai] != area) continue;
            const u16 nr = srcReg[ai];
            if (nr & kBorderRegion) continue;
            if (nr != 0 && nr != r) { ar = nr; break; }

            const NavCompactSpan& as = chf.spans[ai];
            const i32 dir2 = (dir + 1) & 0x3;
            if (GetCon(as, dir2) == kNotConnected) continue;
            const i32 bx = ax + kDirOffsetX[dir2];
            const i32 bz = az + kDirOffsetZ[dir2];
            const u32 bi = chf.cells[bx + bz * w].index + GetCon(as, dir2);
            if (chf.areas[bi] != area) continue;
            const u16 nr2 = srcReg[bi];
            if (nr2 != 0 && nr2 != r) { ar = nr2; break; }
        }
        if (ar != 0) { srcReg[ci] = 0; continue; }
        ++count;

        for (i32 dir = 0; dir < 4; ++dir)
        {
            if (GetCon(cs, dir) == kNotConnected) continue;
            const i32 ax = cx + kDirOffsetX[dir];
            const i32 az = cz + kDirOffsetZ[dir];
            const u32 ai = chf.cells[ax + az * w].index + GetCon(cs, dir);
            if (chf.areas[ai] != area) continue;
            if (chf.dist[ai] >= lev && srcReg[ai] == 0)
            {
                srcReg[ai] = r;
                srcDist[ai] = 0;
                stack.push_back({ ax, az, static_cast<i32>(ai) });
            }
        }
    }
    return count > 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 段階 5a: 分水嶺（既定）
// ---------------------------------------------------------------------------
bool BuildRegions(NavCompactHeightfield& chf, i32 borderSize,
                  i32 minRegionArea, i32 mergeRegionArea, std::string& err)
{
    (void)borderSize;
    if (chf.dist.empty()) { err = "距離場が未構築"; return false; }

    std::vector<u16> srcReg(chf.spanCount, 0);
    std::vector<u16> srcDist(chf.spanCount, 0);

    constexpr i32 kLogNbStacks = 3;
    constexpr i32 kNbStacks    = 1 << kLogNbStacks;
    std::vector<LevelStackEntry> lvlStacks[kNbStacks];
    for (auto& s : lvlStacks) s.reserve(256);
    std::vector<LevelStackEntry> stack;
    stack.reserve(256);

    u16 regionId = 1;
    u16 level = static_cast<u16>((chf.maxDistance + 1) & ~1);
    constexpr i32 kExpandIters = 8;

    i32 sId = -1;
    while (level > 0)
    {
        level = static_cast<u16>(level >= 2 ? level - 2 : 0);
        sId = (sId + 1) & (kNbStacks - 1);

        if (sId == 0) SortCellsByLevel(level, chf, srcReg, kNbStacks, lvlStacks, 1);
        else          AppendStacks(lvlStacks[sId - 1], lvlStacks[sId], srcReg);

        ExpandRegions(kExpandIters, level, chf, srcReg, srcDist, lvlStacks[sId], false);

        for (const LevelStackEntry& e : lvlStacks[sId])
        {
            if (e.index < 0) continue;
            const u32 i = static_cast<u32>(e.index);
            if (srcReg[i] != 0) continue;
            if (regionId == 0xffff) { err = "領域数が上限(65535)を超えた"; return false; }
            if (FloodRegion(e.x, e.z, i, level, regionId, chf, srcReg, srcDist, stack))
                ++regionId;
        }
    }

    ExpandRegions(64, 0, chf, srcReg, srcDist, stack, true);

    u16 maxRegionId = static_cast<u16>(regionId > 0 ? regionId - 1 : 0);
    MergeAndFilterRegions(minRegionArea, mergeRegionArea, maxRegionId, chf, srcReg);
    chf.maxRegions = maxRegionId;

    for (i32 i = 0; i < chf.spanCount; ++i) chf.spans[i].reg = srcReg[i];
    return true;
}

// ---------------------------------------------------------------------------
// 段階 5b: monotone（掃引 1 回。速いが細長い領域になりやすい）
// ---------------------------------------------------------------------------
bool BuildRegionsMonotone(NavCompactHeightfield& chf, i32 borderSize,
                          i32 minRegionArea, i32 mergeRegionArea, std::string& err)
{
    (void)borderSize;
    const i32 w = chf.w, h = chf.h;
    std::vector<u16> srcReg(chf.spanCount, 0);

    struct SweepSpan { u16 rid = 0, id = 0, ns = 0, nei = 0; };
    std::vector<SweepSpan> sweeps(static_cast<size_t>(w) + 1);
    constexpr u16 kNullNei = 0xffff;

    u16 id = 1;
    std::vector<i32> prevCount;

    for (i32 z = 0; z < h; ++z)
    {
        prevCount.assign(static_cast<size_t>(id) + 1, 0);
        u16 rid = 1;

        for (i32 x = 0; x < w; ++x)
        {
            const NavCompactCell& c = chf.cells[x + z * w];
            for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
            {
                const NavCompactSpan& s = chf.spans[i];
                if (chf.areas[i] == kNullArea) continue;

                u16 previd = 0;
                if (GetCon(s, 0) != kNotConnected)
                {
                    const i32 ax = x + kDirOffsetX[0];
                    const i32 az = z + kDirOffsetZ[0];
                    const u32 ai = chf.cells[ax + az * w].index + GetCon(s, 0);
                    if ((srcReg[ai] & kBorderRegion) == 0 && chf.areas[i] == chf.areas[ai])
                        previd = srcReg[ai];
                }
                if (!previd)
                {
                    if (rid >= sweeps.size()) { err = "monotone: 行内の領域数が w を超えた"; return false; }
                    previd = rid++;
                    sweeps[previd].rid = previd;
                    sweeps[previd].ns = 0;
                    sweeps[previd].nei = 0;
                }

                if (GetCon(s, 3) != kNotConnected)
                {
                    const i32 ax = x + kDirOffsetX[3];
                    const i32 az = z + kDirOffsetZ[3];
                    const u32 ai = chf.cells[ax + az * w].index + GetCon(s, 3);
                    const u16 nr = srcReg[ai];
                    if (nr && (nr & kBorderRegion) == 0 && chf.areas[i] == chf.areas[ai])
                    {
                        if (!sweeps[previd].nei || sweeps[previd].nei == nr)
                        {
                            sweeps[previd].nei = nr;
                            sweeps[previd].ns++;
                            if (nr < prevCount.size()) prevCount[nr]++;
                        }
                        else sweeps[previd].nei = kNullNei;
                    }
                }
                srcReg[i] = previd;
            }
        }

        for (u16 i = 1; i < rid; ++i)
        {
            if (sweeps[i].nei != kNullNei && sweeps[i].nei != 0 &&
                sweeps[i].nei < prevCount.size() &&
                prevCount[sweeps[i].nei] == static_cast<i32>(sweeps[i].ns))
            {
                sweeps[i].id = sweeps[i].nei;
            }
            else
            {
                if (id == 0xffff) { err = "monotone: 領域数が上限を超えた"; return false; }
                sweeps[i].id = id++;
            }
        }

        for (i32 x = 0; x < w; ++x)
        {
            const NavCompactCell& c = chf.cells[x + z * w];
            for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
                if (srcReg[i] > 0 && srcReg[i] < rid) srcReg[i] = sweeps[srcReg[i]].id;
        }
    }

    u16 maxRegionId = static_cast<u16>(id > 0 ? id - 1 : 0);
    MergeAndFilterRegions(minRegionArea, mergeRegionArea, maxRegionId, chf, srcReg);
    chf.maxRegions = maxRegionId;

    for (i32 i = 0; i < chf.spanCount; ++i) chf.spans[i].reg = srcReg[i];
    return true;
}

} // namespace nav
} // namespace dx12e

