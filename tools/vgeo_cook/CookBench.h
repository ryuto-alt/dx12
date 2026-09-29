#pragma once
//
// CookBench.h ― 巨大ベンチメッシュの手続き生成（合成データのみ。VGSRC で出す）。設計書 §5 のベンチシーンの素材。
//
//   kind = blob : fBm ノイズで変位した球（岩のような凸凹）。P0 の vgeo_stub の blob と同系統。
//          knot : ねじれた (2,3) トーラス結び目（断面が 5 山に波打ち、管が 3 回ねじれる + 細かいノイズ）。
//          rock : 異方スケールの球 + リッジノイズ（稜線のある岩）+ 平らな底面。
//          torus / sphere / grid : 素直な形状（テスト・比較用）
//   テッセレーションを上げて任意の三角形数（100 万 / 1000 万 / 5000 万 …）にできる。同じ (kind, tris, seed) → 同じデータ。
//   頂点は行ごとにスレッド並列で評価する（結果はスレッド数に依らない）。法線は解析ではなく数値微分（外向きに揃える）。
//   継ぎ目（u / v の巻き戻り）の頂点は位置が完全に一致する（同じ整数格子から評価するため）。UV は左上原点。
//
#include "CookUtil.h"
#include "VgeoCook.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

namespace dx12e::vg::cook
{

struct BenchOptions
{
    std::string kind = "blob";
    u64 tris = 1000000;
    u32 seed = 1;
    u32 materials = 1;             // 行方向に等分して材質セクションを作る（材質境界のテスト用）
    u32 threads = 0;               // 0 = 既定
    bool lowPriority = true;
};

namespace bench_detail
{
inline f32 Hash01(u32 x, u32 y, u32 z, u32 seed)
{
    u32 h = x * 0x9E3779B1u ^ (y + 0x7F4A7C15u) * 0x85EBCA77u ^ (z + 0x165667B1u) * 0xC2B2AE3Du ^ seed * 0x27D4EB2Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return static_cast<f32>(h >> 8) / 16777216.0f;
}
inline f32 ValueNoise(f32 x, f32 y, f32 z, u32 seed)
{
    const f32 fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const i32 ix = static_cast<i32>(fx), iy = static_cast<i32>(fy), iz = static_cast<i32>(fz);
    const f32 tx = x - fx, ty = y - fy, tz = z - fz;
    const f32 sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty), sz = tz * tz * (3 - 2 * tz);
    auto h = [&](i32 a, i32 b, i32 c) { return Hash01(static_cast<u32>(ix + a), static_cast<u32>(iy + b), static_cast<u32>(iz + c), seed); };
    const f32 x00 = h(0, 0, 0) + (h(1, 0, 0) - h(0, 0, 0)) * sx, x10 = h(0, 1, 0) + (h(1, 1, 0) - h(0, 1, 0)) * sx;
    const f32 x01 = h(0, 0, 1) + (h(1, 0, 1) - h(0, 0, 1)) * sx, x11 = h(0, 1, 1) + (h(1, 1, 1) - h(0, 1, 1)) * sx;
    const f32 y0 = x00 + (x10 - x00) * sy, y1 = x01 + (x11 - x01) * sy;
    return y0 + (y1 - y0) * sz;
}
inline f64 Fbm(f64 x, f64 y, f64 z, u32 seed, int octaves)
{
    f32 a = 0.5f, f = 1.0f, sum = 0.0f;
    for (int o = 0; o < octaves; ++o)
    {
        sum += a * (ValueNoise(static_cast<f32>(x) * f + 11.0f, static_cast<f32>(y) * f + 23.0f, static_cast<f32>(z) * f + 37.0f, seed + static_cast<u32>(o)) - 0.5f);
        a *= 0.5f; f *= 2.0f;
    }
    return sum;
}
inline f64 Ridged(f64 x, f64 y, f64 z, u32 seed, int octaves)
{
    f32 a = 0.5f, f = 1.0f, sum = 0.0f;
    for (int o = 0; o < octaves; ++o)
    {
        const f32 n = ValueNoise(static_cast<f32>(x) * f + 5.0f, static_cast<f32>(y) * f + 19.0f, static_cast<f32>(z) * f + 41.0f, seed + 100u + static_cast<u32>(o));
        const f32 r = 1.0f - std::fabs(2.0f * n - 1.0f);
        sum += a * r * r;
        a *= 0.5f; f *= 2.0f;
    }
    return sum;
}

// 行 i（0..rows）・列 j（0..cols）の格子点の (u, v)。継ぎ目は整数の剰余で同じ点にする。
struct Surface
{
    u32 rows = 0, cols = 0;
    bool wrapRows = false, wrapCols = false;
    bool poleTop = false, poleBottom = false;
    f32 uTiles = 1.0f, vTiles = 1.0f;
    // p = 表面の点、ref = 内側の基準点（法線を外向きに揃える）
    std::function<void(u32 i, u32 j, f64 p[3], f64 ref[3])> eval;
};

inline void EvalAt(const Surface& s, u32 i, u32 j, f64 p[3], f64 ref[3])
{
    s.eval(s.wrapRows ? i % s.rows : i, s.wrapCols ? j % s.cols : j, p, ref);
}

inline VgeoError BuildFromSurface(const Surface& s, u32 threads, bool lowPriority, u32 materials, VgsrcData& out)
{
    using detail::MakeErr;
    const u64 nv = static_cast<u64>(s.rows + 1) * (s.cols + 1);
    if (nv > 0xFFFFFFF0ull) return MakeErr(Errc::LimitExceeded, "bench: too many vertices for u32 indices");
    out = VgsrcData{};
    out.flags = kVgsrcHasNormals | kVgsrcHasUV0;
    out.positions.resize(nv * 3);
    out.normals.resize(nv * 3);
    out.uv0.resize(nv * 2);
    const u32 stride = s.cols + 1;
    ParallelFor(s.rows + 1, threads, 8, lowPriority, [&](size_t rb, size_t re, u32)
    {
        for (size_t i = rb; i < re; ++i)
            for (u32 j = 0; j <= s.cols; ++j)
            {
                const size_t v = i * stride + j;
                f64 p[3], ref[3];
                EvalAt(s, static_cast<u32>(i), j, p, ref);
                f64 n[3] = {0, 0, 0};
                // 数値微分: 行方向と列方向の隣（境界では片側）から接線を作る
                const u32 i0 = i > 0 ? static_cast<u32>(i) - 1 : static_cast<u32>(i), i1 = i < s.rows ? static_cast<u32>(i) + 1 : static_cast<u32>(i);
                const u32 j0 = j > 0 ? j - 1 : j, j1 = j < s.cols ? j + 1 : j;
                f64 a[3], b[3], c[3], d[3], tmp[3];
                EvalAt(s, i1, j, a, tmp); EvalAt(s, i0, j, b, tmp); EvalAt(s, static_cast<u32>(i), j1, c, tmp); EvalAt(s, static_cast<u32>(i), j0, d, tmp);
                // 極 / 縮退の近くは隣が同一点になる: 半セル内側へずらした点で補う
                f64 du[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]}, dv[3] = {c[0] - d[0], c[1] - d[1], c[2] - d[2]};
                auto len2 = [](const f64* x) { return x[0] * x[0] + x[1] * x[1] + x[2] * x[2]; };
                if (len2(du) < 1e-24 || len2(dv) < 1e-24)
                {
                    const u32 ii = std::min(std::max(static_cast<u32>(i), 1u), s.rows - 1);
                    const u32 ia = ii + 1 > s.rows ? s.rows : ii + 1, ib = ii > 0 ? ii - 1 : 0;
                    EvalAt(s, ia, j, a, tmp); EvalAt(s, ib, j, b, tmp);
                    EvalAt(s, ii, j1, c, tmp); EvalAt(s, ii, j0, d, tmp);
                    for (int k = 0; k < 3; ++k) { du[k] = a[k] - b[k]; dv[k] = c[k] - d[k]; }
                }
                n[0] = du[1] * dv[2] - du[2] * dv[1]; n[1] = du[2] * dv[0] - du[0] * dv[2]; n[2] = du[0] * dv[1] - du[1] * dv[0];
                f64 l = std::sqrt(len2(n));
                const f64 out3[3] = {p[0] - ref[0], p[1] - ref[1], p[2] - ref[2]};
                if (!(l > 0)) { n[0] = out3[0]; n[1] = out3[1]; n[2] = out3[2]; l = std::sqrt(len2(n)); if (!(l > 0)) { n[0] = 0; n[1] = 1; n[2] = 0; l = 1; } }
                if (n[0] * out3[0] + n[1] * out3[1] + n[2] * out3[2] < 0) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
                for (int k = 0; k < 3; ++k) { out.positions[v * 3 + k] = static_cast<f32>(p[k]); out.normals[v * 3 + k] = static_cast<f32>(n[k] / l); }
                out.uv0[v * 2] = static_cast<f32>(j) / static_cast<f32>(s.cols) * s.uTiles;
                out.uv0[v * 2 + 1] = static_cast<f32>(i) / static_cast<f32>(s.rows) * s.vTiles;
            }
    });
    // 巻き順: 標本セルで (a,c,b) が外向きかを見て決める（形状全体で外向きに統一）
    f64 votes = 0;
    {
        const u32 step = std::max(1u, s.rows / 16);
        for (u32 i = step / 2; i < s.rows; i += step)
            for (u32 j = 0; j < s.cols; j += std::max(1u, s.cols / 16))
            {
                const size_t a = static_cast<size_t>(i) * stride + j, b = static_cast<size_t>(i + 1) * stride + j, c = b + 1;
                const f32* pa = &out.positions[a * 3];
                const f32* pb = &out.positions[b * 3];
                const f32* pc = &out.positions[c * 3];
                const f64 e1[3] = {static_cast<f64>(pc[0]) - pa[0], static_cast<f64>(pc[1]) - pa[1], static_cast<f64>(pc[2]) - pa[2]};
                const f64 e2[3] = {static_cast<f64>(pb[0]) - pa[0], static_cast<f64>(pb[1]) - pa[1], static_cast<f64>(pb[2]) - pa[2]};
                const f64 gn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                const f32* nn = &out.normals[a * 3];
                votes += gn[0] * nn[0] + gn[1] * nn[1] + gn[2] * nn[2];
            }
    }
    const bool flip = votes < 0;
    // インデックス
    std::vector<u64> rowOff(s.rows + 1, 0);
    for (u32 i = 0; i < s.rows; ++i)
    {
        u64 n = static_cast<u64>(s.cols) * 2;
        if (s.poleTop && i == 0) n -= s.cols;
        if (s.poleBottom && i == s.rows - 1) n -= s.cols;
        rowOff[i + 1] = rowOff[i] + n;
    }
    out.indices.resize(rowOff[s.rows] * 3);
    ParallelFor(s.rows, threads, 8, lowPriority, [&](size_t b, size_t e, u32)
    {
        for (size_t i = b; i < e; ++i)
        {
            u64 w = rowOff[i] * 3;
            for (u32 j = 0; j < s.cols; ++j)
            {
                const u32 a = static_cast<u32>(i) * stride + j, bb = (static_cast<u32>(i) + 1) * stride + j, c = bb + 1, d = a + 1;
                if (!(s.poleBottom && i == s.rows - 1))
                {
                    out.indices[w++] = a; out.indices[w++] = flip ? bb : c; out.indices[w++] = flip ? c : bb;
                }
                if (!(s.poleTop && i == 0))
                {
                    out.indices[w++] = a; out.indices[w++] = flip ? c : d; out.indices[w++] = flip ? d : c;
                }
            }
        }
    });
    // 材質セクション（行方向に等分）
    materials = std::max(1u, std::min(materials, s.rows));
    StringPool pool;
    for (u32 m = 0; m < materials; ++m)
    {
        MaterialRecord mr = MakeDefaultMaterial();
        char nm[32];
        std::snprintf(nm, sizeof nm, "bench_mat%u", m);
        mr.nameOff = pool.Add(nm);
        mr.roughness = 0.3f + 0.6f * static_cast<f32>(m) / static_cast<f32>(std::max(1u, materials - 1));
        out.materials.push_back(mr);
        const u32 r0 = static_cast<u32>(static_cast<u64>(m) * s.rows / materials), r1 = static_cast<u32>(static_cast<u64>(m + 1) * s.rows / materials);
        const u64 first = rowOff[r0] * 3, last = rowOff[r1] * 3;
        if (last > first) out.sections.push_back(VgsrcSection{m, static_cast<u32>(first), static_cast<u32>(last - first), 0});
    }
    out.strings = pool.Data();
    return VgeoError{};
}

} // namespace bench_detail

