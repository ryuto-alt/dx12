#pragma once
//
// VgeoStubGen.h ― 合成メッシュ → LOD0 のみの `.vgeo`（P0 スタブ生成器の本体。CLI: tools/vgeo_stub/main.cpp）。
//
//   目的: P1（本物の cooker）を待たずに、P2（ランタイム読み込み + GPU カリング）が「形式どおりの .vgeo」を
//         読めるようにする。DAG / LOD は作らない（全クラスタが LOD0 かつ root: parentLodError = +INF）。
//         ただし BVH（4 分木）・グループ・ページ・プロキシ・材質は仕様どおり正しく埋める。
//   依存: 標準ライブラリ + meshoptimizer（クラスタ化と、プロキシ簡略化のみ）。GPU / D3D は使わない。
//   ★合成データだけを作る。実アセットや UE 由来の資産は一切扱わない（リポジトリは公開）。
//
#include "renderer/vg/VgeoBvh.h"
#include "renderer/vg/VgeoFormat.h"
#include "renderer/vg/VgsrcFormat.h"

#include <meshoptimizer.h>

#include <chrono>
#include <cmath>
#include <string>

namespace dx12e::vg::stub
{

// ── 合成メッシュ ───────────────────────────────────────────────────────────────
struct MeshData
{
    std::vector<f32> pos;   // 3 * vertexCount（エンジン空間: Y up・m）
    std::vector<f32> nrm;   // 3 * vertexCount
    std::vector<f32> uv;    // 2 * vertexCount
    std::vector<u32> idx;   // 三角形リスト。cross(p1 - p0, p2 - p0) が外向き（仕様 §8.3 の巻き順規約）
    u32 vertexCount() const { return static_cast<u32>(pos.size() / 3); }
    u64 triangleCount() const { return idx.size() / 3; }
};

inline f32 Hash01(u32 x, u32 y, u32 z, u32 seed)
{
    u32 h = x * 0x9E3779B1u ^ (y + 0x7F4A7C15u) * 0x85EBCA77u ^ (z + 0x165667B1u) * 0xC2B2AE3Du ^ seed * 0x27D4EB2Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return static_cast<f32>(h >> 8) / 16777216.0f;
}
// 3D バリューノイズ（[0,1)）
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

// (rows x cols) の格子頂点から「(a,c,b),(a,d,c)」の 2 三角形/セルで三角形リストを作る（外向き規約）。
// skipDegenerate: 球のように上下の行が 1 点に潰れる場合、潰れた三角形を除く。
inline void GridIndices(std::vector<u32>& idx, u32 rows, u32 cols, bool poleTop, bool poleBottom)
{
    idx.reserve(static_cast<size_t>(rows) * cols * 6);
    for (u32 i = 0; i < rows; ++i)
        for (u32 j = 0; j < cols; ++j)
        {
            const u32 a = i * (cols + 1) + j, b = (i + 1) * (cols + 1) + j, c = (i + 1) * (cols + 1) + j + 1, d = i * (cols + 1) + j + 1;
            if (!(poleBottom && i == rows - 1)) { idx.push_back(a); idx.push_back(c); idx.push_back(b); }
            if (!(poleTop && i == 0))           { idx.push_back(a); idx.push_back(d); idx.push_back(c); }
        }
}

// 半径 1 の UV 球。三角形数はおおむね targetTris（4r(r-1) に丸める）。
inline MeshData MakeSphere(u64 targetTris)
{
    MeshData m;
    u32 r = static_cast<u32>(std::llround((1.0 + std::sqrt(1.0 + static_cast<f64>(targetTris))) * 0.5));
    r = std::max(3u, r);
    const u32 s = 2 * r;
    m.pos.reserve(static_cast<size_t>(r + 1) * (s + 1) * 3);
    m.nrm.reserve(m.pos.capacity()); m.uv.reserve(static_cast<size_t>(r + 1) * (s + 1) * 2);
    for (u32 i = 0; i <= r; ++i)
    {
        const f64 th = 3.14159265358979323846 * i / r;
        for (u32 j = 0; j <= s; ++j)
        {
            const f64 ph = 2.0 * 3.14159265358979323846 * j / s;
            const f32 x = static_cast<f32>(std::sin(th) * std::cos(ph)), y = static_cast<f32>(std::cos(th)), z = static_cast<f32>(std::sin(th) * std::sin(ph));
            m.pos.insert(m.pos.end(), {x, y, z});
            m.nrm.insert(m.nrm.end(), {x, y, z});
            m.uv.insert(m.uv.end(), {static_cast<f32>(j) / s, static_cast<f32>(i) / r});
        }
    }
    GridIndices(m.idx, r, s, true, true);
    return m;
}

// 大半径 1、小半径 0.35 のトーラス（4V² 三角形）。
inline MeshData MakeTorus(u64 targetTris)
{
    MeshData m;
    const u32 V = std::max(3u, static_cast<u32>(std::llround(std::sqrt(static_cast<f64>(targetTris) / 4.0))));
    const u32 U = 2 * V;
    const f64 R = 1.0, rr = 0.35, kPi = 3.14159265358979323846;
    for (u32 i = 0; i <= U; ++i)
    {
        const f64 u = 2 * kPi * i / U;
        for (u32 j = 0; j <= V; ++j)
        {
            const f64 v = 2 * kPi * j / V;
            const f64 cx = std::cos(u), sz = std::sin(u), cv = std::cos(v), sv = std::sin(v);
            m.pos.insert(m.pos.end(), {static_cast<f32>((R + rr * cv) * cx), static_cast<f32>(rr * sv), static_cast<f32>((R + rr * cv) * sz)});
            m.nrm.insert(m.nrm.end(), {static_cast<f32>(cv * cx), static_cast<f32>(sv), static_cast<f32>(cv * sz)});
            m.uv.insert(m.uv.end(), {static_cast<f32>(i) / U * 2.0f, static_cast<f32>(j) / V});
        }
    }
    GridIndices(m.idx, U, V, false, false);
    return m;
}

// [-1,1]² の XZ 平面に緩い起伏をつけた格子（2n² 三角形）。法線は解析微分。
inline MeshData MakeGrid(u64 targetTris)
{
    MeshData m;
    const u32 n = std::max(1u, static_cast<u32>(std::llround(std::sqrt(static_cast<f64>(targetTris) / 2.0))));
    for (u32 i = 0; i <= n; ++i)
    {
        const f64 x = -1.0 + 2.0 * i / n;
        for (u32 j = 0; j <= n; ++j)
        {
            const f64 z = -1.0 + 2.0 * j / n;
            const f64 y = 0.08 * std::sin(3.0 * x) * std::cos(3.0 * z);
            const f64 dydx = 0.24 * std::cos(3.0 * x) * std::cos(3.0 * z), dydz = -0.24 * std::sin(3.0 * x) * std::sin(3.0 * z);
            const f64 l = std::sqrt(dydx * dydx + 1.0 + dydz * dydz);
            m.pos.insert(m.pos.end(), {static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)});
            m.nrm.insert(m.nrm.end(), {static_cast<f32>(-dydx / l), static_cast<f32>(1.0 / l), static_cast<f32>(-dydz / l)});
            m.uv.insert(m.uv.end(), {static_cast<f32>(i) / n, static_cast<f32>(j) / n});
        }
    }
    GridIndices(m.idx, n, n, false, false);
    return m;
}

