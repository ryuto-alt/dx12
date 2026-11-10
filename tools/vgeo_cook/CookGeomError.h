#pragma once
//
// CookGeomError.h ― 簡略化 1 回ぶんの「幾何誤差」（メートル）を測る純関数。ヘッダオンリー・標準ライブラリのみ。
//
//   なぜ要るか（2026-11-10 の実測）:
//     meshopt_simplifyWithAttributes の result_error は「位置 + 属性（法線・UV）」を混ぜた誤差で、単位がメートルではない。
//     属性の重み 0.25 のとき、524 万三角形の岩（半径 1.2 m）のルートの lodError が 6.5 m になっていた（岩より大きい）。
//     ランタイムはこれを画面 px に射影して LOD を選ぶので、遠景でも 1 画素あたり数十三角形を描いていた（vg_stats の被覆画素で 69 tri/px）。
//     LOD の判定に使う誤差は「形がどれだけずれたか」だけでよい（UE の Nanite も位置の誤差で判定する）。
//
//   測り方（サンプリングした両方向のハウスドルフ距離）:
//     A: 入力（簡略化前）の頂点 → 出力（簡略化後）の三角形面までの最短距離
//     B: 出力の三角形の重心と 3 辺の中点 → 入力の三角形面までの最短距離
//     誤差 = A と B の最大。簡略化は頂点を動かさない（出力の頂点は入力の頂点の部分集合）ので、A は「消えた頂点がどれだけ面から離れたか」、
//     B は「新しい大きな三角形が元の面からどれだけ浮いた / 沈んだか」を拾う。
//   速さ: 求めるのは「各点の最短距離」の最大なので、今までの最大以下の距離が 1 つ見つかった点はそこで打ち切る（Nearest2 の stopBelow2）。
//     三角形を一様格子（1 辺 ≤ 48 セル、三角形の AABB が掛かる全セルへ登録）に入れ、点の属するセルから同心のリングで探す。
//     リング r まで見て最短距離 ≤ r * (セルの最短辺) なら打ち切る（三角形の最近点はその三角形の登録セルのどれかに入るので、
//     未探索のセルの三角形は少なくともその距離だけ離れている）。
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dx12e::vg::cook::geomerr
{

// 点 p から三角形 (a, b, c) への最短距離の二乗（Ericson, Real-Time Collision Detection §5.1.5 の最近点）。
inline float PointTriDist2(const float* p, const float* a, const float* b, const float* c)
{
    auto sub = [](const float* x, const float* y, float* o) { o[0] = x[0] - y[0]; o[1] = x[1] - y[1]; o[2] = x[2] - y[2]; };
    auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    float ab[3], ac[3], ap[3];
    sub(b, a, ab); sub(c, a, ac); sub(p, a, ap);
    float q[3];
    auto at = [&](float u, float v)   // a + u*ab + v*ac
    {
        q[0] = a[0] + u * ab[0] + v * ac[0];
        q[1] = a[1] + u * ab[1] + v * ac[1];
        q[2] = a[2] + u * ab[2] + v * ac[2];
    };
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) { q[0] = a[0]; q[1] = a[1]; q[2] = a[2]; }
    else
    {
        float bp[3];
        sub(p, b, bp);
        const float d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) { q[0] = b[0]; q[1] = b[1]; q[2] = b[2]; }
        else
        {
            const float vc = d1 * d4 - d3 * d2;
            float cp[3];
            sub(p, c, cp);
            const float d5 = dot(ab, cp), d6 = dot(ac, cp);
            const float vb = d5 * d2 - d1 * d6;
            const float va = d3 * d6 - d5 * d4;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) { const float den = d1 - d3; at(den != 0.0f ? d1 / den : 0.0f, 0.0f); }
            else if (d6 >= 0.0f && d5 <= d6) { q[0] = c[0]; q[1] = c[1]; q[2] = c[2]; }
            else if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) { const float den = d2 - d6; at(0.0f, den != 0.0f ? d2 / den : 0.0f); }
            else if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
            {
                const float den = (d4 - d3) + (d5 - d6);
                const float w = den != 0.0f ? (d4 - d3) / den : 0.0f;
                q[0] = b[0] + w * (c[0] - b[0]); q[1] = b[1] + w * (c[1] - b[1]); q[2] = b[2] + w * (c[2] - b[2]);
            }
            else
            {
                const float s = va + vb + vc;
                if (!(std::fabs(s) > 0.0f))
                {
                    // 縮退三角形: 3 辺の線分の最短
                    float best = 3.4e38f;
                    const float* e[3][2] = {{a, b}, {b, c}, {c, a}};
                    for (auto& seg : e)
                    {
                        float d[3], w[3];
                        sub(seg[1], seg[0], d); sub(p, seg[0], w);
                        const float dd = dot(d, d);
                        const float t = dd > 0.0f ? std::clamp(dot(w, d) / dd, 0.0f, 1.0f) : 0.0f;
                        const float r[3] = {w[0] - t * d[0], w[1] - t * d[1], w[2] - t * d[2]};
                        best = std::min(best, dot(r, r));
                    }
                    return best;
                }
                const float inv = 1.0f / s;
                at(vb * inv, vc * inv);
            }
        }
    }
    const float r[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
    return dot(r, r);
}