// 種類名 → 生成。tris は目標（格子に丸まる）。
inline VgeoError GenerateBench(const BenchOptions& bo, VgsrcData& out)
{
    using namespace bench_detail;
    using detail::MakeErr;
    const f64 kPi = 3.14159265358979323846;
    const u32 threads = bo.threads ? bo.threads : DefaultThreadCount();
    const u32 seed = bo.seed;
    Surface s;
    const std::string& kind = bo.kind;
    auto sphereRows = [&]() { return std::max(3u, static_cast<u32>(std::llround((1.0 + std::sqrt(1.0 + static_cast<f64>(bo.tris))) * 0.5))); };

    if (kind == "blob" || kind == "rock" || kind == "sphere")
    {
        const u32 r = sphereRows();
        s.rows = r; s.cols = 2 * r; s.wrapCols = true; s.poleTop = s.poleBottom = true; s.uTiles = 2.0f;
        const bool isRock = kind == "rock", isSphere = kind == "sphere";
        s.eval = [=](u32 i, u32 j, f64 p[3], f64 ref[3])
        {
            const f64 th = kPi * i / r, ph = 2.0 * kPi * j / (2.0 * r);
            const f64 st = (i == 0 || i == r) ? 0.0 : std::sin(th);
            const f64 dx = st * std::cos(ph), dy = std::cos(th), dz = st * std::sin(ph);
            ref[0] = ref[1] = ref[2] = 0;
            if (isSphere) { p[0] = dx; p[1] = dy; p[2] = dz; return; }
            if (!isRock)
            {
                f32 a = 0.5f, f = 2.0f, sum = 0.0f;
                for (int o = 0; o < 5; ++o) { sum += a * (ValueNoise(static_cast<f32>(dx) * f + 11.0f, static_cast<f32>(dy) * f + 23.0f, static_cast<f32>(dz) * f + 37.0f, seed + static_cast<u32>(o)) - 0.5f); a *= 0.5f; f *= 2.0f; }
                const f64 rho = 1.0 + 0.35 * sum;
                p[0] = rho * dx; p[1] = rho * dy; p[2] = rho * dz;
                return;
            }
            // 岩: 大きな塊 + リッジ（稜線）+ 細かい凹凸、異方スケール、平らな底
            const f64 lump = Fbm(dx * 1.5, dy * 1.5, dz * 1.5, seed, 3);
            const f64 ridge = Ridged(dx * 2.0, dy * 2.0, dz * 2.0, seed, 5);
            const f64 fine = Fbm(dx * 24.0, dy * 24.0, dz * 24.0, seed + 7u, 3);
            const f64 rho = 0.85 + 0.9 * lump + 0.55 * ridge + 0.03 * fine;
            p[0] = 1.0 * rho * dx; p[1] = std::max(0.72 * rho * dy, -0.5); p[2] = 0.9 * rho * dz;
        };
    }
    else if (kind == "torus")
    {
        const u32 V = std::max(3u, static_cast<u32>(std::llround(std::sqrt(static_cast<f64>(bo.tris) / 4.0))));
        s.rows = 2 * V; s.cols = V; s.wrapRows = s.wrapCols = true; s.uTiles = 1.0f; s.vTiles = 2.0f;
        s.eval = [=](u32 i, u32 j, f64 p[3], f64 ref[3])
        {
            const f64 u = 2 * kPi * i / (2.0 * V), v = 2 * kPi * j / V;
            const f64 R = 1.0, rr = 0.35;
            p[0] = (R + rr * std::cos(v)) * std::cos(u); p[1] = rr * std::sin(v); p[2] = (R + rr * std::cos(v)) * std::sin(u);
            ref[0] = R * std::cos(u); ref[1] = 0; ref[2] = R * std::sin(u);
        };
    }
    else if (kind == "knot")
    {
        const u32 V = std::max(3u, static_cast<u32>(std::llround(std::sqrt(static_cast<f64>(bo.tris) / 8.0))));
        const u32 U = 4 * V;
        s.rows = U; s.cols = V; s.wrapRows = s.wrapCols = true; s.uTiles = 1.0f; s.vTiles = 8.0f;
        s.eval = [=](u32 i, u32 j, f64 p[3], f64 ref[3])
        {
            auto C = [&](f64 t, f64 c[3])
            {
                const f64 k = 2.0 + std::cos(3.0 * t);
                c[0] = 0.5 * k * std::cos(2.0 * t); c[1] = 0.5 * std::sin(3.0 * t) * 1.2; c[2] = 0.5 * k * std::sin(2.0 * t);
            };
            const f64 t = 2 * kPi * i / static_cast<f64>(U), th = 2 * kPi * j / static_cast<f64>(V);
            const f64 h = 1e-4;
            f64 c0[3], c1[3], c2[3];
            C(t - h, c0); C(t, c1); C(t + h, c2);
            f64 T[3] = {c2[0] - c0[0], c2[1] - c0[1], c2[2] - c0[2]};
            const f64 tl = std::sqrt(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
            for (int k = 0; k < 3; ++k) T[k] /= tl;
            f64 acc[3] = {(c2[0] - 2 * c1[0] + c0[0]), (c2[1] - 2 * c1[1] + c0[1]), (c2[2] - 2 * c1[2] + c0[2])};
            const f64 dT = acc[0] * T[0] + acc[1] * T[1] + acc[2] * T[2];
            f64 N[3] = {acc[0] - dT * T[0], acc[1] - dT * T[1], acc[2] - dT * T[2]};
            const f64 nl = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
            for (int k = 0; k < 3; ++k) N[k] /= nl;
            const f64 B[3] = {T[1] * N[2] - T[2] * N[1], T[2] * N[0] - T[0] * N[2], T[0] * N[1] - T[1] * N[0]};
            const f64 tw = 3.0 * t + th;
            const f64 rad = 0.16 * (1.0 + 0.25 * std::sin(5.0 * th + 7.0 * t));
            const f64 ca = std::cos(tw), sa = std::sin(tw);
            f64 d3[3] = {ca * N[0] + sa * B[0], ca * N[1] + sa * B[1], ca * N[2] + sa * B[2]};
            const f64 disp = 0.012 * Fbm(c1[0] * 6 + d3[0], c1[1] * 6 + d3[1], c1[2] * 6 + d3[2], seed, 4);
            const f64 rr = rad + disp;
            for (int k = 0; k < 3; ++k) { p[k] = c1[k] + rr * d3[k]; ref[k] = c1[k]; }
        };
    }
    else if (kind == "grid")
    {
        const u32 n = std::max(1u, static_cast<u32>(std::llround(std::sqrt(static_cast<f64>(bo.tris) / 2.0))));
        s.rows = n; s.cols = n;
        s.eval = [=](u32 i, u32 j, f64 p[3], f64 ref[3])
        {
            const f64 x = -1.0 + 2.0 * i / n, z = -1.0 + 2.0 * j / n;
            p[0] = x; p[1] = 0.08 * std::sin(3.0 * x) * std::cos(3.0 * z); p[2] = z;
            ref[0] = x; ref[1] = -1.0; ref[2] = z;
        };
    }
    else return MakeErr(Errc::UnsupportedFeature, "bench: unknown kind (blob | knot | rock | torus | sphere | grid)");
    if (bo.tris == 0) return MakeErr(Errc::CountMismatch, "bench: tris must be > 0");
    return BuildFromSurface(s, threads, bo.lowPriority, bo.materials, out);
}

} // namespace dx12e::vg::cook