// fBm で変位した球（岩のような凸凹。巨大ベンチ向け）。同じ (targetTris, seed) なら同じ形。
inline MeshData MakeBlob(u64 targetTris, u32 seed)
{
    MeshData m = MakeSphere(targetTris);
    const f64 kPi = 3.14159265358979323846;
    // 頂点は (r+1)*(s+1) 個。行・列は MakeSphere と同じ式で復元する。
    u32 r = static_cast<u32>(std::llround((1.0 + std::sqrt(1.0 + static_cast<f64>(targetTris))) * 0.5));
    r = std::max(3u, r);
    const u32 s = 2 * r;
    auto rho = [&](f64 th, f64 ph) -> f64
    {
        const f32 dx = static_cast<f32>(std::sin(th) * std::cos(ph)), dy = static_cast<f32>(std::cos(th)), dz = static_cast<f32>(std::sin(th) * std::sin(ph));
        f32 a = 0.5f, f = 2.0f, sum = 0.0f;
        for (int o = 0; o < 5; ++o) { sum += a * (ValueNoise(dx * f + 11.0f, dy * f + 23.0f, dz * f + 37.0f, seed + o) - 0.5f); a *= 0.5f; f *= 2.0f; }
        return 1.0 + 0.35 * sum;
    };
    for (u32 i = 0; i <= r; ++i)
    {
        const f64 th = kPi * i / r;
        for (u32 j = 0; j <= s; ++j)
        {
            const f64 ph = 2.0 * kPi * (j % s) / s;    // シームの列は同じ形になるよう j % s
            const size_t v = static_cast<size_t>(i) * (s + 1) + j;
            auto P = [&](f64 t, f64 p, f64 out[3]) { const f64 rh = rho(t, p); out[0] = rh * std::sin(t) * std::cos(p); out[1] = rh * std::cos(t); out[2] = rh * std::sin(t) * std::sin(p); };
            f64 p0[3]; P(th, ph, p0);
            m.pos[v * 3] = static_cast<f32>(p0[0]); m.pos[v * 3 + 1] = static_cast<f32>(p0[1]); m.pos[v * 3 + 2] = static_cast<f32>(p0[2]);
            // 法線 = 数値微分の外積（極では th をずらして安定化）
            const f64 e = 1e-3;
            const f64 thc = std::min(std::max(th, e), kPi - e);
            f64 a[3], b[3], c[3], d[3];
            P(thc + e, ph, a); P(thc - e, ph, b); P(thc, ph + e, c); P(thc, ph - e, d);
            const f64 dt[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]}, dp[3] = {c[0] - d[0], c[1] - d[1], c[2] - d[2]};
            f64 n[3] = {dp[1] * dt[2] - dp[2] * dt[1], dp[2] * dt[0] - dp[0] * dt[2], dp[0] * dt[1] - dp[1] * dt[0]};
            f64 l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (!(l > 0)) { n[0] = p0[0]; n[1] = p0[1]; n[2] = p0[2]; l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); }
            // 外向きに揃える
            if (n[0] * p0[0] + n[1] * p0[1] + n[2] * p0[2] < 0) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
            m.nrm[v * 3] = static_cast<f32>(n[0] / l); m.nrm[v * 3 + 1] = static_cast<f32>(n[1] / l); m.nrm[v * 3 + 2] = static_cast<f32>(n[2] / l);
        }
    }
    return m;
}

