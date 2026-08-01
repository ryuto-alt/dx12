// ============================================================================
// ナビメッシュ 段階 1-2: 三角形のボクセル化 と 歩行面フィルタ
// ============================================================================
// AABB ではなく **実際の三角形** を列（column）ごとに切り刻んで span へ焼く。
// これにより坂道は「連続する高さの span 列」として正しく残り、後段の
// walkableClimb 接続で自然に歩ける面になる。
//
// 三角形の切り方は「まず z のスラブで分割 → 次に x のスラブで分割」の 2 段。
// 1 セルぶんに閉じた凸多角形が得られるので、その y の min/max がその列の span。
// ============================================================================

#include "nav/NavTypes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dx12e
{
namespace nav
{

void NavInputGeometry::ComputeBounds()
{
    if (verts.empty())
    {
        bmin[0] = bmin[1] = bmin[2] = 0.0f;
        bmax[0] = bmax[1] = bmax[2] = 0.0f;
        return;
    }
    for (i32 k = 0; k < 3; ++k) { bmin[k] = verts[k]; bmax[k] = verts[k]; }
    for (size_t i = 3; i + 2 < verts.size(); i += 3)
        for (i32 k = 0; k < 3; ++k)
        {
            bmin[k] = (std::min)(bmin[k], verts[i + k]);
            bmax[k] = (std::max)(bmax[k], verts[i + k]);
        }
}

namespace
{

inline void VCopy(f32* d, const f32* s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }

// クリップ用バッファの 1 面あたりの頂点上限。
// 三角形を z スラブ → x スラブで切ると幾何的には最大 7 頂点だが、
// **軸に完全に揃った入力（床や壁の四角形）では分割線に乗る頂点が両側へ複製される**ため
// 一時的に 7 を超えうる。ここが溢れると x/z のループ変数まで壊れて
// AddSpan が範囲外のセルへ書き込み、ヒープが静かに壊れる（実際に踏んだ）。
// 余裕を持って 12 にし、さらに書き込み時に上限で打ち切る。
constexpr i32 kClipMaxVerts = 12;

// 凸多角形を axis 方向の平面 (= x) で 2 つへ切る。
// out1 = 座標 <= x 側、out2 = 座標 >= x 側。どちらも kClipMaxVerts で打ち切る。
void DividePoly(const f32* in, i32 nin,
                f32* out1, i32* nout1,
                f32* out2, i32* nout2,
                f32 x, i32 axis)
{
    f32 d[kClipMaxVerts + 4];
    nin = (std::min)(nin, kClipMaxVerts);
    for (i32 i = 0; i < nin; ++i) d[i] = x - in[i * 3 + axis];

    i32 m = 0, n = 0;
    auto push1 = [&](const f32* p) { if (m < kClipMaxVerts) { VCopy(out1 + m * 3, p); ++m; } };
    auto push2 = [&](const f32* p) { if (n < kClipMaxVerts) { VCopy(out2 + n * 3, p); ++n; } };

    for (i32 i = 0, j = nin - 1; i < nin; j = i, ++i)
    {
        const bool ina = d[j] >= 0.0f;
        const bool inb = d[i] >= 0.0f;
        if (ina != inb)
        {
            const f32 s = d[j] / (d[j] - d[i]);
            const f32 cut[3] = {
                in[j * 3 + 0] + (in[i * 3 + 0] - in[j * 3 + 0]) * s,
                in[j * 3 + 1] + (in[i * 3 + 1] - in[j * 3 + 1]) * s,
                in[j * 3 + 2] + (in[i * 3 + 2] - in[j * 3 + 2]) * s,
            };
            push1(cut); push2(cut);
            // 分割線そのものに乗る点を両側へ入れないよう厳密比較で振り分ける
            if (d[i] > 0.0f)      push1(in + i * 3);
            else if (d[i] < 0.0f) push2(in + i * 3);
        }
        else
        {
            if (d[i] >= 0.0f)
            {
                push1(in + i * 3);
                if (d[i] != 0.0f) continue;
            }
            push2(in + i * 3);
        }
    }
    *nout1 = m;
    *nout2 = n;
}

// 列 (x,z) へ span を足す。重なる既存 span は 1 本へ潰す。
// 上面の高さ差が flagMergeThr 以内なら area の強い方（歩ける方）を残す
// ＝薄い床が重なっている場所で歩行面が消えない。
void AddSpan(NavHeightfield& hf, i32 x, i32 z, u16 smin, u16 smax, u8 area, i32 flagMergeThr)
{
    const i32 ci = x + z * hf.w;

    NavSpan s;
    s.smin = smin; s.smax = smax; s.area = area; s.next = -1;

    i32 prev = -1;
    i32 cur  = hf.cells[ci];

    while (cur != -1)
    {
        NavSpan& c = hf.spans[cur];
        if (c.smin > s.smax)
        {
            break;                       // 完全に上 → ここへ挿す
        }
        else if (c.smax < s.smin)
        {
            prev = cur; cur = c.next;    // 完全に下 → 次へ
        }
        else
        {
            // 重なっている → 統合して既存を外す
            if (c.smin < s.smin) s.smin = c.smin;
            if (c.smax > s.smax) s.smax = c.smax;
            if (std::abs(static_cast<i32>(s.smax) - static_cast<i32>(c.smax)) <= flagMergeThr)
                s.area = (std::max)(s.area, c.area);

            const i32 nxt = c.next;
            if (prev != -1) hf.spans[prev].next = nxt;
            else            hf.cells[ci]        = nxt;
            cur = nxt;
        }
    }

    const i32 idx = static_cast<i32>(hf.spans.size());
    s.next = cur;
    hf.spans.push_back(s);
    if (prev != -1) hf.spans[prev].next = idx;
    else            hf.cells[ci]        = idx;
}

// 1 三角形をハイトフィールドへ焼く
void RasterizeTri(const f32* v0, const f32* v1, const f32* v2, u8 area,
                  NavHeightfield& hf, f32 ics, f32 ich, i32 flagMergeThr)
{
    const f32* bmin = hf.bmin;
    const f32  by   = hf.bmax[1] - hf.bmin[1];

    f32 tmin[3], tmax[3];
    for (i32 k = 0; k < 3; ++k)
    {
        tmin[k] = (std::min)(v0[k], (std::min)(v1[k], v2[k]));
        tmax[k] = (std::max)(v0[k], (std::max)(v1[k], v2[k]));
    }
    // XZ で AABB が外れていれば捨てる（y は後でクランプするので見ない）
    if (tmax[0] < hf.bmin[0] || tmin[0] > hf.bmax[0]) return;
    if (tmax[2] < hf.bmin[2] || tmin[2] > hf.bmax[2]) return;

    i32 z0 = static_cast<i32>((tmin[2] - bmin[2]) * ics);
    i32 z1 = static_cast<i32>((tmax[2] - bmin[2]) * ics);
    if (z1 < 0 || z0 >= hf.h) return;
    z0 = std::clamp(z0, -1, hf.h - 1);
    z1 = std::clamp(z1, 0, hf.h - 1);

    // 4 バッファを回して使う（in / inrow / p1 / p2）。上限は kClipMaxVerts。
    f32 buf[kClipMaxVerts * 3 * 4];
    f32* in    = buf;
    f32* inrow = buf + kClipMaxVerts * 3;
    f32* p1    = inrow + kClipMaxVerts * 3;
    f32* p2    = p1 + kClipMaxVerts * 3;

    VCopy(in + 0, v0); VCopy(in + 3, v1); VCopy(in + 6, v2);
    i32 nvRow = 0, nvIn = 3;

    for (i32 z = z0; z <= z1; ++z)
    {
        const f32 cellZ = bmin[2] + static_cast<f32>(z) * hf.cs;
        DividePoly(in, nvIn, inrow, &nvRow, p1, &nvIn, cellZ + hf.cs, 2);
        std::swap(in, p1);
        if (nvRow < 3) continue;
        if (z < 0) continue;

        f32 minX = inrow[0], maxX = inrow[0];
        for (i32 i = 1; i < nvRow; ++i)
        {
            minX = (std::min)(minX, inrow[i * 3]);
            maxX = (std::max)(maxX, inrow[i * 3]);
        }
        i32 x0 = static_cast<i32>((minX - bmin[0]) * ics);
        i32 x1 = static_cast<i32>((maxX - bmin[0]) * ics);
        if (x1 < 0 || x0 >= hf.w) continue;
        x0 = std::clamp(x0, -1, hf.w - 1);
        x1 = std::clamp(x1, 0, hf.w - 1);

        i32 nv = 0, nv2 = nvRow;
        for (i32 x = x0; x <= x1; ++x)
        {
            const f32 cellX = bmin[0] + static_cast<f32>(x) * hf.cs;
            DividePoly(inrow, nv2, p1, &nv, p2, &nv2, cellX + hf.cs, 0);
            std::swap(inrow, p2);
            if (nv < 3) continue;
            if (x < 0) continue;

            f32 smin = p1[1], smax = p1[1];
            for (i32 i = 1; i < nv; ++i)
            {
                smin = (std::min)(smin, p1[i * 3 + 1]);
                smax = (std::max)(smax, p1[i * 3 + 1]);
            }
            smin -= bmin[1];
            smax -= bmin[1];
            if (smax < 0.0f) continue;
            if (smin > by)   continue;
            if (smin < 0.0f) smin = 0.0f;
            if (smax > by)   smax = by;

            i32 ismin = std::clamp(static_cast<i32>(std::floor(smin * ich)), 0, 0xfffe);
            i32 ismax = std::clamp(static_cast<i32>(std::ceil(smax * ich)), ismin + 1, 0xffff);
            AddSpan(hf, x, z, static_cast<u16>(ismin), static_cast<u16>(ismax),
                    area, flagMergeThr);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 段階 1: ラスタライズ
// ---------------------------------------------------------------------------
bool RasterizeInput(const NavInputGeometry& geom, const NavBuildConfig& cfg,
                    NavHeightfield& hf, std::string& err)
{
    if (geom.Empty()) { err = "入力ジオメトリが空（歩ける三角形が1枚も無い）"; return false; }
    if (cfg.cellSize <= 0.0f || cfg.cellHeight <= 0.0f)
    {
        err = "cellSize / cellHeight は正の値でなければならない";
        return false;
    }

    f32 bmin[3], bmax[3];
    if (cfg.useBounds)
    {
        for (i32 k = 0; k < 3; ++k) { bmin[k] = cfg.boundsMin[k]; bmax[k] = cfg.boundsMax[k]; }
    }
    else
    {
        for (i32 k = 0; k < 3; ++k) { bmin[k] = geom.bmin[k]; bmax[k] = geom.bmax[k]; }
    }
    // 端が欠けないよう少し広げる（エージェント半径ぶん + 1 セル）
    const f32 pad = cfg.agentRadius + cfg.cellSize * 2.0f;
    bmin[0] -= pad; bmin[2] -= pad;
    bmax[0] += pad; bmax[2] += pad;
    bmin[1] -= cfg.cellHeight * 2.0f;
    bmax[1] += cfg.agentHeight + cfg.cellHeight * 2.0f;

    if (bmax[0] <= bmin[0] || bmax[2] <= bmin[2]) { err = "ビルド範囲が空"; return false; }

    hf.cs = cfg.cellSize;
    hf.ch = cfg.cellHeight;
    for (i32 k = 0; k < 3; ++k) { hf.bmin[k] = bmin[k]; hf.bmax[k] = bmax[k]; }
    hf.w = static_cast<i32>((bmax[0] - bmin[0]) / hf.cs + 0.5f);
    hf.h = static_cast<i32>((bmax[2] - bmin[2]) / hf.cs + 0.5f);
    if (hf.w <= 0 || hf.h <= 0) { err = "ビルド範囲がセルサイズより小さい"; return false; }

    // 暴走ガード: 4096^2 セル（= 1.6 千万列）を超えたら cellSize を上げてもらう
    const i64 cellTotal = static_cast<i64>(hf.w) * static_cast<i64>(hf.h);
    if (cellTotal > 16'000'000LL)
    {
        err = "ボクセル数が多すぎる（" + std::to_string(hf.w) + "x" + std::to_string(hf.h) +
              "）。cellSize を上げるかビルド範囲を絞ること";
        return false;
    }

    hf.cells.assign(static_cast<size_t>(cellTotal), -1);
    hf.spans.clear();
    hf.spans.reserve(static_cast<size_t>(cellTotal / 4 + 64));

    const f32 ics = 1.0f / hf.cs;
    const f32 ich = 1.0f / hf.ch;
    const i32 walkableClimbVox = static_cast<i32>(std::floor(cfg.agentMaxClimb / cfg.cellHeight));
    const f32 slopeCos = std::cos(cfg.agentMaxSlope * 3.14159265358979323846f / 180.0f);

    const i32 triCount = geom.TriCount();
    for (i32 t = 0; t < triCount; ++t)
    {
        const i32 i0 = geom.tris[t * 3 + 0];
        const i32 i1 = geom.tris[t * 3 + 1];
        const i32 i2 = geom.tris[t * 3 + 2];
        const f32* v0 = &geom.verts[static_cast<size_t>(i0) * 3];
        const f32* v1 = &geom.verts[static_cast<size_t>(i1) * 3];
        const f32* v2 = &geom.verts[static_cast<size_t>(i2) * 3];

        // 面法線から傾斜判定。★これが「坂道を歩けるようにする」本体。
        // ここは **|n.y| で判定する**（Recast は n.y > 閾値 で巻き方向に依存する）。
        // エンジンには利用者が持ち込む任意のメッシュが入り、裏返った面や
        // 両面ポリゴンが普通にある。符号を見ると「ナビメッシュが空になる」事故が
        // 起きるうえ原因も分かりにくい。ボクセル化は span の【上面】を歩行面に
        // 採るので、閉じた立体でも薄板でも |n.y| で困らない。
        const f32 e0[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
        const f32 e1[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };
        f32 n[3] = { e0[1] * e1[2] - e0[2] * e1[1],
                     e0[2] * e1[0] - e0[0] * e1[2],
                     e0[0] * e1[1] - e0[1] * e1[0] };
        const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-12f) { n[0] /= len; n[1] /= len; n[2] /= len; }
        const u8 area = (std::fabs(n[1]) > slopeCos) ? static_cast<u8>(kWalkableArea)
                                                     : static_cast<u8>(kNullArea);

        RasterizeTri(v0, v1, v2, area, hf, ics, ich, walkableClimbVox);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 段階 2: フィルタ
// ---------------------------------------------------------------------------
namespace
{
constexpr i32 kDirX[4] = { -1, 0, 1, 0 };
constexpr i32 kDirZ[4] = { 0, 1, 0, -1 };

// 低い障害物（縁石など）を「またげる」ものとして歩行可能にする
void FilterLowHangingWalkableObstacles(i32 walkableClimb, NavHeightfield& hf)
{
    for (i32 z = 0; z < hf.h; ++z)
        for (i32 x = 0; x < hf.w; ++x)
        {
            i32 prev = -1;
            bool prevWalkable = false;
            u8   prevArea = kNullArea;
            for (i32 si = hf.cells[x + z * hf.w]; si != -1; si = hf.spans[si].next)
            {
                NavSpan& s = hf.spans[si];
                const bool walkable = s.area != kNullArea;
                if (!walkable && prevWalkable && prev != -1)
                {
                    if (std::abs(static_cast<i32>(s.smax) -
                                 static_cast<i32>(hf.spans[prev].smax)) <= walkableClimb)
                        s.area = prevArea;
                }
                prevWalkable = walkable;
                prevArea = walkable ? s.area : prevArea;
                prev = si;
            }
        }
}

// 崖ぎわ（隣が walkableClimb を超えて落ちている）を歩行不可にする。
// これが無いと AI が崖から落ちる経路を平気で引く。
void FilterLedgeSpans(i32 walkableHeight, i32 walkableClimb, NavHeightfield& hf)
{
    const i32 MAXH = 0xffff;
    for (i32 z = 0; z < hf.h; ++z)
        for (i32 x = 0; x < hf.w; ++x)
            for (i32 si = hf.cells[x + z * hf.w]; si != -1; si = hf.spans[si].next)
            {
                NavSpan& s = hf.spans[si];
                if (s.area == kNullArea) continue;

                const i32 bot = static_cast<i32>(s.smax);
                const i32 top = (s.next != -1) ? static_cast<i32>(hf.spans[s.next].smin) : MAXH;

                i32 minNeighborDrop = MAXH;
                i32 accessibleMin = bot, accessibleMax = bot;

                for (i32 dir = 0; dir < 4; ++dir)
                {
                    const i32 nx = x + kDirX[dir];
                    const i32 nz = z + kDirZ[dir];
                    if (nx < 0 || nz < 0 || nx >= hf.w || nz >= hf.h)
                    {
                        minNeighborDrop = (std::min)(minNeighborDrop, -walkableClimb - bot);
                        break;
                    }

                    // 隣の列の最下 span より下（＝奈落）も候補に入れる
                    i32 nbot = -walkableClimb;
                    i32 nsi  = hf.cells[nx + nz * hf.w];
                    i32 ntop = (nsi != -1) ? static_cast<i32>(hf.spans[nsi].smin) : MAXH;
                    if ((std::min)(top, ntop) - (std::max)(bot, nbot) > walkableHeight)
                        minNeighborDrop = (std::min)(minNeighborDrop, nbot - bot);

                    for (; nsi != -1; nsi = hf.spans[nsi].next)
                    {
                        const NavSpan& ns = hf.spans[nsi];
                        nbot = static_cast<i32>(ns.smax);
                        ntop = (ns.next != -1) ? static_cast<i32>(hf.spans[ns.next].smin) : MAXH;
                        if ((std::min)(top, ntop) - (std::max)(bot, nbot) <= walkableHeight)
                            continue;
                        minNeighborDrop = (std::min)(minNeighborDrop, nbot - bot);
                        if (std::abs(nbot - bot) <= walkableClimb)
                        {
                            accessibleMin = (std::min)(accessibleMin, nbot);
                            accessibleMax = (std::max)(accessibleMax, nbot);
                        }
                    }
                }

                if (minNeighborDrop < -walkableClimb)
                    s.area = kNullArea;                       // 崖から落ちる
                else if ((accessibleMax - accessibleMin) > walkableClimb)
                    s.area = kNullArea;                       // 段差が急すぎる縁
            }
}

// 頭がつかえる面（天井が近い）を歩行不可にする
void FilterWalkableLowHeightSpans(i32 walkableHeight, NavHeightfield& hf)
{
    const i32 MAXH = 0xffff;
    for (i32 z = 0; z < hf.h; ++z)
        for (i32 x = 0; x < hf.w; ++x)
            for (i32 si = hf.cells[x + z * hf.w]; si != -1; si = hf.spans[si].next)
            {
                NavSpan& s = hf.spans[si];
                const i32 bot = static_cast<i32>(s.smax);
                const i32 top = (s.next != -1) ? static_cast<i32>(hf.spans[s.next].smin) : MAXH;
                if ((top - bot) < walkableHeight) s.area = kNullArea;
            }
}
} // namespace

void FilterHeightfield(const NavBuildConfig& cfg, NavHeightfield& hf)
{
    const i32 walkableHeight = static_cast<i32>(std::ceil(cfg.agentHeight / cfg.cellHeight));
    const i32 walkableClimb  = static_cast<i32>(std::floor(cfg.agentMaxClimb / cfg.cellHeight));

    // 順序に意味がある: またぎ → 崖 → 頭上。
    // 先に頭上を落とすと「またげる低障害物の上」が消えてしまう。
    if (cfg.filterLowHanging) FilterLowHangingWalkableObstacles(walkableClimb, hf);
    if (cfg.filterLedgeSpans) FilterLedgeSpans(walkableHeight, walkableClimb, hf);
    FilterWalkableLowHeightSpans(walkableHeight, hf);
}

} // namespace nav
} // namespace dx12e