// 三角形の一様格子（CSR）。pos は 3 * nv、idx は 3 * nTri（pos への添字）。
struct TriGrid
{
    static constexpr int kMaxRes = 48;
    const float* pos = nullptr;
    const uint32_t* idx = nullptr;
    float lo[3] = {0, 0, 0}, cell[3] = {1, 1, 1}, invCell[3] = {1, 1, 1}, minCell = 1;
    int n[3] = {1, 1, 1};
    std::vector<uint32_t> start, items;   // start: セル数 + 1

    void Build(const float* p, const uint32_t* ix, size_t nTri, const float boxLo[3], const float boxHi[3])
    {
        pos = p; idx = ix;
        // グループはほぼ面（2 次元）なので、長い軸のセル数を sqrt(三角形数) にすると 1 セル ≈ 1〜2 三角形になる
        const int res = std::clamp(static_cast<int>(std::sqrt(static_cast<double>(nTri))) + 1, 1, kMaxRes);
        float ext[3];
        float maxExt = 0.0f;
        for (int k = 0; k < 3; ++k) { ext[k] = std::max(boxHi[k] - boxLo[k], 0.0f); maxExt = std::max(maxExt, ext[k]); }
        if (!(maxExt > 0.0f)) maxExt = 1.0f;
        const float target = maxExt / static_cast<float>(res);
        minCell = 3.4e38f;
        for (int k = 0; k < 3; ++k)
        {
            n[k] = std::clamp(static_cast<int>(std::ceil(ext[k] / target)), 1, kMaxRes);
            lo[k] = boxLo[k];
            cell[k] = ext[k] > 0.0f ? ext[k] / static_cast<float>(n[k]) : target;
            invCell[k] = 1.0f / cell[k];
            // 打ち切りの下界は「リングの外にセルがある軸」の幅だけで決まる（セルが 1 列しかない軸はリングが広がらない）。
            //   ★薄い面（グループはほぼ平らな面）で 1 列の軸の幅（ほぼ 0）を使うと打ち切れず、全セルを総当たりして 20 倍遅くなった。
            if (n[k] > 1) minCell = std::min(minCell, cell[k]);
        }
        if (!(minCell < 3.0e38f)) minCell = target;
        const size_t cells = static_cast<size_t>(n[0]) * n[1] * n[2];
        start.assign(cells + 1, 0);
        auto range = [&](size_t t, int c0[3], int c1[3])
        {
            float tl[3] = {3.4e38f, 3.4e38f, 3.4e38f}, th[3] = {-3.4e38f, -3.4e38f, -3.4e38f};
            for (int v = 0; v < 3; ++v)
                for (int k = 0; k < 3; ++k) { const float x = pos[static_cast<size_t>(idx[t * 3 + v]) * 3 + k]; tl[k] = std::min(tl[k], x); th[k] = std::max(th[k], x); }
            for (int k = 0; k < 3; ++k) { c0[k] = Clampi(Coord(tl[k], k), k); c1[k] = Clampi(Coord(th[k], k), k); }
        };
        for (int pass = 0; pass < 2; ++pass)
        {
            if (pass == 1)
            {
                for (size_t i = 0; i < cells; ++i) start[i + 1] += start[i];
                items.assign(start[cells], 0);
            }
            std::vector<uint32_t> fill;
            if (pass == 1) fill.assign(start.begin(), start.end() - 1);
            for (size_t t = 0; t < nTri; ++t)
            {
                int c0[3], c1[3];
                range(t, c0, c1);
                for (int z = c0[2]; z <= c1[2]; ++z)
                    for (int y = c0[1]; y <= c1[1]; ++y)
                        for (int x = c0[0]; x <= c1[0]; ++x)
                        {
                            const size_t ci = (static_cast<size_t>(z) * n[1] + y) * n[0] + x;
                            if (pass == 0) ++start[ci + 1];
                            else items[fill[ci]++] = static_cast<uint32_t>(t);
                        }
            }
        }
    }
    int Coord(float x, int k) const { return static_cast<int>(std::floor((x - lo[k]) * invCell[k])); }
    int Clampi(int c, int k) const { return std::clamp(c, 0, n[k] - 1); }