inline bool MakeMeshByName(const std::string& kind, u64 tris, u32 seed, MeshData& out)
{
    if (kind == "sphere") out = MakeSphere(tris);
    else if (kind == "torus") out = MakeTorus(tris);
    else if (kind == "grid") out = MakeGrid(tris);
    else if (kind == "blob") out = MakeBlob(tris, seed);
    else return false;
    return true;
}

// ── VGSRC の見本（P6 の C# 実装の突き合わせ用）─────────────────────────────────
// 合成メッシュを VGSRC にする。ueCoords = true なら UE 座標（Z up・cm。unitScaleToMeters = 0.01、coordSystem = 1）へ逆変換して書く
// （エンジン (x,y,z) = UE (y,z,x) × 0.01 の逆: UE = (z, x, y) × 100）。読み戻して ConvertPositionToEngine すると元の位置に戻る。
inline VgsrcData MeshToVgsrc(const MeshData& m, bool ueCoords, const std::string& label = "stub")
{
    VgsrcData d;
    d.flags = kVgsrcHasNormals | kVgsrcHasUV0;
    d.uv0 = m.uv;
    d.indices = m.idx;
    d.positions.resize(m.pos.size());
    d.normals.resize(m.nrm.size());
    for (size_t v = 0; v < m.pos.size() / 3; ++v)
    {
        const f32* p = &m.pos[v * 3];
        const f32* n = &m.nrm[v * 3];
        if (ueCoords)
        {
            d.positions[v * 3] = p[2] * 100.0f; d.positions[v * 3 + 1] = p[0] * 100.0f; d.positions[v * 3 + 2] = p[1] * 100.0f;
            d.normals[v * 3] = n[2]; d.normals[v * 3 + 1] = n[0]; d.normals[v * 3 + 2] = n[1];
        }
        else
        {
            std::memcpy(&d.positions[v * 3], p, 12); std::memcpy(&d.normals[v * 3], n, 12);
        }
    }
    if (ueCoords) { d.coordSystem = kCoordUE; d.unitScaleToMeters = 0.01f; }
    d.sections.push_back(VgsrcSection{0, 0, static_cast<u32>(m.idx.size()), 0});
    StringPool pool;
    MaterialRecord mat = MakeDefaultMaterial();
    mat.nameOff = pool.Add(label);
    d.materials.push_back(mat);
    d.strings = pool.Data();
    return d;
}