    // 点 p から最も近い三角形面までの距離の二乗。三角形が無ければ +大。
    // stopBelow2: これ以下の距離が 1 つ見つかった時点で打ち切ってその値を返す（「最短距離の最大」を求めるときは、
    //   今までの最大以下と分かった点は最大を更新しないので、正確な最短距離は要らない）。0 で正確な最短距離。
    float Nearest2(const float* p, float stopBelow2 = 0.0f) const
    {
        if (items.empty()) return 3.4e38f;
        int b[3];
        for (int k = 0; k < 3; ++k) b[k] = Clampi(Coord(p[k], k), k);
        // 格子の外にある点は、格子までの距離ぶん打ち切りの下界が甘くなるだけ（正しさは変わらない）
        float best = 3.4e38f;
        int maxR = 0;   // このリングで全セルを見終わる
        for (int k = 0; k < 3; ++k) maxR = std::max(maxR, std::max(b[k], n[k] - 1 - b[k]));
        for (int r = 0; r <= maxR; ++r)
        {
            for (int z = b[2] - r; z <= b[2] + r; ++z)
            {
                if (z < 0 || z >= n[2]) continue;
                for (int y = b[1] - r; y <= b[1] + r; ++y)
                {
                    if (y < 0 || y >= n[1]) continue;
                    const bool shellYZ = (z == b[2] - r || z == b[2] + r || y == b[1] - r || y == b[1] + r);
                    for (int x = b[0] - r; x <= b[0] + r; ++x)
                    {
                        if (x < 0 || x >= n[0]) continue;
                        if (!shellYZ && x != b[0] - r && x != b[0] + r) continue;   // リングの殻だけ
                        const size_t ci = (static_cast<size_t>(z) * n[1] + y) * n[0] + x;
                        for (uint32_t s = start[ci]; s < start[ci + 1]; ++s)
                        {
                            const size_t t = items[s];
                            const float d = PointTriDist2(p, pos + static_cast<size_t>(idx[t * 3]) * 3, pos + static_cast<size_t>(idx[t * 3 + 1]) * 3,
                                                          pos + static_cast<size_t>(idx[t * 3 + 2]) * 3);
                            if (d < best) { best = d; if (best <= stopBelow2) return best; }
                        }
                    }
                }
            }
            const float bound = static_cast<float>(r) * minCell;
            if (best <= bound * bound) break;
        }
        return best;
    }
};

// 簡略化 1 回ぶんの幾何誤差（メートル）。pos = 3 * nv（入力と出力で共通の頂点配列）、inIdx / outIdx = 三角形リスト。
// 出力が空なら入力の境界箱の対角の半分（グループが丸ごと消えるのに相当）を返す。
inline float SimplifyGeomError(const float* pos, size_t nv, const uint32_t* inIdx, size_t inCount, const uint32_t* outIdx, size_t outCount)
{
    if (inCount < 3 || nv == 0) return 0.0f;
    float lo[3] = {3.4e38f, 3.4e38f, 3.4e38f}, hi[3] = {-3.4e38f, -3.4e38f, -3.4e38f};
    for (size_t v = 0; v < nv; ++v)
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], pos[v * 3 + k]); hi[k] = std::max(hi[k], pos[v * 3 + k]); }
    if (outCount < 3)
    {
        const float d[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
        return 0.5f * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    const size_t nIn = inCount / 3, nOut = outCount / 3;
    float worst = 0.0f;
    // A: 入力の頂点（入力の三角形が参照するもの）→ 出力の面
    {
        TriGrid g;
        g.Build(pos, outIdx, nOut, lo, hi);
        std::vector<uint8_t> seen(nv, 0);
        for (size_t i = 0; i < nOut * 3; ++i) seen[outIdx[i]] = 1;   // 出力にも残った頂点は距離 0（簡略化は頂点を動かさない）
        for (size_t i = 0; i < nIn * 3; ++i)
        {
            const uint32_t v = inIdx[i];
            if (seen[v]) continue;
            seen[v] = 1;
            worst = std::max(worst, g.Nearest2(pos + static_cast<size_t>(v) * 3, worst));
        }
    }
    // B: 出力の三角形の重心と 3 辺の中点 → 入力の面
    {
        TriGrid g;
        g.Build(pos, inIdx, nIn, lo, hi);
        for (size_t t = 0; t < nOut; ++t)
        {
            const float* a = pos + static_cast<size_t>(outIdx[t * 3]) * 3;
            const float* b = pos + static_cast<size_t>(outIdx[t * 3 + 1]) * 3;
            const float* c = pos + static_cast<size_t>(outIdx[t * 3 + 2]) * 3;
            float s[4][3];
            for (int k = 0; k < 3; ++k)
            {
                s[0][k] = (a[k] + b[k] + c[k]) * (1.0f / 3.0f);
                s[1][k] = 0.5f * (a[k] + b[k]);
                s[2][k] = 0.5f * (b[k] + c[k]);
                s[3][k] = 0.5f * (c[k] + a[k]);
            }
            for (auto& q : s) worst = std::max(worst, g.Nearest2(q, worst));
        }
    }
    return std::sqrt(worst);
}

} // namespace dx12e::vg::cook::geomerr