// ── スタブの構築 ─────────────────────────────────────────────────────────────
struct StubOptions
{
    u32  maxVerts   = 128;        // 1 クラスタの最大頂点数（形式の上限 128）
    u32  maxTris    = 128;        // 同 三角形数（meshopt の都合で 4 の倍数）
    f32  coneWeight = 0.25f;
    u32  groupSize  = 4;          // 1 グループのクラスタ数（1..15）。実 DAG の 2〜5 に合わせて 4
    bool proxy      = true;       // PROXY セクションを作る（P2 のローダが必要とする）
    u32  proxyMaxTris = 250000;   // プロキシの三角形数の上限
    std::string paramString;      // cookParamsHash の元（空なら自動）
    std::string label = "stub";   // 材質名 / debugJson の識別子
};

struct StubStats
{
    u64 sourceTris = 0, sourceVerts = 0;
    u32 clusters = 0, groups = 0, pages = 0, nodes = 0;
    f64 avgTrisPerCluster = 0.0, avgVertsPerCluster = 0.0;
    u64 fileBytesEstimate = 0;
    f64 msQuantize = 0, msMeshlets = 0, msPack = 0, msProxy = 0, msTotal = 0;
};

namespace detail
{
inline u64 Spread21(u64 x)
{
    x &= 0x1FFFFFull;
    x = (x | x << 32) & 0x1F00000000FFFFull;
    x = (x | x << 16) & 0x1F0000FF0000FFull;
    x = (x | x << 8)  & 0x100F00F00F00F00Full;
    x = (x | x << 4)  & 0x10C30C30C30C30C3ull;
    x = (x | x << 2)  & 0x1249249249249249ull;
    return x;
}
inline double MsSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
} // namespace detail

inline VgeoError BuildStubContent(const MeshData& mesh, const StubOptions& opt, VgeoContent& out, StubStats* stats = nullptr)
{
    using dx12e::vg::detail::MakeErr;
    using clk = std::chrono::steady_clock;
    const auto tStart = clk::now();
    if (mesh.idx.empty() || mesh.idx.size() % 3 != 0 || mesh.pos.size() < 9) return MakeErr(Errc::CountMismatch, "empty mesh / index count is not a multiple of 3");
    const size_t vcount = mesh.pos.size() / 3;
    if (mesh.nrm.size() != mesh.pos.size() || mesh.uv.size() != vcount * 2) return MakeErr(Errc::CountMismatch, "pos/nrm/uv sizes disagree");
    if (vcount > 0xFFFFFFFFull) return MakeErr(Errc::LimitExceeded, "too many vertices");
    if (opt.maxVerts < 3 || opt.maxVerts > kMaxClusterVerts || opt.maxTris < 4 || opt.maxTris > kMaxClusterTris || (opt.maxTris % 4) != 0)
        return MakeErr(Errc::LimitExceeded, "maxVerts must be in [3,128], maxTris a multiple of 4 in [4,128]");
    if (opt.groupSize < 1 || opt.groupSize > kMaxGroupClusters) return MakeErr(Errc::LimitExceeded, "groupSize must be in [1,15]");
    for (u32 i : mesh.idx) if (i >= vcount) return MakeErr(Errc::ClusterInvalid, "mesh index out of range");

    // 1) 格子への量子化。meshopt には「量子化後の位置」を渡す（境界球が実際に描かれる点を確実に包むように）。
    auto t0 = clk::now();
    f64 lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (size_t v = 0; v < vcount; ++v)
        for (int a = 0; a < 3; ++a)
        {
            const f32 p = mesh.pos[v * 3 + a];
            if (!Finite(p)) return MakeErr(Errc::BadFloat, "non-finite position in the input mesh");
            lo[a] = std::min(lo[a], static_cast<f64>(p)); hi[a] = std::max(hi[a], static_cast<f64>(p));
        }
    f32 origin[3];
    f64 maxExtent = 0;
    for (int a = 0; a < 3; ++a) { origin[a] = static_cast<f32>(lo[a]); maxExtent = std::max(maxExtent, static_cast<f64>(static_cast<f32>(hi[a])) - static_cast<f64>(origin[a])); }
    const f32 step = ComputePosStep(maxExtent);
    std::vector<u32> q(vcount * 3);
    std::vector<f32> dpos(vcount * 3);
    for (size_t v = 0; v < vcount; ++v)
        for (int a = 0; a < 3; ++a)
        {
            q[v * 3 + a] = QuantizeCoord(mesh.pos[v * 3 + a], origin[a], step);
            dpos[v * 3 + a] = DequantizeCoord(q[v * 3 + a], origin[a], step);
        }
    const f64 msQuant = dx12e::vg::stub::detail::MsSince(t0);

    // 2) クラスタ化
    t0 = clk::now();
    const size_t maxMeshlets = meshopt_buildMeshletsBound(mesh.idx.size(), opt.maxVerts, opt.maxTris);
    std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
    std::vector<u32> mv(mesh.idx.size());
    std::vector<u8> mt(mesh.idx.size());
    const size_t nm = meshopt_buildMeshlets(meshlets.data(), mv.data(), mt.data(), mesh.idx.data(), mesh.idx.size(), dpos.data(), vcount, 12, opt.maxVerts, opt.maxTris, opt.coneWeight);
    meshlets.resize(nm);
    struct Info { f32 sphere[4]; u32 cone; f32 maxEdge; u64 morton; };
    std::vector<Info> info(nm);
    f64 clo[3] = {1e300, 1e300, 1e300}, chi[3] = {-1e300, -1e300, -1e300};
    for (size_t i = 0; i < nm; ++i)
    {
        const meshopt_Meshlet& ml = meshlets[i];
        u32* mvp = mv.data() + ml.vertex_offset;
        u8* mtp = mt.data() + ml.triangle_offset;
        meshopt_optimizeMeshlet(mvp, mtp, ml.triangle_count, ml.vertex_count);
        const meshopt_Bounds b = meshopt_computeMeshletBounds(mvp, mtp, ml.triangle_count, dpos.data(), vcount, 12);
        Info& in = info[i];
        in.sphere[0] = b.center[0]; in.sphere[1] = b.center[1]; in.sphere[2] = b.center[2];
        // meshopt の球が（量子化後の）全頂点を包むことを保証する（退化メッシュでは中心が外れることがあるため、半径を実測で広げる）
        f64 rmax = b.radius;
        for (u32 v = 0; v < ml.vertex_count; ++v)
        {
            const f32* pv = dpos.data() + static_cast<size_t>(mvp[v]) * 3;
            const f64 dx = static_cast<f64>(pv[0]) - in.sphere[0], dy = static_cast<f64>(pv[1]) - in.sphere[1], dz = static_cast<f64>(pv[2]) - in.sphere[2];
            rmax = std::max(rmax, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        in.sphere[3] = std::nextafter(static_cast<f32>(rmax), std::numeric_limits<f32>::infinity());
        auto cl127 = [](signed char v) -> i8 { return static_cast<i8>(v < -127 ? -127 : v); };
        in.cone = PackConeS8(cl127(b.cone_axis_s8[0]), cl127(b.cone_axis_s8[1]), cl127(b.cone_axis_s8[2]), cl127(b.cone_cutoff_s8));
        f64 me2 = 0.0;
        for (u32 t = 0; t < ml.triangle_count; ++t)
            for (u32 e = 0; e < 3; ++e)
            {
                const f32* pa = dpos.data() + static_cast<size_t>(mvp[mtp[t * 3 + e]]) * 3;
                const f32* pb = dpos.data() + static_cast<size_t>(mvp[mtp[t * 3 + (e + 1) % 3]]) * 3;
                const f64 dx = static_cast<f64>(pa[0]) - pb[0], dy = static_cast<f64>(pa[1]) - pb[1], dz = static_cast<f64>(pa[2]) - pb[2];
                me2 = std::max(me2, dx * dx + dy * dy + dz * dz);
            }
        in.maxEdge = static_cast<f32>(std::sqrt(me2));
        for (int a = 0; a < 3; ++a) { clo[a] = std::min(clo[a], static_cast<f64>(in.sphere[a])); chi[a] = std::max(chi[a], static_cast<f64>(in.sphere[a])); }
    }
    for (size_t i = 0; i < nm; ++i)
    {
        u64 code = 0;
        for (int a = 0; a < 3; ++a)
        {
            const f64 ext = chi[a] - clo[a];
            const f64 t = ext > 0 ? (static_cast<f64>(info[i].sphere[a]) - clo[a]) / ext : 0.0;
            code |= dx12e::vg::stub::detail::Spread21(static_cast<u64>(t * 2097151.0)) << a;
        }
        info[i].morton = code;
    }
    std::vector<u32> order(nm);
    for (size_t i = 0; i < nm; ++i) order[i] = static_cast<u32>(i);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return info[a].morton != info[b].morton ? info[a].morton < info[b].morton : a < b; });
    const f64 msMeshlets = dx12e::vg::stub::detail::MsSince(t0);

    // 3) グループ化（Morton 順に groupSize 個ずつ）→ ページ詰め → BVH の葉
    t0 = clk::now();
    out = VgeoContent{};
    StringPool pool;
    MaterialRecord mat = MakeDefaultMaterial();
    mat.nameOff = pool.Add(opt.label);
    out.materials.push_back(mat);
    out.strings = pool.Data();

    PageBuilder pb(0);
    u32 pageIndex = 0;
    std::vector<BvhLeaf> leaves;
    std::vector<ClusterSource> group;
    u64 sumTris = 0, sumVerts = 0;
    auto makeCluster = [&](u32 mi, ClusterSource& cs)
    {
        const meshopt_Meshlet& ml = meshlets[mi];
        const u32* mvp = mv.data() + ml.vertex_offset;
        const u8* mtp = mt.data() + ml.triangle_offset;
        const Info& in = info[mi];
        std::memcpy(cs.cullSphere, in.sphere, 16); std::memcpy(cs.lodSphere, in.sphere, 16); std::memcpy(cs.parentLodSphere, in.sphere, 16);
        cs.lodError = 0.0f; cs.parentLodError = PosInf(); cs.coneS8 = in.cone; cs.maxEdgeLength = in.maxEdge;
        cs.level = 0; cs.root = true; cs.childPage = kNone; cs.childGroup = kNone; cs.materialIndex = 0;
        cs.q.resize(static_cast<size_t>(ml.vertex_count) * 3);
        cs.normalOct.resize(ml.vertex_count);
        cs.uv.resize(static_cast<size_t>(ml.vertex_count) * 2);
        for (u32 v = 0; v < ml.vertex_count; ++v)
        {
            const size_t src = mvp[v];
            cs.q[v * 3] = q[src * 3]; cs.q[v * 3 + 1] = q[src * 3 + 1]; cs.q[v * 3 + 2] = q[src * 3 + 2];
            cs.normalOct[v] = EncodeOct16(mesh.nrm[src * 3], mesh.nrm[src * 3 + 1], mesh.nrm[src * 3 + 2]);
            cs.uv[v * 2] = mesh.uv[src * 2]; cs.uv[v * 2 + 1] = mesh.uv[src * 2 + 1];
        }
        cs.tri.assign(mtp, mtp + static_cast<size_t>(ml.triangle_count) * 3);
        sumTris += ml.triangle_count; sumVerts += ml.vertex_count;
    };
    auto flushGroup = [&]() -> VgeoError
    {
        if (group.empty()) return VgeoError{};
        u32 first = 0;
        PageBuilder::AddResult r = pb.TryAddGroup(group.data(), static_cast<u32>(group.size()), &first);
        if (r == PageBuilder::AddResult::NoFit)
        {
            pb.FinishAppend(out.pages, true);
            pb.Reset(++pageIndex);
            r = pb.TryAddGroup(group.data(), static_cast<u32>(group.size()), &first);
        }
        if (r != PageBuilder::AddResult::Added) return MakeErr(Errc::ClusterInvalid, "a cluster group could not be packed into a page");
        BvhLeaf leaf;
        std::vector<f32> sph;
        for (const auto& c : group) sph.insert(sph.end(), c.cullSphere, c.cullSphere + 4);
        EnclosingSphere(sph.data(), static_cast<u32>(group.size()), leaf.cullSphere);
        std::memcpy(leaf.lodSphere, leaf.cullSphere, 16);           // members' parentLodSphere == their cullSphere（ルート）
        leaf.minOwnError = 0.0f; leaf.maxParentError = PosInf();
        leaf.groupId = static_cast<u32>(leaves.size());
        leaf.groupPacked = MakeGroupPacked(pageIndex, first, static_cast<u32>(group.size()));
        leaves.push_back(leaf);
        group.clear();
        return VgeoError{};
    };
    for (size_t k = 0; k < nm; ++k)
    {
        group.emplace_back();
        makeCluster(order[k], group.back());
        if (group.size() == opt.groupSize) { const VgeoError e = flushGroup(); if (!e.ok()) return e; }
    }
    { const VgeoError e = flushGroup(); if (!e.ok()) return e; }
    pb.FinishAppend(out.pages, true);
    const u32 pageCount = pageIndex + 1;
    if (pageCount > kMaxPages) return MakeErr(Errc::LimitExceeded, "the mesh needs more than 65535 pages (too many triangles for one .vgeo)");
    out.nodes = BuildBvh4(leaves);
    const f64 msPack = dx12e::vg::stub::detail::MsSince(t0);

    // 4) ヘッダのメタ
    VgeoHeader& h = out.header;
    h.maxClusterVerts = opt.maxVerts; h.maxClusterTris = opt.maxTris;
    h.sourceTriangleCount = mesh.triangleCount(); h.sourceVertexCount = vcount;
    for (int a = 0; a < 3; ++a) { h.aabbMin[a] = origin[a]; h.aabbMax[a] = static_cast<f32>(hi[a]); h.posOrigin[a] = origin[a]; }
    h.posStep = step;
    {
        std::vector<f32> sph;
        for (const HierChild& c : out.nodes[0].child) if (c.ref != kNone) sph.insert(sph.end(), c.cullSphere, c.cullSphere + 4);
        EnclosingSphere(sph.data(), static_cast<u32>(sph.size() / 4), h.boundingSphere);
    }
    h.pinnedPageCount = pageCount;    // LOD0 のみ = 全ページが root を持つ = 全部 pinned
    {
        u64 hash = 0xcbf29ce484222325ull;
        hash = Fnv1a64(mesh.pos.data(), mesh.pos.size() * 4, hash);
        hash = Fnv1a64(mesh.nrm.data(), mesh.nrm.size() * 4, hash);
        hash = Fnv1a64(mesh.uv.data(), mesh.uv.size() * 4, hash);
        hash = Fnv1a64(mesh.idx.data(), mesh.idx.size() * 4, hash);
        h.sourceHash = hash;
    }
    std::string params = opt.paramString;
    if (params.empty())
    {
        char buf[160];
        std::snprintf(buf, sizeof buf, "v%u f%.3f g%u px%d pt%u", opt.maxVerts * 1000u + opt.maxTris, static_cast<f64>(opt.coneWeight), opt.groupSize, opt.proxy ? 1 : 0, opt.proxyMaxTris);
        params = buf;
    }
    h.cookParamsHash = Fnv1a32(params.data(), params.size());
    std::snprintf(h.cooker, sizeof h.cooker, "vgeo_stub 1.0.0 meshopt %u.%u", static_cast<u32>(MESHOPTIMIZER_VERSION / 1000), static_cast<u32>((MESHOPTIMIZER_VERSION / 10) % 100));

    // 5) プロキシ
    t0 = clk::now();
    if (opt.proxy)
    {
        std::vector<u32> pidx;
        f32 perr = 0.0f;
        if (mesh.triangleCount() <= opt.proxyMaxTris) pidx = mesh.idx;
        else
        {
            pidx.resize(mesh.idx.size());
            f32 rel = 0.0f;
            const size_t n = meshopt_simplify(pidx.data(), mesh.idx.data(), mesh.idx.size(), mesh.pos.data(), vcount, 12, static_cast<size_t>(opt.proxyMaxTris) * 3, 1.0f, 0, &rel);
            pidx.resize(n);
            perr = rel * meshopt_simplifyScale(mesh.pos.data(), vcount, 12);
        }
        std::vector<u32> remap(vcount, kNone);
        ProxySectionData ps;
        ps.materialIndex = 0; ps.error = perr; ps.flags = (perr == 0.0f) ? kProxyFlagExact : 0u;
        f32 plo[3] = {1e30f, 1e30f, 1e30f}, phi[3] = {-1e30f, -1e30f, -1e30f};
        ps.indices.reserve(pidx.size());
        for (u32 vi : pidx)
        {
            if (remap[vi] == kNone)
            {
                remap[vi] = static_cast<u32>(ps.vertices.size());
                ProxyVertex pv{};
                std::memcpy(pv.position, &mesh.pos[static_cast<size_t>(vi) * 3], 12);
                std::memcpy(pv.normal, &mesh.nrm[static_cast<size_t>(vi) * 3], 12);
                pv.color[0] = pv.color[1] = pv.color[2] = pv.color[3] = 1.0f;
                std::memcpy(pv.texCoord, &mesh.uv[static_cast<size_t>(vi) * 2], 8);
                pv.tangent[3] = 1.0f;
                for (int a = 0; a < 3; ++a) { plo[a] = std::min(plo[a], pv.position[a]); phi[a] = std::max(phi[a], pv.position[a]); }
                ps.vertices.push_back(pv);
            }
            ps.indices.push_back(remap[vi]);
        }
        std::memcpy(ps.aabbMin, plo, 12); std::memcpy(ps.aabbMax, phi, 12);
        if (ps.vertices.size() >= 3 && !ps.indices.empty()) out.proxy.push_back(std::move(ps));
    }
    const f64 msProxy = dx12e::vg::stub::detail::MsSince(t0);

    // 6) 統計 JSON（決定的: 時刻や所要時間は入れない）
    {
        char buf[384];
        std::snprintf(buf, sizeof buf, "{\"tool\":\"vgeo_stub\",\"label\":\"%s\",\"sourceTriangles\":%llu,\"sourceVertices\":%llu,\"clusters\":%zu,\"groups\":%zu,\"pages\":%u,\"maxClusterVerts\":%u,\"maxClusterTris\":%u}",
                      opt.label.c_str(), static_cast<unsigned long long>(mesh.triangleCount()), static_cast<unsigned long long>(vcount), nm, leaves.size(), pageCount, opt.maxVerts, opt.maxTris);
        out.debugJson = buf;
    }

    if (stats)
    {
        stats->sourceTris = mesh.triangleCount(); stats->sourceVerts = vcount;
        stats->clusters = static_cast<u32>(nm); stats->groups = static_cast<u32>(leaves.size()); stats->pages = pageCount; stats->nodes = static_cast<u32>(out.nodes.size());
        stats->avgTrisPerCluster = nm ? static_cast<f64>(sumTris) / static_cast<f64>(nm) : 0.0;
        stats->avgVertsPerCluster = nm ? static_cast<f64>(sumVerts) / static_cast<f64>(nm) : 0.0;
        stats->msQuantize = msQuant; stats->msMeshlets = msMeshlets; stats->msPack = msPack; stats->msProxy = msProxy;
        stats->msTotal = dx12e::vg::stub::detail::MsSince(tStart);
    }
    return VgeoError{};
}

} // namespace dx12e::vg::stub
