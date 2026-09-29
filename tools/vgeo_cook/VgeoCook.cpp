// VgeoCook.cpp ― 仮想ジオメトリのオフライン cooker 本体（P1）。設計: docs/VIRTUAL_GEOMETRY_DESIGN.md §2.3 / 仕様: docs/VGEO_SPEC.md。
//
//   処理の流れ（Cook）:
//     1. Ingest   : 座標変換 → 24bit 格子へ量子化 → （任意）頂点融合 → 溶接 ID → 縮退三角形の除去 → セクション（材質）別の三角形リスト
//     2. Lod0     : セクションごとに三角形を空間（Morton）チャンクへ分け、チャンク内を meshopt でクラスタ化（並列）
//     3. DAG      : 1 レベルずつ [クラスタ隣接グラフ → グループ化 → グループごとに境界頂点をロックして簡略化 → 再クラスタ]（並列）
//                   親の誤差 = max(子の誤差) + 今回の簡略化誤差（単調）。球はグループ内で共有（Nanite 方式）。
//     4. Pack     : ルートグループ → レベル降順 → Morton の順にページへ詰める（2 パス: サイズ計算 → 参照を書いて符号化）
//     5. BVH / ヘッダ / プロキシ / NONVG / debugJson
//
//   ★決定論: 並列は「作業項目（チャンク / グループ / ページ）ごとに独立に計算 → 添字順に結合」。スレッド数・実行順に依らない。
//   ★メモリ: 頂点は 24 B（格子座標 + oct 法線 + UV）で 1 本だけ持ち、クラスタは全レベルで同じ頂点配列を参照する
//            （簡略化は頂点を動かさない）。入力配列は量子化後すぐ解放する。
#include "VgeoCook.h"

#include <meshoptimizer.h>

#include <atomic>
#include <bit>
#include <cfloat>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <numeric>
#include <unordered_map>

namespace dx12e::vg::cook
{

using detail::Fmt;
using detail::MakeErr;

std::string CookOptions::CanonicalString() const
{
    // 固定順（アルファベット順ではない。バージョンで変えない）
    char buf[768];
    std::snprintf(buf, sizeof buf,
                  "cone=%.4f;dedupe=%d;groupmethod=%s;regularize=%d;nolocks=%d;permissive=%u;groupsize=%u;lock=%d;lod0=%u;lod0chunk=%u;maxtris=%u;maxverts=%u;nonvg=%u;normal=%.4f;pin=%u;proxy=%d;proxytris=%u;reduction=%.4f;"
                  "target=%.4f;uv=%.4f;vcache=%d",
                  static_cast<f64>(coneWeight), dedupe ? 1 : 0, GroupMethodName(groupMethod), regularize ? 1 : 0, debugNoLocks ? 1 : 0, permissive, groupSize, lockOpenBorders ? 1 : 0, static_cast<u32>(lod0Builder), lod0ChunkTris,
                  maxTris, maxVerts, nonVgMaxTris, static_cast<f64>(normalWeight), pinPages, proxy ? 1 : 0, proxyMaxTris, static_cast<f64>(reduction), static_cast<f64>(targetFill),
                  static_cast<f64>(uvWeight), (vertexCacheOpt ? 1 : 0) + (overdrawOpt ? 2 : 0));
    return buf;
}

VgsrcData MakeVgsrcFromTriangles(const std::vector<f32>& positions, const std::vector<u32>& indices, const std::vector<f32>* normals, const std::vector<f32>* uvs,
                                 const std::string& materialName)
{
    VgsrcData d;
    d.flags = 0;
    d.positions = positions;
    d.indices = indices;
    if (normals) { d.normals = *normals; d.flags |= kVgsrcHasNormals; }
    if (uvs) { d.uv0 = *uvs; d.flags |= kVgsrcHasUV0; }
    StringPool pool;
    MaterialRecord m = MakeDefaultMaterial();
    m.nameOff = pool.Add(materialName);
    d.materials.push_back(m);
    d.strings = pool.Data();
    d.sections.push_back(VgsrcSection{0, 0, static_cast<u32>(indices.size()), 0});
    return d;
}

namespace
{

// ════════════════════════════════════════════════════════════════════════════
// データ構造
// ════════════════════════════════════════════════════════════════════════════
struct Vtx
{
    u32 q[3];      // 24bit 格子座標
    u32 nrm;       // oct16 x 2
    f32 uv[2];
};
static_assert(sizeof(Vtx) == 24, "Vtx is 24 bytes");

struct Global
{
    std::vector<Vtx> v;
    std::vector<f32> dpos;        // 3 * V（復元後の位置）
    std::vector<u32> weld;        // V: 同じ位置の頂点の代表番号
    std::vector<u8>  permLock;    // 溶接 ID ごと（複数セクションで共有される頂点 = 材質境界。空なら無し）
    f32 origin[3] = {0, 0, 0};
    f32 step = 1.0f;
    f32 aabbMin[3] = {0, 0, 0}, aabbMax[3] = {0, 0, 0};
    size_t V() const { return v.size(); }
};

struct SecTris
{
    u32  inSection = 0;
    u32  inMat = 0;
    bool nonvg = false;
    std::vector<u32> idx;         // 三角形リスト（縮退除去済み）
};

// 1 クラスタ（全レベル共通）。頂点は Global::v の番号（vpool）、三角形はクラスタ内番号（tpool）。
struct Cl
{
    u32 vOff = 0, tOff = 0;
    u32 vc = 0, tc = 0;
    u32 level = 0, section = 0;
    f32 cull[4] = {0, 0, 0, 0};
    u32 cone = 0;
    f32 maxEdge = 0;
    f32 lodSphere[4] = {0, 0, 0, 0};
    f32 parSphere[4] = {0, 0, 0, 0};
    f32 lodErr = 0, parErr = 0;
    u32 producer = kNone;         // このクラスタを生んだグループ（LOD0 は kNone）
    u32 consumer = kNone;         // このクラスタを消費したグループ（ルートは「ルートグループ」）
};

struct Batch
{
    std::vector<Cl> cl;
    std::vector<u32> vp;
    std::vector<u8> tp;
};

struct Store
{
    std::vector<Cl> cl;
    std::vector<u32> vpool;
    std::vector<u8> tpool;
    u32 Append(Batch&& b)
    {
        const u32 base = static_cast<u32>(cl.size());
        const u32 vb = static_cast<u32>(vpool.size()), tb = static_cast<u32>(tpool.size());
        for (Cl c : b.cl) { c.vOff += vb; c.tOff += tb; cl.push_back(c); }
        vpool.insert(vpool.end(), b.vp.begin(), b.vp.end());
        tpool.insert(tpool.end(), b.tp.begin(), b.tp.end());
        b = Batch{};
        return base;
    }
};

// グループ = 「同じ親を生むクラスタの集合」（ファイルの葉が指す消費集合）。ルートグループは最上位クラスタの集合。
struct Gr
{
    u32 level = 0, section = 0;
    u32 memOff = 0, memCount = 0;   // gmembers 内
    f32 sphere[4] = {0, 0, 0, 0};   // 消費側の LOD 球（メンバーの parentLodSphere）
    f32 err = 0;
    bool root = false;
};

struct Scratch
{
    LocalMap chunkMap, groupMap;
    std::vector<u32> gidx, lv, lidx, outIdx, lweld, lockWeld;
    std::vector<f32> lpos, lattr, sp;
    std::vector<u8> lock;
    std::vector<u64> keys;
    std::vector<meshopt_Meshlet> ml;
    std::vector<u32> mv;
    std::vector<u8> mt;
    std::vector<u32> tmp;
};

f64 SphereDist(const f32* a, const f32* b)
{
    const f64 dx = static_cast<f64>(a[0]) - b[0], dy = static_cast<f64>(a[1]) - b[1], dz = static_cast<f64>(a[2]) - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ════════════════════════════════════════════════════════════════════════════
// 1. Ingest
// ════════════════════════════════════════════════════════════════════════════
VgeoError Ingest(VgsrcData& in, const CookOptions& opt, u32 threads, Global& g, std::vector<SecTris>& secs, CookStats& st)
{
    const size_t V0 = in.positions.size() / 3;
    if (in.positions.size() % 3 != 0 || V0 == 0) return MakeErr(Errc::CountMismatch, "input: positions must be a non-empty multiple of 3 floats");
    if (V0 > 0xFFFFFFF0ull) return MakeErr(Errc::LimitExceeded, "input: too many vertices");
    const bool hasN = (in.flags & kVgsrcHasNormals) != 0, hasUV = (in.flags & kVgsrcHasUV0) != 0;
    if (hasN && in.normals.size() != in.positions.size()) return MakeErr(Errc::CountMismatch, "input: normals array size disagrees with positions");
    if (hasUV && in.uv0.size() != V0 * 2) return MakeErr(Errc::CountMismatch, "input: uv0 array size disagrees with positions");
    if (in.indices.empty() || in.indices.size() % 3 != 0) return MakeErr(Errc::CountMismatch, "input: index count must be a non-empty multiple of 3");
    if (in.coordSystem > kCoordUE) return MakeErr(Errc::UnsupportedFeature, "input: unknown coordSystem");
    if (!Finite(in.unitScaleToMeters) || !(in.unitScaleToMeters > 0.0f)) return MakeErr(Errc::BadFloat, "input: unitScaleToMeters must be finite and > 0");
    if (in.sections.empty())
    {
        if (in.materials.empty()) { MaterialRecord m = MakeDefaultMaterial(); in.materials.push_back(m); }
        in.sections.push_back(VgsrcSection{0, 0, static_cast<u32>(in.indices.size()), 0});
    }
    {
        u64 cursor = 0;
        for (size_t i = 0; i < in.sections.size(); ++i)
        {
            const VgsrcSection& s = in.sections[i];
            if (s.firstIndex != cursor || s.indexCount == 0 || s.indexCount % 3 != 0) return MakeErr(Errc::SectionShape, Fmt("input: section %zu does not tile the index array", i), static_cast<i64>(i));
            if (s.materialIndex >= in.materials.size()) return MakeErr(Errc::MaterialInvalid, Fmt("input: section %zu references material %u out of range", i, s.materialIndex), static_cast<i64>(i));
            cursor += s.indexCount;
        }
        if (cursor != in.indices.size()) return MakeErr(Errc::SectionShape, "input: sections do not cover the whole index array");
    }
    // 範囲 / 有限性（並列）
    std::atomic<int> bad{0};
    ParallelFor(in.indices.size(), threads, 1u << 20, opt.lowPriority, [&](size_t b, size_t e, u32)
    {
        for (size_t i = b; i < e; ++i) if (in.indices[i] >= V0) { bad.store(1); return; }
    });
    if (bad.load()) return MakeErr(Errc::ClusterInvalid, "input: a triangle index is >= vertexCount");
    auto finiteRange = [&](const std::vector<f32>& a, const char* what) -> VgeoError
    {
        std::atomic<int> nf{0};
        ParallelFor(a.size(), threads, 1u << 20, opt.lowPriority, [&](size_t b, size_t e, u32)
        {
            for (size_t i = b; i < e; ++i) if (!Finite(a[i])) { nf.store(1); return; }
        });
        return nf.load() ? MakeErr(Errc::BadFloat, Fmt("input: a %s value is not finite", what)) : VgeoError{};
    };
    if (VgeoError e = finiteRange(in.positions, "position"); !e.ok()) return e;
    if (hasN) if (VgeoError e = finiteRange(in.normals, "normal"); !e.ok()) return e;
    if (hasUV) if (VgeoError e = finiteRange(in.uv0, "uv"); !e.ok()) return e;

    st.inputTris = in.indices.size() / 3;
    st.inputVerts = V0;

    // エンジン空間へ（並列）
    std::atomic<int> overflow{0};
    ParallelFor(V0, threads, 1u << 16, opt.lowPriority, [&](size_t b, size_t e, u32)
    {
        for (size_t v = b; v < e; ++v)
        {
            f32 o[3];
            ConvertPositionToEngine(in.coordSystem, in.unitScaleToMeters, &in.positions[v * 3], o);
            if (!Finite(o[0]) || !Finite(o[1]) || !Finite(o[2])) overflow.store(1);
            std::memcpy(&in.positions[v * 3], o, 12);
            if (hasN) { f32 n[3]; ConvertNormalToEngine(in.coordSystem, &in.normals[v * 3], n); std::memcpy(&in.normals[v * 3], n, 12); }
        }
    });
    if (overflow.load()) return MakeErr(Errc::BadFloat, "input: a position overflows after the unit conversion");

    // 使われている頂点の AABB
    std::vector<u8> used(V0, 0);
    for (u32 i : in.indices) used[i] = 1;
    f64 lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (size_t v = 0; v < V0; ++v)
    {
        if (!used[v]) continue;
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], static_cast<f64>(in.positions[v * 3 + a])); hi[a] = std::max(hi[a], static_cast<f64>(in.positions[v * 3 + a])); }
    }
    used = std::vector<u8>{};
    f64 maxExtent = 0;
    for (int a = 0; a < 3; ++a)
    {
        g.origin[a] = static_cast<f32>(lo[a]);
        g.aabbMin[a] = g.origin[a];
        g.aabbMax[a] = static_cast<f32>(hi[a]);
        maxExtent = std::max(maxExtent, static_cast<f64>(g.aabbMax[a]) - static_cast<f64>(g.origin[a]));
    }
    g.step = ComputePosStep(maxExtent);

    // 量子化 + 法線 oct + UV
    g.v.resize(V0);
    ParallelFor(V0, threads, 1u << 16, opt.lowPriority, [&](size_t b, size_t e, u32)
    {
        for (size_t v = b; v < e; ++v)
        {
            Vtx& x = g.v[v];
            for (int a = 0; a < 3; ++a) x.q[a] = QuantizeCoord(in.positions[v * 3 + a], g.origin[a], g.step);
            x.nrm = hasN ? EncodeOct16(in.normals[v * 3], in.normals[v * 3 + 1], in.normals[v * 3 + 2]) : EncodeOct16(0, 0, 1);
            x.uv[0] = hasUV ? in.uv0[v * 2] : 0.0f;
            x.uv[1] = hasUV ? in.uv0[v * 2 + 1] : 0.0f;
        }
    });
    in.positions = std::vector<f32>{};
    in.normals = std::vector<f32>{};
    in.uv0 = std::vector<f32>{};

    // 頂点融合（位置 + 法線 + UV が同一）
    if (opt.dedupe)
    {
        std::vector<u32> remap(V0);
        const size_t U = meshopt_generateVertexRemap(remap.data(), nullptr, V0, g.v.data(), V0, sizeof(Vtx));
        if (U < V0)
        {
            std::vector<Vtx> nv(U);
            meshopt_remapVertexBuffer(nv.data(), g.v.data(), V0, sizeof(Vtx), remap.data());
            g.v.swap(nv);
            ParallelFor(in.indices.size(), threads, 1u << 20, opt.lowPriority, [&](size_t b, size_t e, u32)
            {
                for (size_t i = b; i < e; ++i) in.indices[i] = remap[in.indices[i]];
            });
            st.verticesMerged = V0 - U;
        }
    }
    const size_t V = g.v.size();

    // 復元位置 + 溶接 ID
    g.dpos.resize(V * 3);
    ParallelFor(V, threads, 1u << 16, opt.lowPriority, [&](size_t b, size_t e, u32)
    {
        for (size_t v = b; v < e; ++v)
            for (int a = 0; a < 3; ++a) g.dpos[v * 3 + a] = DequantizeCoord(g.v[v].q[a], g.origin[a], g.step);
    });
    g.weld.resize(V);
    meshopt_generatePositionRemap(g.weld.data(), g.dpos.data(), V, 12);
    if (opt.verbose)
    {
        size_t uniq = 0;
        for (size_t v = 0; v < V; ++v) if (g.weld[v] == v) ++uniq;
        std::fprintf(stderr, "[cook] ingest: vertices %zu (merged %llu), unique positions %zu\n", V, static_cast<unsigned long long>(st.verticesMerged), uniq);
    }

    // 法線が無い入力: 位置で溶接した面積重みの滑らかな法線
    if (!hasN)
    {
        std::vector<f64> acc(V * 3, 0.0);
        for (size_t t = 0; t + 2 < in.indices.size(); t += 3)
        {
            const u32 a = in.indices[t], b = in.indices[t + 1], c = in.indices[t + 2];
            const f32* p0 = &g.dpos[static_cast<size_t>(a) * 3];
            const f32* p1 = &g.dpos[static_cast<size_t>(b) * 3];
            const f32* p2 = &g.dpos[static_cast<size_t>(c) * 3];
            const f64 e1[3] = {static_cast<f64>(p1[0]) - p0[0], static_cast<f64>(p1[1]) - p0[1], static_cast<f64>(p1[2]) - p0[2]};
            const f64 e2[3] = {static_cast<f64>(p2[0]) - p0[0], static_cast<f64>(p2[1]) - p0[1], static_cast<f64>(p2[2]) - p0[2]};
            const f64 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            for (u32 w : {g.weld[a], g.weld[b], g.weld[c]})
                for (int k = 0; k < 3; ++k) acc[static_cast<size_t>(w) * 3 + k] += n[k];
        }
        for (size_t v = 0; v < V; ++v)
        {
            const f64* n = &acc[static_cast<size_t>(g.weld[v]) * 3];
            g.v[v].nrm = (n[0] == 0 && n[1] == 0 && n[2] == 0) ? EncodeOct16(0, 1, 0) : EncodeOct16(n[0], n[1], n[2]);
        }
    }

    // セクションごとの三角形（縮退を除く）
    auto keep = [&](u32 a, u32 b, u32 c) -> bool
    {
        if (a == b || b == c || a == c) return false;
        const u32 wa = g.weld[a], wb = g.weld[b], wc = g.weld[c];
        if (wa == wb || wb == wc || wa == wc) return false;
        const f32* p0 = &g.dpos[static_cast<size_t>(a) * 3];
        const f32* p1 = &g.dpos[static_cast<size_t>(b) * 3];
        const f32* p2 = &g.dpos[static_cast<size_t>(c) * 3];
        const f64 e1[3] = {static_cast<f64>(p1[0]) - p0[0], static_cast<f64>(p1[1]) - p0[1], static_cast<f64>(p1[2]) - p0[2]};
        const f64 e2[3] = {static_cast<f64>(p2[0]) - p0[0], static_cast<f64>(p2[1]) - p0[1], static_cast<f64>(p2[2]) - p0[2]};
        const f64 n0 = e1[1] * e2[2] - e1[2] * e2[1], n1 = e1[2] * e2[0] - e1[0] * e2[2], n2 = e1[0] * e2[1] - e1[1] * e2[0];
        return !(n0 == 0.0 && n1 == 0.0 && n2 == 0.0);
    };
    secs.clear();
    u64 dropped = 0;
    for (size_t si = 0; si < in.sections.size(); ++si)
    {
        const VgsrcSection& s = in.sections[si];
        const size_t nT = s.indexCount / 3, first = s.firstIndex;
        const size_t blockT = 1u << 15;
        const size_t nb = (nT + blockT - 1) / blockT;
        std::vector<u32> cnt(nb, 0);
        ParallelFor(nb, threads, 1, opt.lowPriority, [&](size_t b, size_t e, u32)
        {
            for (size_t k = b; k < e; ++k)
            {
                const size_t t0 = k * blockT, t1 = std::min(nT, t0 + blockT);
                u32 c = 0;
                for (size_t t = t0; t < t1; ++t) c += keep(in.indices[first + t * 3], in.indices[first + t * 3 + 1], in.indices[first + t * 3 + 2]) ? 1u : 0u;
                cnt[k] = c;
            }
        });
        std::vector<size_t> off(nb + 1, 0);
        for (size_t k = 0; k < nb; ++k) off[k + 1] = off[k] + cnt[k];
        SecTris out;
        out.inSection = static_cast<u32>(si);
        out.inMat = s.materialIndex;
        out.idx.resize(off[nb] * 3);
        ParallelFor(nb, threads, 1, opt.lowPriority, [&](size_t b, size_t e, u32)
        {
            for (size_t k = b; k < e; ++k)
            {
                const size_t t0 = k * blockT, t1 = std::min(nT, t0 + blockT);
                size_t w = off[k] * 3;
                for (size_t t = t0; t < t1; ++t)
                {
                    const u32 a = in.indices[first + t * 3], bb = in.indices[first + t * 3 + 1], c = in.indices[first + t * 3 + 2];
                    if (!keep(a, bb, c)) continue;
                    out.idx[w++] = a; out.idx[w++] = bb; out.idx[w++] = c;
                }
            }
        });
        dropped += nT - off[nb];
        if (!out.idx.empty()) secs.push_back(std::move(out));
    }
    st.degenerateDropped = dropped;
    in.indices = std::vector<u32>{};
    if (secs.empty()) return MakeErr(Errc::CountMismatch, "input: no valid (non-degenerate) triangles");
    return VgeoError{};
}

// ════════════════════════════════════════════════════════════════════════════
// 2. クラスタ生成（LOD0 チャンク / 簡略化後の再クラスタ 共通）
// ════════════════════════════════════════════════════════════════════════════
// ローカル頂点配列（lpos = 復元位置 3*nv, lv = ローカル → Global 番号）と三角形リスト lidx から meshlet を作り、Batch へ追加する。
void EmitClusters(Scratch& sc, const f32* lpos, u32 nv, const u32* lv, const u32* lidx, size_t nIdx, const CookOptions& opt, u32 section, Batch& out)
{
    if (nIdx == 0) return;
    const size_t maxT = opt.maxTris, maxV = opt.maxVerts;
    const size_t minT = std::max<size_t>(4, (maxT / 2) / 4 * 4);
    const size_t bound = meshopt_buildMeshletsBound(nIdx, maxV, opt.lod0Builder == Lod0Builder::Classic ? maxT : minT);
    sc.ml.resize(bound);
    sc.mv.resize(nIdx);
    sc.mt.resize(nIdx);
    size_t nm = 0;
    switch (opt.lod0Builder)
    {
    case Lod0Builder::Classic:
        nm = meshopt_buildMeshlets(sc.ml.data(), sc.mv.data(), sc.mt.data(), lidx, nIdx, lpos, nv, 12, maxV, maxT, opt.coneWeight);
        break;
    case Lod0Builder::Flex:
        nm = meshopt_buildMeshletsFlex(sc.ml.data(), sc.mv.data(), sc.mt.data(), lidx, nIdx, lpos, nv, 12, maxV, minT, maxT, opt.coneWeight, 0.0f);
        break;
    case Lod0Builder::Spatial:
        nm = meshopt_buildMeshletsSpatial(sc.ml.data(), sc.mv.data(), sc.mt.data(), lidx, nIdx, lpos, nv, 12, maxV, minT, maxT, 0.5f);
        break;
    }
    for (size_t i = 0; i < nm; ++i)
    {
        const meshopt_Meshlet& m = sc.ml[i];
        u32* mvp = sc.mv.data() + m.vertex_offset;
        u8* mtp = sc.mt.data() + m.triangle_offset;
        meshopt_optimizeMeshlet(mvp, mtp, m.triangle_count, m.vertex_count);
        const meshopt_Bounds b = meshopt_computeMeshletBounds(mvp, mtp, m.triangle_count, lpos, nv, 12);
        Cl c;
        c.vOff = static_cast<u32>(out.vp.size());
        c.tOff = static_cast<u32>(out.tp.size());
        c.vc = m.vertex_count;
        c.tc = m.triangle_count;
        c.section = section;
        c.cull[0] = b.center[0]; c.cull[1] = b.center[1]; c.cull[2] = b.center[2];
        f64 rmax = b.radius;
        for (u32 v = 0; v < m.vertex_count; ++v)
        {
            const f32* pv = lpos + static_cast<size_t>(mvp[v]) * 3;
            const f64 dx = static_cast<f64>(pv[0]) - c.cull[0], dy = static_cast<f64>(pv[1]) - c.cull[1], dz = static_cast<f64>(pv[2]) - c.cull[2];
            rmax = std::max(rmax, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        c.cull[3] = std::nextafter(static_cast<f32>(rmax), std::numeric_limits<f32>::infinity());
        auto cl127 = [](signed char v) -> i8 { return static_cast<i8>(v < -127 ? -127 : v); };
        c.cone = PackConeS8(cl127(b.cone_axis_s8[0]), cl127(b.cone_axis_s8[1]), cl127(b.cone_axis_s8[2]), cl127(b.cone_cutoff_s8));
        f64 me2 = 0.0;
        for (u32 t = 0; t < m.triangle_count; ++t)
            for (u32 e = 0; e < 3; ++e)
            {
                const f32* pa = lpos + static_cast<size_t>(mvp[mtp[t * 3 + e]]) * 3;
                const f32* pb = lpos + static_cast<size_t>(mvp[mtp[t * 3 + (e + 1) % 3]]) * 3;
                const f64 dx = static_cast<f64>(pa[0]) - pb[0], dy = static_cast<f64>(pa[1]) - pb[1], dz = static_cast<f64>(pa[2]) - pb[2];
                me2 = std::max(me2, dx * dx + dy * dy + dz * dz);
            }
        c.maxEdge = static_cast<f32>(std::sqrt(me2));
        for (u32 v = 0; v < m.vertex_count; ++v) out.vp.push_back(lv[mvp[v]]);
        out.tp.insert(out.tp.end(), mtp, mtp + static_cast<size_t>(m.triangle_count) * 3);
        out.cl.push_back(c);
    }
}

// LOD0: セクションの三角形を空間チャンクへ分けてクラスタ化する。戻り値 = 追加したクラスタの先頭番号。
u32 BuildLod0(const Global& g, const std::vector<u32>& tris, const CookOptions& opt, u32 threads, std::vector<Scratch>& scratch, u32 section, Store& st, u32* countOut)
{
    const size_t nT = tris.size() / 3;
    // 三角形の重心の Morton キーで並べる（30bit。同キーは元の順 = 安定）
    f64 inv[3], lo[3];
    for (int a = 0; a < 3; ++a)
    {
        lo[a] = g.aabbMin[a];
        const f64 ext = static_cast<f64>(g.aabbMax[a]) - static_cast<f64>(g.aabbMin[a]);
        inv[a] = ext > 0 ? 1023.0 / ext : 0.0;
    }
    std::vector<u64> kv(nT);
    ParallelFor(nT, threads, 1u << 16, opt.lowPriority, [&](size_t b, size_t e, u32)
    {
        for (size_t t = b; t < e; ++t)
        {
            f64 c[3] = {0, 0, 0};
            for (int k = 0; k < 3; ++k)
                for (int a = 0; a < 3; ++a) c[a] += static_cast<f64>(g.dpos[static_cast<size_t>(tris[t * 3 + k]) * 3 + a]);
            u32 q[3];
            for (int a = 0; a < 3; ++a)
            {
                f64 x = (c[a] / 3.0 - lo[a]) * inv[a];
                x = std::min(1023.0, std::max(0.0, x));
                q[a] = static_cast<u32>(x);
            }
            kv[t] = (static_cast<u64>(Morton30(q[0], q[1], q[2])) << 32) | static_cast<u64>(t);
        }
    });
    RadixSortByKey32(kv, 30);

    const size_t S = std::max<size_t>(opt.lod0ChunkTris, 1024);
    const size_t nChunks = std::max<size_t>(1, (nT + S - 1) / S);
    std::vector<Batch> outs(nChunks);
    ParallelFor(nChunks, threads, 1, opt.lowPriority, [&](size_t b, size_t e, u32 tid)
    {
        Scratch& sc = scratch[tid];
        for (size_t ci = b; ci < e; ++ci)
        {
            const size_t t0 = ci * nT / nChunks, t1 = (ci + 1) * nT / nChunks;
            const size_t n = t1 - t0;
            sc.gidx.resize(n * 3);
            for (size_t k = 0; k < n; ++k)
            {
                const size_t t = static_cast<size_t>(kv[t0 + k] & 0xFFFFFFFFull);
                sc.gidx[k * 3] = tris[t * 3]; sc.gidx[k * 3 + 1] = tris[t * 3 + 1]; sc.gidx[k * 3 + 2] = tris[t * 3 + 2];
            }
            sc.chunkMap.Reset(sc.gidx.size());
            sc.lv.clear();
            sc.lidx.resize(sc.gidx.size());
            for (size_t i = 0; i < sc.gidx.size(); ++i)
            {
                bool added = false;
                const u32 l = sc.chunkMap.GetOrAdd(sc.gidx[i], static_cast<u32>(sc.lv.size()), added);
                if (added) sc.lv.push_back(sc.gidx[i]);
                sc.lidx[i] = l;
            }
            const u32 nv = static_cast<u32>(sc.lv.size());
            sc.lpos.resize(static_cast<size_t>(nv) * 3);
            for (u32 v = 0; v < nv; ++v) std::memcpy(&sc.lpos[static_cast<size_t>(v) * 3], &g.dpos[static_cast<size_t>(sc.lv[v]) * 3], 12);
            if (opt.vertexCacheOpt) meshopt_optimizeVertexCache(sc.lidx.data(), sc.lidx.data(), sc.lidx.size(), nv);
            if (opt.vertexCacheOpt && opt.overdrawOpt) meshopt_optimizeOverdraw(sc.lidx.data(), sc.lidx.data(), sc.lidx.size(), sc.lpos.data(), nv, 12, 1.05f);
            EmitClusters(sc, sc.lpos.data(), nv, sc.lv.data(), sc.lidx.data(), sc.lidx.size(), opt, section, outs[ci]);
        }
    });
    kv = std::vector<u64>{};
    const u32 first = static_cast<u32>(st.cl.size());
    for (Batch& b : outs) st.Append(std::move(b));
    *countOut = static_cast<u32>(st.cl.size()) - first;
    for (u32 i = first; i < st.cl.size(); ++i)
    {
        Cl& c = st.cl[i];
        std::memcpy(c.lodSphere, c.cull, 16);
        c.lodErr = 0.0f;
        c.level = 0;
    }
    return first;
}

// ════════════════════════════════════════════════════════════════════════════
// 3. DAG
// ════════════════════════════════════════════════════════════════════════════
struct LevelGraph
{
    ClusterGraph g;
    std::vector<u64> border;    // 元メッシュの開いた境界エッジ（このレベルのメッシュで、1 クラスタにしか現れない奇数出現のエッジ）。昇順
    u64 shared = 0;
};

struct Ent { u64 key; u32 ci; };

void BuildLevelGraph(const Store& st, const Global& gl, const std::vector<u32>& cur, u32 threads, bool lowPrio, LevelGraph& lg)
{
    const size_t n = cur.size();
    const size_t block = 256;
    const size_t nb = (n + block - 1) / block;
    std::vector<std::vector<Ent>> parts(nb);
    ParallelFor(nb, threads, 1, lowPrio, [&](size_t b, size_t e, u32)
    {
        std::vector<u64> keys;
        for (size_t blk = b; blk < e; ++blk)
        {
            const size_t i0 = blk * block, i1 = std::min(n, i0 + block);
            for (size_t i = i0; i < i1; ++i)
            {
                const Cl& c = st.cl[cur[i]];
                keys.clear();
                for (u32 t = 0; t < c.tc; ++t)
                {
                    u32 w[3];
                    for (int k = 0; k < 3; ++k) w[k] = gl.weld[st.vpool[c.vOff + st.tpool[c.tOff + t * 3 + k]]];
                    for (int k = 0; k < 3; ++k)
                    {
                        const u32 a = w[k], bb = w[(k + 1) % 3];
                        if (a == bb) continue;
                        keys.push_back((static_cast<u64>(std::min(a, bb)) << 32) | std::max(a, bb));
                    }
                }
                std::sort(keys.begin(), keys.end());
                for (size_t s = 0; s < keys.size();)
                {
                    size_t e2 = s + 1;
                    while (e2 < keys.size() && keys[e2] == keys[s]) ++e2;
                    if (((e2 - s) & 1u) != 0) parts[blk].push_back(Ent{keys[s], static_cast<u32>(i)});
                    s = e2;
                }
            }
        }
    });
    size_t total = 0;
    for (auto& p : parts) total += p.size();
    std::vector<Ent> all;
    all.reserve(total);
    for (auto& p : parts) { all.insert(all.end(), p.begin(), p.end()); p = std::vector<Ent>{}; }
    ParallelSort(all, [](const Ent& a, const Ent& b) { return a.key != b.key ? a.key < b.key : a.ci < b.ci; }, threads, lowPrio);

    lg.border.clear();
    std::vector<u64> pairs;
    for (size_t s = 0; s < all.size();)
    {
        size_t e = s + 1;
        while (e < all.size() && all[e].key == all[s].key) ++e;
        if (e - s == 1) lg.border.push_back(all[s].key);
        else
            for (size_t k = s; k + 1 < e; ++k)
            {
                const u32 a = all[k].ci, b = all[k + 1].ci;
                if (a != b) pairs.push_back((static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b));
            }
        s = e;
    }
    all = std::vector<Ent>{};
    ParallelSort(pairs, [](u64 a, u64 b) { return a < b; }, threads, lowPrio);
    struct AW { u32 a, b, w; };
    std::vector<AW> aw;
    for (size_t s = 0; s < pairs.size();)
    {
        size_t e = s + 1;
        while (e < pairs.size() && pairs[e] == pairs[s]) ++e;
        aw.push_back(AW{static_cast<u32>(pairs[s] >> 32), static_cast<u32>(pairs[s] & 0xFFFFFFFFull), static_cast<u32>(e - s)});
        lg.shared += e - s;
        s = e;
    }
    pairs = std::vector<u64>{};
    ClusterGraph& cg = lg.g;
    cg.n = static_cast<u32>(n);
    cg.off.assign(n + 1, 0);
    for (const AW& x : aw) { ++cg.off[x.a + 1]; ++cg.off[x.b + 1]; }
    for (size_t i = 0; i < n; ++i) cg.off[i + 1] += cg.off[i];
    cg.nbr.resize(cg.off[n]);
    cg.w.resize(cg.off[n]);
    std::vector<u32> fill(cg.off.begin(), cg.off.end() - 1);
    for (const AW& x : aw)
    {
        cg.nbr[fill[x.a]] = x.b; cg.w[fill[x.a]++] = x.w;
        cg.nbr[fill[x.b]] = x.a; cg.w[fill[x.b]++] = x.w;
    }
}

struct DagCtx
{
    const Global& g;
    const Store& st;
    const CookOptions& opt;
    const std::vector<u64>* border = nullptr;
};

struct GroupOut
{
    Batch products;
    f32 sphere[4] = {0, 0, 0, 0};
    f32 err = 0.0f;
    f32 simErr = 0.0f;
    u32 trisIn = 0, trisOut = 0, verts = 0, lockedVerts = 0, permissiveUsed = 0;
    bool reached = false;
};

void SimplifyGroup(const DagCtx& cx, const u32* members, u32 nMembers, bool final, Scratch& sc, GroupOut& out)
{
    const Store& st = cx.st;
    const Global& g = cx.g;
    const CookOptions& opt = cx.opt;

    // 1) 三角形（Global 頂点番号）を集める
    sc.gidx.clear();
    f32 childMaxErr = 0.0f;
    u32 trisIn = 0;
    for (u32 i = 0; i < nMembers; ++i)
    {
        const Cl& c = st.cl[members[i]];
        childMaxErr = std::max(childMaxErr, c.lodErr);
        trisIn += c.tc;
        for (u32 k = 0; k < c.tc * 3; ++k) sc.gidx.push_back(st.vpool[c.vOff + st.tpool[c.tOff + k]]);
    }
    // 2) ローカル頂点
    sc.groupMap.Reset(sc.gidx.size());
    sc.lv.clear();
    sc.lidx.resize(sc.gidx.size());
    for (size_t i = 0; i < sc.gidx.size(); ++i)
    {
        bool added = false;
        const u32 l = sc.groupMap.GetOrAdd(sc.gidx[i], static_cast<u32>(sc.lv.size()), added);
        if (added) sc.lv.push_back(sc.gidx[i]);
        sc.lidx[i] = l;
    }
    const u32 nv = static_cast<u32>(sc.lv.size());
    sc.lpos.resize(static_cast<size_t>(nv) * 3);
    sc.lattr.resize(static_cast<size_t>(nv) * 5);
    sc.lweld.resize(nv);
    f32 uvLo[2] = {1e30f, 1e30f}, uvHi[2] = {-1e30f, -1e30f};
    for (u32 v = 0; v < nv; ++v)
    {
        const u32 gv = sc.lv[v];
        std::memcpy(&sc.lpos[static_cast<size_t>(v) * 3], &g.dpos[static_cast<size_t>(gv) * 3], 12);
        sc.lweld[v] = g.weld[gv];
        DecodeOct16(g.v[gv].nrm, &sc.lattr[static_cast<size_t>(v) * 5]);
        for (int a = 0; a < 2; ++a)
        {
            sc.lattr[static_cast<size_t>(v) * 5 + 3 + a] = g.v[gv].uv[a];
            uvLo[a] = std::min(uvLo[a], g.v[gv].uv[a]); uvHi[a] = std::max(uvHi[a], g.v[gv].uv[a]);
        }
    }
    {
        const f32 range = std::max(uvHi[0] - uvLo[0], uvHi[1] - uvLo[1]);
        const f32 inv = range > 1e-20f ? 1.0f / range : 0.0f;
        for (u32 v = 0; v < nv; ++v)
            for (int a = 0; a < 2; ++a) sc.lattr[static_cast<size_t>(v) * 5 + 3 + a] = (sc.lattr[static_cast<size_t>(v) * 5 + 3 + a] - uvLo[a]) * inv;
    }
    // 3) ロック: グループ外周（他のグループへ続く奇数出現のエッジ）の両端点 + 材質境界（永久ロック）
    sc.keys.clear();
    for (size_t t = 0; t + 2 < sc.lidx.size(); t += 3)
        for (int e = 0; e < 3; ++e)
        {
            const u32 a = sc.lweld[sc.lidx[t + e]], b = sc.lweld[sc.lidx[t + (e + 1) % 3]];
            if (a == b) continue;
            sc.keys.push_back((static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b));
        }
    std::sort(sc.keys.begin(), sc.keys.end());
    sc.lockWeld.clear();
    for (size_t s = 0; s < sc.keys.size();)
    {
        size_t e = s + 1;
        while (e < sc.keys.size() && sc.keys[e] == sc.keys[s]) ++e;
        if (((e - s) & 1u) != 0)
        {
            const bool isOpenBorder = cx.border && std::binary_search(cx.border->begin(), cx.border->end(), sc.keys[s]);
            if (!opt.debugNoLocks && (!isOpenBorder || opt.lockOpenBorders))
            {
                sc.lockWeld.push_back(static_cast<u32>(sc.keys[s] >> 32));
                sc.lockWeld.push_back(static_cast<u32>(sc.keys[s] & 0xFFFFFFFFull));
            }
        }
        s = e;
    }
    std::sort(sc.lockWeld.begin(), sc.lockWeld.end());
    sc.lockWeld.erase(std::unique(sc.lockWeld.begin(), sc.lockWeld.end()), sc.lockWeld.end());
    sc.lock.assign(nv, 0);
    u32 locked = 0;
    for (u32 v = 0; v < nv; ++v)
    {
        const u32 w = sc.lweld[v];
        if (std::binary_search(sc.lockWeld.begin(), sc.lockWeld.end(), w) || (!g.permLock.empty() && g.permLock[w])) { sc.lock[v] = static_cast<u8>(meshopt_SimplifyVertex_Lock); ++locked; }
    }
    // 4) 簡略化 → 再クラスタ
    // 目標三角形数: 通常は「入力三角形数 * reduction」（三角形数を半分にする。クラスタ数ではなく三角形数で決めるので DAG 全体の三角形が 2 倍に収まる）。
    // 最終グループはルート 1 クラスタ（maxTris * targetFill）。
    const f64 fillTris = static_cast<f64>(opt.maxTris) * static_cast<f64>(opt.targetFill);
    size_t targetTris = final ? std::max<size_t>(1, static_cast<size_t>(fillTris))
                              : std::max<size_t>(1, static_cast<size_t>(std::llround(static_cast<f64>(trisIn) * static_cast<f64>(opt.reduction))));
    const f32 weights[5] = {opt.normalWeight, opt.normalWeight, opt.normalWeight, opt.uvWeight, opt.uvWeight};
    f32 simErr = 0.0f;
    size_t nOut = 0;
    Batch products;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        products = Batch{};
        simErr = 0.0f;
        if (trisIn > targetTris)
        {
            sc.outIdx.resize(sc.lidx.size());
            unsigned options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute;
            if (opt.permissive == 1) options |= meshopt_SimplifyPermissive;
            if (opt.regularize) options |= meshopt_SimplifyRegularize;
            nOut = meshopt_simplifyWithAttributes(sc.outIdx.data(), sc.lidx.data(), sc.lidx.size(), sc.lpos.data(), nv, 12, sc.lattr.data(), 20, weights, 5,
                                                  sc.lock.data(), targetTris * 3, FLT_MAX, options, &simErr);
            if (!(simErr >= 0.0f)) simErr = 0.0f;
            if (opt.permissive == 2 && nOut > targetTris * 3 + targetTris * 3 / 8)
            {
                // 自動: 通常の簡略化が目標に届かない（継ぎ目・極の扇の制約）→ 属性の不連続をまたぐ縮約を許して再試行し、進んだ方を採る
                sc.tmp.resize(sc.lidx.size());
                f32 err2 = 0.0f;
                const size_t n2 = meshopt_simplifyWithAttributes(sc.tmp.data(), sc.lidx.data(), sc.lidx.size(), sc.lpos.data(), nv, 12, sc.lattr.data(), 20, weights, 5, sc.lock.data(),
                                                                 targetTris * 3, FLT_MAX, options | meshopt_SimplifyPermissive, &err2);
                if (n2 < nOut)
                {
                    sc.outIdx.assign(sc.tmp.begin(), sc.tmp.begin() + static_cast<std::ptrdiff_t>(n2));
                    nOut = n2;
                    simErr = (err2 >= 0.0f) ? err2 : 0.0f;
                    ++out.permissiveUsed;
                }
            }
        }
        else
        {
            sc.outIdx.assign(sc.lidx.begin(), sc.lidx.end());
            nOut = sc.outIdx.size();
        }
        if (nOut == 0)
        {
            // グループが丸ごと消える場合の規則: 最低 1 三角形は残す（最大面積の三角形）。誤差はグループのジオメトリの大きさ。
            size_t best = 0;
            f64 bestA = -1.0;
            for (size_t t = 0; t + 2 < sc.lidx.size(); t += 3)
            {
                const f32* p0 = &sc.lpos[static_cast<size_t>(sc.lidx[t]) * 3];
                const f32* p1 = &sc.lpos[static_cast<size_t>(sc.lidx[t + 1]) * 3];
                const f32* p2 = &sc.lpos[static_cast<size_t>(sc.lidx[t + 2]) * 3];
                const f64 e1[3] = {static_cast<f64>(p1[0]) - p0[0], static_cast<f64>(p1[1]) - p0[1], static_cast<f64>(p1[2]) - p0[2]};
                const f64 e2[3] = {static_cast<f64>(p2[0]) - p0[0], static_cast<f64>(p2[1]) - p0[1], static_cast<f64>(p2[2]) - p0[2]};
                const f64 n0 = e1[1] * e2[2] - e1[2] * e2[1], n1 = e1[2] * e2[0] - e1[0] * e2[2], n2 = e1[0] * e2[1] - e1[1] * e2[0];
                const f64 a = n0 * n0 + n1 * n1 + n2 * n2;
                if (a > bestA) { bestA = a; best = t; }
            }
            sc.outIdx.assign(sc.lidx.begin() + static_cast<std::ptrdiff_t>(best), sc.lidx.begin() + static_cast<std::ptrdiff_t>(best) + 3);
            nOut = 3;
            const meshopt_Bounds gb = meshopt_computeSphereBounds(sc.lpos.data(), nv, 12, nullptr, 0);
            simErr = std::max(simErr, gb.radius);
        }
        EmitClusters(sc, sc.lpos.data(), nv, sc.lv.data(), sc.outIdx.data(), nOut, opt, st.cl[members[0]].section, products);
        if (!final || products.cl.size() <= 1) break;
        targetTris = std::max<size_t>(4, static_cast<size_t>(static_cast<f64>(targetTris) * 0.7));   // ルートが 1 クラスタに収まらない: もっと削る
    }
    out.products = std::move(products);
    out.simErr = simErr;
    out.trisIn = trisIn;
    out.trisOut = static_cast<u32>(nOut / 3);
    out.verts = nv;
    out.lockedVerts = locked;
    out.reached = out.trisOut <= static_cast<u32>(static_cast<f64>(targetTris) * 1.02 + 1);
    if (opt.verbose && out.trisOut + 4 >= out.trisIn && out.trisIn > 200)
    {
        // 診断: 簡略化が進まなかったグループの位相
        u32 odd = 0, oddInBorder = 0, over2 = 0, distinctW = 0;
        for (size_t s = 0; s < sc.keys.size();)
        {
            size_t e = s + 1;
            while (e < sc.keys.size() && sc.keys[e] == sc.keys[s]) ++e;
            if ((e - s) & 1u) { ++odd; if (cx.border && std::binary_search(cx.border->begin(), cx.border->end(), sc.keys[s])) ++oddInBorder; }
            if (e - s > 2) ++over2;
            s = e;
        }
        std::vector<u32> ws(sc.lweld.begin(), sc.lweld.end());
        std::sort(ws.begin(), ws.end());
        distinctW = static_cast<u32>(std::unique(ws.begin(), ws.end()) - ws.begin());
        std::fprintf(stderr, "[cook]   stuck group: members %u tris %u->%u verts %u distinctPos %u oddEdges %u (in border set %u) nonManifoldEdges %u locked %u target %zu simErr %g\n", nMembers, out.trisIn, out.trisOut, nv,
                     distinctW, odd, oddInBorder, over2, locked, targetTris, static_cast<f64>(simErr));
    }

    // 5) 誤差と球（グループ内で共有）
    out.err = childMaxErr + simErr;
    sc.sp.resize(static_cast<size_t>(nMembers) * 4);
    for (u32 i = 0; i < nMembers; ++i) std::memcpy(&sc.sp[static_cast<size_t>(i) * 4], st.cl[members[i]].lodSphere, 16);
    const meshopt_Bounds b = meshopt_computeSphereBounds(sc.sp.data(), nMembers, 16, sc.sp.data() + 3, 16);
    f64 r = b.radius;
    for (u32 i = 0; i < nMembers; ++i) r = std::max(r, SphereDist(b.center, &sc.sp[static_cast<size_t>(i) * 4]) + static_cast<f64>(sc.sp[static_cast<size_t>(i) * 4 + 3]));
    out.sphere[0] = b.center[0]; out.sphere[1] = b.center[1]; out.sphere[2] = b.center[2];
    out.sphere[3] = std::nextafter(static_cast<f32>(r * (1.0 + 1e-6)), std::numeric_limits<f32>::infinity());
}

struct DagBuilder
{
    const Global& g;
    const CookOptions& opt;
    u32 threads;
    Store& st;
    std::vector<Gr>& groups;
    std::vector<u32>& gmembers;
    std::vector<Scratch>& scratch;
    std::vector<LevelStat>& levels;
    CookStats& stats;

    LevelStat& LS(u32 level)
    {
        while (levels.size() <= level) { LevelStat s; s.level = static_cast<u32>(levels.size()); levels.push_back(s); }
        return levels[level];
    }

    // ルートグループ: メンバーは parentLodError = +INF / parentLodSphere = 自分の lodSphere。1 グループは最大 15 クラスタなので、超えるときは複数のルートグループに分ける。
    void MakeRootGroup(u32 sec, u32 level, const std::vector<u32>& members)
    {
        for (size_t off = 0; off < members.size(); off += kMaxGroupClusters)
        {
            const size_t n = std::min<size_t>(kMaxGroupClusters, members.size() - off);
            Gr gr;
            gr.level = level; gr.section = sec; gr.root = true;
            gr.memOff = static_cast<u32>(gmembers.size());
            gr.memCount = static_cast<u32>(n);
            gr.err = PosInf();
            const u32 gid = static_cast<u32>(groups.size());
            for (size_t k = 0; k < n; ++k)
            {
                const u32 m = members[off + k];
                gmembers.push_back(m);
                Cl& c = st.cl[m];
                std::memcpy(c.parSphere, c.lodSphere, 16);
                c.parErr = PosInf();
                c.consumer = gid;
            }
            std::memcpy(gr.sphere, st.cl[members[off]].lodSphere, 16);
            groups.push_back(gr);
        }
    }

    VgeoError RunSection(u32 sec, u32 first, u32 count)
    {
        std::vector<u32> cur(count);
        std::iota(cur.begin(), cur.end(), first);
        u32 level = 0;
        {
            LevelStat& ls = LS(0);
            ls.clusters += cur.size();
            for (u32 c : cur) ls.tris += st.cl[c].tc;
        }
        for (;;)
        {
            if (level + 1 >= kMaxLevels) return MakeErr(Errc::LimitExceeded, "the LOD hierarchy would exceed 254 levels");
            if (cur.size() == 1) { MakeRootGroup(sec, level, cur); break; }
            const bool final = cur.size() <= 2;

            auto t0 = std::chrono::steady_clock::now();
            LevelGraph lg;
            BuildLevelGraph(st, g, cur, threads, opt.lowPriority, lg);
            stats.msGraph += MsSince(t0);

            // 入力（Meshopt 方式用）
            std::vector<f32> centers(cur.size() * 3);
            for (size_t i = 0; i < cur.size(); ++i) std::memcpy(&centers[i * 3], st.cl[cur[i]].cull, 12);
            std::vector<u32> mIdx, mCnt;
            if (opt.groupMethod == GroupMethod::Meshopt && !final)
            {
                mCnt.resize(cur.size());
                for (size_t i = 0; i < cur.size(); ++i)
                {
                    const Cl& c = st.cl[cur[i]];
                    mCnt[i] = c.vc;
                    for (u32 k = 0; k < c.vc; ++k) mIdx.push_back(g.weld[st.vpool[c.vOff + k]]);
                }
            }

            std::vector<GroupOut> outs;
            std::vector<std::vector<u32>> members;
            u64 productCount = 0;
            u32 target = final ? static_cast<u32>(cur.size()) : std::max(2u, opt.groupSize);
            std::vector<u32> groupOf;
            u32 G = 0;
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                t0 = std::chrono::steady_clock::now();
                if (final) { groupOf.assign(cur.size(), 0); G = 1; }
                else
                {
                    GroupingInput gi;
                    gi.n = static_cast<u32>(cur.size());
                    gi.centers = centers.data();
                    gi.graph = &lg.g;
                    gi.mIndices = mIdx.data(); gi.mCounts = mCnt.data(); gi.mTotal = mIdx.size(); gi.mPositions = g.dpos.data(); gi.mVertexCount = g.V();
                    G = PartitionClusters(opt.groupMethod, target, gi, groupOf);
                }
                stats.msGroup += MsSince(t0);
                members.assign(G, {});
                for (u32 i = 0; i < cur.size(); ++i) members[groupOf[i]].push_back(cur[i]);
                outs.clear();
                outs.resize(G);
                DagCtx cx{g, st, opt, &lg.border};
                t0 = std::chrono::steady_clock::now();
                ParallelFor(G, threads, 4, opt.lowPriority, [&](size_t b, size_t e, u32 tid)
                {
                    for (size_t gi2 = b; gi2 < e; ++gi2) SimplifyGroup(cx, members[gi2].data(), static_cast<u32>(members[gi2].size()), final, scratch[tid], outs[gi2]);
                });
                stats.msSimplify += MsSince(t0);
                productCount = 0;
                for (const GroupOut& o : outs) productCount += o.products.cl.size();
                if (opt.verbose)
                {
                    u64 ti = 0, to = 0, lk = 0, vv = 0;
                    for (const GroupOut& o : outs) { ti += o.trisIn; to += o.trisOut; lk += o.lockedVerts; vv += o.verts; }
                    std::fprintf(stderr, "[cook] sec %u level %u attempt %d: %zu clusters -> %u groups (target %u) -> %llu clusters | tris %llu -> %llu | locked %.1f%%\n", sec, level, attempt, cur.size(), G, target,
                                 static_cast<unsigned long long>(productCount), static_cast<unsigned long long>(ti), static_cast<unsigned long long>(to), vv ? 100.0 * static_cast<double>(lk) / static_cast<double>(vv) : 0.0);
                }
                if (final || productCount < cur.size()) break;
                target = std::min(kMaxGroupClusters, target + 2);   // 進まない: グループを大きくして再試行
            }
            if ((!final && productCount >= cur.size()) || (final && productCount > kMaxGroupClusters))
            {
                // これ以上簡略化できない（材質境界 / 開いた境界のロックが支配的、病的な入力など）→ 現在のレベルをそのままルートにして階層を終える。
                // ルートクラスタが 2 個を超えるが、形式上は有効（ルートグループは 15 クラスタごとに分ける）。統計の stagnatedRoots に数える。
                ++stats.stagnatedRoots;
                if (opt.verbose) std::fprintf(stderr, "[cook] sec %u level %u: cannot simplify further (%zu clusters); using this level as the roots\n", sec, level, cur.size());
                MakeRootGroup(sec, level, cur);
                break;
            }

            // 確定: グループ / 親子リンク / 誤差
            LevelStat& ls = LS(level);
            ls.groups += G;
            ls.groupClusters += cur.size();
            if (!final) ls.cutEdges += CutWeight(lg.g, groupOf);
            ls.sharedEdges += lg.shared;
            std::vector<u32> next;
            const u32 gBase = static_cast<u32>(groups.size());
            for (u32 gi2 = 0; gi2 < G; ++gi2)
            {
                GroupOut& o = outs[gi2];
                Gr gr;
                gr.level = level; gr.section = sec; gr.root = false;
                gr.memOff = static_cast<u32>(gmembers.size());
                gr.memCount = static_cast<u32>(members[gi2].size());
                std::memcpy(gr.sphere, o.sphere, 16);
                gr.err = o.err;
                const u32 gid = gBase + gi2;
                for (u32 m : members[gi2])
                {
                    gmembers.push_back(m);
                    Cl& c = st.cl[m];
                    std::memcpy(c.parSphere, o.sphere, 16);
                    c.parErr = o.err;
                    c.consumer = gid;
                }
                groups.push_back(gr);
                ls.trisIn += o.trisIn; ls.trisOut += o.trisOut; ls.lockedVerts += o.lockedVerts; ls.groupVerts += o.verts;
                ls.meanSimplifyErr += o.simErr; ls.maxSimplifyErr = std::max<f64>(ls.maxSimplifyErr, o.simErr);
                if (o.reached) ++ls.reachedTarget;
                ls.permissiveGroups += o.permissiveUsed;
                const f32 sphere[4] = {o.sphere[0], o.sphere[1], o.sphere[2], o.sphere[3]};
                const f32 err = o.err;
                const u32 base = st.Append(std::move(o.products));
                const u32 endc = static_cast<u32>(st.cl.size());
                for (u32 c = base; c < endc; ++c)
                {
                    Cl& pc = st.cl[c];
                    std::memcpy(pc.lodSphere, sphere, 16);
                    pc.lodErr = err;
                    pc.producer = gid;
                    pc.level = level + 1;
                    pc.section = sec;
                    next.push_back(c);
                }
            }
            {
                LevelStat& nl = LS(level + 1);
                nl.clusters += next.size();
                for (u32 c : next) nl.tris += st.cl[c].tc;
            }
            if (final) { MakeRootGroup(sec, level + 1, next); break; }
            cur = std::move(next);
            ++level;
        }
        return VgeoError{};
    }
};

// ════════════════════════════════════════════════════════════════════════════
// 5. マテリアル / プロキシ
// ════════════════════════════════════════════════════════════════════════════
struct OutMats
{
    const VgsrcData* in = nullptr;
    std::vector<MaterialRecord> recs;
    StringPool pool;
    std::map<std::pair<u32, u32>, u32> index;

    u32 Get(u32 inMat, u32 kind, const std::string& fallbackName)
    {
        const auto key = std::make_pair(inMat, kind);
        const auto it = index.find(key);
        if (it != index.end()) return it->second;
        MaterialRecord m = in->materials[inMat];
        auto remap = [&](u32 off) -> u32
        {
            if (off == kNone || off >= in->strings.size()) return kNone;
            return pool.Add(std::string_view(in->strings.data() + off));
        };
        m.nameOff = (m.nameOff == kNone) ? pool.Add(fallbackName) : remap(m.nameOff);
        m.albedoPathOff = remap(m.albedoPathOff);
        m.normalPathOff = remap(m.normalPathOff);
        m.metalRoughPathOff = remap(m.metalRoughPathOff);
        m.emissivePathOff = remap(m.emissivePathOff);
        m.sectionKind = kind;
        m.reserved = 0;
        if (kind == 0) m.flags &= ~(kMatAlphaTest | kMatBlend);
        const u32 idx = static_cast<u32>(recs.size());
        recs.push_back(m);
        index.emplace(key, idx);
        return idx;
    }
};

// 頂点配列 + 三角形 → ProxySectionData（復元位置・oct 法線・UV で作る。Global 番号を密な番号へ詰める）
ProxySectionData MakeProxyFromIndices(const Global& g, const std::vector<u32>& gidx, std::vector<u32>& remapScratch, u32 materialIndex, f32 error, bool exact)
{
    ProxySectionData ps;
    ps.materialIndex = materialIndex;
    ps.error = error;
    ps.flags = exact ? kProxyFlagExact : 0u;
    f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    std::vector<u32> touched;
    ps.indices.reserve(gidx.size());
    for (u32 vi : gidx)
    {
        if (remapScratch[vi] == kNone)
        {
            remapScratch[vi] = static_cast<u32>(ps.vertices.size());
            touched.push_back(vi);
            ProxyVertex pv{};
            std::memcpy(pv.position, &g.dpos[static_cast<size_t>(vi) * 3], 12);
            DecodeOct16(g.v[vi].nrm, pv.normal);
            pv.color[0] = pv.color[1] = pv.color[2] = pv.color[3] = 1.0f;
            pv.texCoord[0] = g.v[vi].uv[0]; pv.texCoord[1] = g.v[vi].uv[1];
            pv.tangent[3] = 1.0f;
            for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], pv.position[a]); hi[a] = std::max(hi[a], pv.position[a]); }
            ps.vertices.push_back(pv);
        }
        ps.indices.push_back(remapScratch[vi]);
    }
    for (u32 vi : touched) remapScratch[vi] = kNone;
    std::memcpy(ps.aabbMin, lo, 12);
    std::memcpy(ps.aabbMax, hi, 12);
    return ps;
}

// LOD 曲線: 距離ごとの LOD 選択（CPU 参照: draw = parentCoarse && fine）で描く三角形数。
std::vector<LodCurvePoint> ComputeLodCurve(const Store& st, const f32 bsphere[4], u64 sourceTris)
{
    std::vector<LodCurvePoint> out;
    const f64 projScale = 0.5 * 1080.0 / std::tan(0.5 * (60.0 * 3.14159265358979323846 / 180.0));
    const f64 tau = 1.0;
    static const f64 kDist[] = {1.5, 2.0, 3.0, 5.0, 8.0, 12.0, 20.0, 40.0, 80.0, 160.0};
    const f64 R = std::max<f64>(bsphere[3], 1e-6);
    for (f64 k : kDist)
    {
        const f64 camx = bsphere[0] + k * R, camy = bsphere[1], camz = bsphere[2];
        auto proj = [&](f32 err, const f32* s) -> f64
        {
            if (IsPosInf(err)) return 1e300;
            const f64 dx = camx - s[0], dy = camy - s[1], dz = camz - s[2];
            const f64 d = std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - static_cast<f64>(s[3]), 0.1);   // zNear 0.1 m
            return static_cast<f64>(err) * projScale / d;
        };
        u64 drawn = 0;
        for (const Cl& c : st.cl)
        {
            const bool fine = proj(c.lodErr, c.lodSphere) <= tau;
            const bool parentCoarse = proj(c.parErr, c.parSphere) > tau;
            if (fine && parentCoarse) drawn += c.tc;
        }
        LodCurvePoint p;
        p.distanceOverRadius = k;
        p.drawnTris = drawn;
        p.reduction = sourceTris ? 1.0 - static_cast<f64>(drawn) / static_cast<f64>(sourceTris) : 0.0;
        out.push_back(p);
    }
    return out;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// Cook
// ════════════════════════════════════════════════════════════════════════════
static VgeoError CookImpl(VgsrcData&& in, const CookOptions& opt, VgeoContent& out, CookStats* statsOut)
{
    const auto tStart = std::chrono::steady_clock::now();
    CookStats stats;
    if (opt.maxVerts < 3 || opt.maxVerts > kMaxClusterVerts || opt.maxTris < 4 || opt.maxTris > kMaxClusterTris || (opt.maxTris % 4) != 0)
        return MakeErr(Errc::LimitExceeded, "maxVerts must be in [3,128] and maxTris a multiple of 4 in [4,128]");
    if (opt.groupSize < 2 || opt.groupSize > 8) return MakeErr(Errc::LimitExceeded, "groupSize must be in [2,8]");
    if (!(opt.reduction > 0.05f && opt.reduction < 0.95f)) return MakeErr(Errc::LimitExceeded, "reduction must be in (0.05, 0.95)");
    if (!(opt.targetFill > 0.3f && opt.targetFill <= 1.0f)) return MakeErr(Errc::LimitExceeded, "targetFill must be in (0.3, 1.0]");
    const u32 threads = std::max(1u, opt.threads ? opt.threads : DefaultThreadCount());
    stats.threadsUsed = threads;
    out = VgeoContent{};

    // 入力ハッシュ（VGSRC が持っていなければ自前で取る。入力を解放する前に）
    u64 sourceHash = in.sourceHash;
    if (sourceHash == 0)
    {
        u64 h = 0xcbf29ce484222325ull;
        h = Fnv1a64(in.positions.data(), in.positions.size() * 4, h);
        h = Fnv1a64(in.normals.data(), in.normals.size() * 4, h);
        h = Fnv1a64(in.uv0.data(), in.uv0.size() * 4, h);
        h = Fnv1a64(in.indices.data(), in.indices.size() * 4, h);
        sourceHash = h;
    }

    Global g;
    std::vector<SecTris> secs;
    auto t0 = std::chrono::steady_clock::now();
    if (VgeoError e = Ingest(in, opt, threads, g, secs, stats); !e.ok()) return e;
    stats.msIngest = MsSince(t0);
    stats.peakWorkingSet = std::max(stats.peakWorkingSet, ProcessWorkingSetBytes());

    // セクションの種別（VG / NONVG）と出力材質
    OutMats mats;
    mats.in = &in;
    std::vector<u32> secMat(secs.size(), 0);
    for (size_t i = 0; i < secs.size(); ++i)
    {
        const VgsrcSection& s = in.sections[secs[i].inSection];
        const MaterialRecord& m = in.materials[s.materialIndex];
        secs[i].nonvg = (s.flags & kVgsrcSectionForceNonVg) != 0 || (m.flags & (kMatAlphaTest | kMatBlend)) != 0;
        secMat[i] = mats.Get(s.materialIndex, secs[i].nonvg ? 1u : 0u, opt.label);
    }

    // テクスチャパスは相対のみ（'..' / 絶対 / '\' 禁止。仕様 C24）。検査を通らないファイルを黙って出さない。
    for (size_t i = 0; i < mats.recs.size(); ++i)
    {
        const MaterialRecord& m = mats.recs[i];
        const u32 offs[4] = {m.albedoPathOff, m.normalPathOff, m.metalRoughPathOff, m.emissivePathOff};
        for (u32 o : offs)
            if (o != kNone && o < mats.pool.Data().size() && !detail::IsSafeRelativePath(std::string_view(mats.pool.Data().data() + o)))
                return MakeErr(Errc::MaterialInvalid, Fmt("input material %zu has a texture path that is not a safe relative path: \"%s\"", i, mats.pool.Data().data() + o), static_cast<i64>(i));
    }
    // 複数セクションで共有される頂点 = 材質境界 → 全 LOD で永久ロック
    if (secs.size() >= 2)
    {
        std::vector<u32> owner(g.V(), kNone);
        g.permLock.assign(g.V(), 0);
        for (size_t si = 0; si < secs.size(); ++si)
            for (u32 vi : secs[si].idx)
            {
                const u32 w = g.weld[vi];
                if (owner[w] == kNone) owner[w] = static_cast<u32>(si);
                else if (owner[w] != si) g.permLock[w] = 1;
            }
    }

    // LOD0 + DAG（VG セクションごと）
    Store st;
    std::vector<Gr> groups;
    std::vector<u32> gmembers;
    std::vector<Scratch> scratch(threads);
    std::vector<LevelStat> levels;
    DagBuilder dag{g, opt, threads, st, groups, gmembers, scratch, levels, stats};
    std::vector<u32> secFirst(secs.size(), 0), secCount(secs.size(), 0);
    u64 vgTris = 0, nonVgTris = 0;
    const auto tDag = std::chrono::steady_clock::now();
    for (size_t si = 0; si < secs.size(); ++si)
    {
        if (secs[si].nonvg) { nonVgTris += secs[si].idx.size() / 3; continue; }
        vgTris += secs[si].idx.size() / 3;
        t0 = std::chrono::steady_clock::now();
        secFirst[si] = BuildLod0(g, secs[si].idx, opt, threads, scratch, static_cast<u32>(si), st, &secCount[si]);
        stats.msLod0 += MsSince(t0);
        stats.peakWorkingSet = std::max(stats.peakWorkingSet, ProcessWorkingSetBytes());
        if (VgeoError e = dag.RunSection(static_cast<u32>(si), secFirst[si], secCount[si]); !e.ok()) return e;
        stats.peakWorkingSet = std::max(stats.peakWorkingSet, ProcessWorkingSetBytes());
        secs[si].idx = std::vector<u32>{};   // LOD0 の三角形リストはクラスタが持っている（プロキシは LOD0 が小さいときクラスタから作る）
    }
    stats.msDag = MsSince(tDag);
    stats.vgTris = vgTris;
    stats.nonVgTris = nonVgTris;
    if (st.cl.empty() && nonVgTris == 0) return MakeErr(Errc::CountMismatch, "no clusters were produced");
    scratch = std::vector<Scratch>{};

    // ── ページ詰め ────────────────────────────────────────────────────────────
    t0 = std::chrono::steady_clock::now();
    const u32 C = static_cast<u32>(st.cl.size());
    const u32 GN = static_cast<u32>(groups.size());
    // クラスタごとのバイト数（ページに入る大きさ）
    std::vector<u32> clBytes(C);
    ParallelFor(C, threads, 1024, opt.lowPriority, [&](size_t b, size_t e, u32)
    {
        for (size_t ci = b; ci < e; ++ci)
        {
            const Cl& c = st.cl[ci];
            u32 lo[3] = {kGridMax, kGridMax, kGridMax}, hi[3] = {0, 0, 0};
            for (u32 k = 0; k < c.vc; ++k)
            {
                const Vtx& v = g.v[st.vpool[c.vOff + k]];
                for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], v.q[a]); hi[a] = std::max(hi[a], v.q[a]); }
            }
            u32 bpp = 0;
            for (int a = 0; a < 3; ++a) bpp += static_cast<u32>(std::bit_width(hi[a] - lo[a]));
            clBytes[ci] = VertexBlockBytes(c.vc, bpp) + TriBlockBytes(c.tc);
        }
    });
    // グループの並び: ルートグループ → レベル降順 → Morton（グループ中心）
    struct OrdKey { u32 rootFirst; u32 levelDesc; u64 morton; u32 gid; };
    std::vector<OrdKey> ok(GN);
    {
        f64 lo[3], inv[3];
        for (int a = 0; a < 3; ++a)
        {
            lo[a] = g.aabbMin[a];
            const f64 ext = static_cast<f64>(g.aabbMax[a]) - static_cast<f64>(g.aabbMin[a]);
            inv[a] = ext > 0 ? 1.0 / ext : 0.0;
        }
        std::vector<f32> sph;
        for (u32 gi = 0; gi < GN; ++gi)
        {
            const Gr& gr = groups[gi];
            sph.clear();
            for (u32 k = 0; k < gr.memCount; ++k) sph.insert(sph.end(), st.cl[gmembers[gr.memOff + k]].cull, st.cl[gmembers[gr.memOff + k]].cull + 4);
            f32 es[4];
            EnclosingSphere(sph.data(), gr.memCount, es);
            ok[gi] = OrdKey{gr.root ? 0u : 1u, 255u - gr.level, MortonOfPoint(es, lo, inv), gi};
        }
    }
    std::sort(ok.begin(), ok.end(), [](const OrdKey& a, const OrdKey& b)
    {
        if (a.rootFirst != b.rootFirst) return a.rootFirst < b.rootFirst;
        if (a.levelDesc != b.levelDesc) return a.levelDesc < b.levelDesc;
        if (a.morton != b.morton) return a.morton < b.morton;
        return a.gid < b.gid;
    });
    // パス 1: ページの割り当て（PageBuilder::TryAddGroup と同じ入る/入らない判定）
    std::vector<u32> gPage(GN, 0), gFirst(GN, 0);
    std::vector<u32> pageGroupStart;   // ページ p のグループは ok[pageGroupStart[p] .. pageGroupStart[p+1])
    {
        u32 page = 0, count = 0;
        u64 arena = 0;
        pageGroupStart.push_back(0);
        for (u32 k = 0; k < GN; ++k)
        {
            const Gr& gr = groups[ok[k].gid];
            u64 add = 0;
            for (u32 m = 0; m < gr.memCount; ++m) add += clBytes[gmembers[gr.memOff + m]];
            auto fits = [&]() -> bool
            {
                const u64 nc = static_cast<u64>(count) + gr.memCount;
                return nc <= kMaxClustersPerPage && kPageHeaderSize + nc * kClusterHeaderSize + arena + add <= kPageSize;
            };
            if (!fits())
            {
                if (count == 0) return MakeErr(Errc::ClusterInvalid, "a cluster group does not fit in an empty page");
                ++page; count = 0; arena = 0;
                pageGroupStart.push_back(k);
                if (!fits()) return MakeErr(Errc::ClusterInvalid, "a cluster group does not fit in an empty page");
            }
            gPage[ok[k].gid] = page; gFirst[ok[k].gid] = count;
            count += gr.memCount; arena += add;
        }
        pageGroupStart.push_back(GN);
        if (GN == 0) pageGroupStart.assign(1, 0);   // VG クラスタが 0 個（全部 NONVG）: ページは 0 枚
    }
    const u32 pageCount = static_cast<u32>(pageGroupStart.size()) - 1;
    if (pageCount > kMaxPages) return MakeErr(Errc::LimitExceeded, "the asset needs more than 65535 pages");
    // pinned: ルートを含むページは全部 + 先頭から pinPages まで
    u32 pinned = 0;
    for (u32 k = 0; k < GN; ++k) if (groups[ok[k].gid].root) pinned = std::max(pinned, gPage[ok[k].gid] + 1);
    pinned = std::min(pageCount, std::max(pinned, std::min(pageCount, opt.pinPages)));

    // パス 2: 符号化（ページごとに並列）
    out.pages.assign(static_cast<size_t>(pageCount) * kPageSize, 0);
    std::atomic<int> packFail{0};
    ParallelFor(pageCount, threads, 1, opt.lowPriority, [&](size_t b, size_t e, u32)
    {
        std::vector<u8> tmp;
        std::vector<ClusterSource> cs;
        for (size_t p = b; p < e; ++p)
        {
            PageBuilder pb(static_cast<u32>(p));
            for (u32 k = pageGroupStart[p]; k < pageGroupStart[p + 1]; ++k)
            {
                const Gr& gr = groups[ok[k].gid];
                cs.clear();
                cs.resize(gr.memCount);
                for (u32 m = 0; m < gr.memCount; ++m)
                {
                    const Cl& c = st.cl[gmembers[gr.memOff + m]];
                    ClusterSource& s = cs[m];
                    std::memcpy(s.lodSphere, c.lodSphere, 16);
                    std::memcpy(s.parentLodSphere, c.parSphere, 16);
                    std::memcpy(s.cullSphere, c.cull, 16);
                    s.lodError = c.lodErr; s.parentLodError = c.parErr;
                    s.coneS8 = c.cone; s.maxEdgeLength = c.maxEdge;
                    s.level = c.level; s.root = IsPosInf(c.parErr);
                    if (c.producer == kNone) { s.childPage = kNone; s.childGroup = kNone; }
                    else
                    {
                        const Gr& pg = groups[c.producer];
                        s.childPage = gPage[c.producer];
                        s.childGroup = MakeGroupPacked(gPage[c.producer], gFirst[c.producer], pg.memCount);
                    }
                    s.materialIndex = secMat[c.section];
                    s.q.resize(static_cast<size_t>(c.vc) * 3);
                    s.normalOct.resize(c.vc);
                    s.uv.resize(static_cast<size_t>(c.vc) * 2);
                    for (u32 v = 0; v < c.vc; ++v)
                    {
                        const Vtx& x = g.v[st.vpool[c.vOff + v]];
                        s.q[v * 3] = x.q[0]; s.q[v * 3 + 1] = x.q[1]; s.q[v * 3 + 2] = x.q[2];
                        s.normalOct[v] = x.nrm;
                        s.uv[v * 2] = x.uv[0]; s.uv[v * 2 + 1] = x.uv[1];
                    }
                    s.tri.assign(st.tpool.begin() + c.tOff, st.tpool.begin() + c.tOff + static_cast<size_t>(c.tc) * 3);
                }
                u32 first = 0;
                if (pb.TryAddGroup(cs.data(), gr.memCount, &first) != PageBuilder::AddResult::Added || first != gFirst[ok[k].gid]) { packFail.store(1); return; }
            }
            tmp.clear();
            pb.FinishAppend(tmp, p < pinned);
            std::memcpy(out.pages.data() + p * kPageSize, tmp.data(), kPageSize);
        }
    });
    if (packFail.load()) return MakeErr(Errc::ClusterInvalid, "internal error: the page packing simulation disagrees with PageBuilder");
    stats.msPack = MsSince(t0);
    stats.peakWorkingSet = std::max(stats.peakWorkingSet, ProcessWorkingSetBytes());

    // ── BVH ─────────────────────────────────────────────────────────────────
    t0 = std::chrono::steady_clock::now();
    std::vector<BvhLeaf> leaves(GN);
    {
        std::vector<f32> sphC, sphL;
        for (u32 k = 0; k < GN; ++k)
        {
            const u32 gid = ok[k].gid;
            const Gr& gr = groups[gid];
            sphC.clear(); sphL.clear();
            f32 minOwn = std::numeric_limits<f32>::infinity(), maxPar = 0.0f;
            for (u32 m = 0; m < gr.memCount; ++m)
            {
                const Cl& c = st.cl[gmembers[gr.memOff + m]];
                sphC.insert(sphC.end(), c.cull, c.cull + 4);
                sphL.insert(sphL.end(), c.parSphere, c.parSphere + 4);
                minOwn = std::min(minOwn, c.lodErr);
                maxPar = std::max(maxPar, c.parErr);
            }
            BvhLeaf& lf = leaves[k];
            EnclosingSphere(sphC.data(), gr.memCount, lf.cullSphere);
            EnclosingSphere(sphL.data(), gr.memCount, lf.lodSphere);
            lf.minOwnError = minOwn; lf.maxParentError = maxPar;
            lf.groupId = k;
            lf.groupPacked = MakeGroupPacked(gPage[gid], gFirst[gid], gr.memCount);
        }
    }
    if (!leaves.empty()) out.nodes = BuildBvh4(leaves);
    stats.msBvh = MsSince(t0);

    // ── プロキシ / NONVG ────────────────────────────────────────────────────
    t0 = std::chrono::steady_clock::now();
    std::vector<u32> remapScratch;
    if (opt.proxy || nonVgTris > 0) remapScratch.assign(g.V(), kNone);
    if (opt.proxy && !st.cl.empty())
    {
        // セクションごとの最大レベル（ルートのレベル）とレベルごとの三角形数
        std::vector<u32> rootLevel(secs.size(), 0);
        std::vector<std::vector<u64>> lvTris(secs.size());
        for (const Cl& c : st.cl)
        {
            rootLevel[c.section] = std::max(rootLevel[c.section], c.level);
            if (lvTris[c.section].size() <= c.level) lvTris[c.section].resize(c.level + 1, 0);
            lvTris[c.section][c.level] += c.tc;
        }
        u32 maxL = 0;
        for (u32 r : rootLevel) maxL = std::max(maxL, r);
        u32 chosen = maxL;
        for (u32 lvl = 0; lvl <= maxL; ++lvl)
        {
            u64 total = 0;
            for (size_t si = 0; si < secs.size(); ++si)
                if (!secs[si].nonvg && !lvTris[si].empty()) total += lvTris[si][std::min<u32>(lvl, rootLevel[si])];
            if (total <= opt.proxyMaxTris) { chosen = lvl; break; }
        }
        for (size_t si = 0; si < secs.size(); ++si)
        {
            if (secs[si].nonvg || lvTris[si].empty()) continue;
            const u32 L = std::min<u32>(chosen, rootLevel[si]);
            std::vector<u32> gidx;
            f32 err = 0.0f;
            for (const Cl& c : st.cl)
            {
                if (c.section != si || c.level != L) continue;
                err = std::max(err, c.lodErr);
                for (u32 k = 0; k < c.tc * 3; ++k) gidx.push_back(st.vpool[c.vOff + st.tpool[c.tOff + k]]);
            }
            if (gidx.size() < 3) continue;
            ProxySectionData ps = MakeProxyFromIndices(g, gidx, remapScratch, secMat[si], err, L == 0 && err == 0.0f);
            if (ps.vertices.size() >= 3) out.proxy.push_back(std::move(ps));
        }
    }
    for (size_t si = 0; si < secs.size(); ++si)
    {
        if (!secs[si].nonvg) continue;
        // NONVG: 予算内へ簡略化した実描画用（MASK / BLEND）。secs[si].idx は解放していない。
        const std::vector<u32>& idx = secs[si].idx;
        std::vector<u32> gidx;
        f32 err = 0.0f;
        bool exact = true;
        if (idx.size() / 3 <= opt.nonVgMaxTris) gidx = idx;
        else
        {
            std::vector<u32> lv;
            std::vector<u32> lidx(idx.size());
            LocalMap map;
            map.Reset(idx.size());
            for (size_t i = 0; i < idx.size(); ++i)
            {
                bool added = false;
                const u32 l = map.GetOrAdd(idx[i], static_cast<u32>(lv.size()), added);
                if (added) lv.push_back(idx[i]);
                lidx[i] = l;
            }
            const u32 nv = static_cast<u32>(lv.size());
            std::vector<f32> lpos(static_cast<size_t>(nv) * 3), lattr(static_cast<size_t>(nv) * 5);
            std::vector<u8> lock(nv, 0);
            for (u32 v = 0; v < nv; ++v)
            {
                std::memcpy(&lpos[static_cast<size_t>(v) * 3], &g.dpos[static_cast<size_t>(lv[v]) * 3], 12);
                DecodeOct16(g.v[lv[v]].nrm, &lattr[static_cast<size_t>(v) * 5]);
                lattr[static_cast<size_t>(v) * 5 + 3] = g.v[lv[v]].uv[0]; lattr[static_cast<size_t>(v) * 5 + 4] = g.v[lv[v]].uv[1];
                if (!g.permLock.empty() && g.permLock[g.weld[lv[v]]]) lock[v] = static_cast<u8>(meshopt_SimplifyVertex_Lock);
            }
            const f32 weights[5] = {opt.normalWeight, opt.normalWeight, opt.normalWeight, opt.uvWeight, opt.uvWeight};
            std::vector<u32> res(lidx.size());
            const size_t n = meshopt_simplifyWithAttributes(res.data(), lidx.data(), lidx.size(), lpos.data(), nv, 12, lattr.data(), 20, weights, 5, lock.data(),
                                                            static_cast<size_t>(opt.nonVgMaxTris) * 3, FLT_MAX, meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute, &err);
            res.resize(n);
            for (u32 i : res) gidx.push_back(lv[i]);
            exact = false;
        }
        if (gidx.size() < 3) continue;
        ProxySectionData ps = MakeProxyFromIndices(g, gidx, remapScratch, secMat[si], err, exact && err == 0.0f);
        if (ps.vertices.size() >= 3) out.nonvg.push_back(std::move(ps));
    }
    stats.msProxy = MsSince(t0);

    // ── ヘッダ / 材質 / debugJson ───────────────────────────────────────────
    out.materials = mats.recs;
    out.strings = mats.pool.Data();
    VgeoHeader& h = out.header;
    h.maxClusterVerts = opt.maxVerts;
    h.maxClusterTris = opt.maxTris;
    h.sourceTriangleCount = vgTris;
    h.sourceVertexCount = g.V();
    for (int a = 0; a < 3; ++a) { h.aabbMin[a] = g.aabbMin[a]; h.aabbMax[a] = g.aabbMax[a]; h.posOrigin[a] = g.origin[a]; }
    h.posStep = g.step;
    {
        std::vector<f32> sph;
        if (!out.nodes.empty())
            for (const HierChild& c : out.nodes[0].child) if (c.ref != kNone) sph.insert(sph.end(), c.cullSphere, c.cullSphere + 4);
        for (const auto* list : {&out.nonvg})
            for (const ProxySectionData& ps : *list)
            {
                f32 c[4] = {(ps.aabbMin[0] + ps.aabbMax[0]) * 0.5f, (ps.aabbMin[1] + ps.aabbMax[1]) * 0.5f, (ps.aabbMin[2] + ps.aabbMax[2]) * 0.5f, 0.0f};
                f64 r = 0;
                for (const ProxyVertex& v : ps.vertices)
                {
                    const f64 dx = v.position[0] - c[0], dy = v.position[1] - c[1], dz = v.position[2] - c[2];
                    r = std::max(r, std::sqrt(dx * dx + dy * dy + dz * dz));
                }
                c[3] = std::nextafter(static_cast<f32>(r), std::numeric_limits<f32>::infinity());
                sph.insert(sph.end(), c, c + 4);
            }
        if (sph.empty()) { const f32 c[4] = {(g.aabbMin[0] + g.aabbMax[0]) * 0.5f, (g.aabbMin[1] + g.aabbMax[1]) * 0.5f, (g.aabbMin[2] + g.aabbMax[2]) * 0.5f, 1.0f}; sph.insert(sph.end(), c, c + 4); }
        EnclosingSphere(sph.data(), static_cast<u32>(sph.size() / 4), h.boundingSphere);
    }
    h.pinnedPageCount = pageCount ? pinned : 0;
    h.sourceHash = sourceHash;
    const std::string params = opt.CanonicalString();
    h.cookParamsHash = Fnv1a32(params.data(), params.size());
    std::snprintf(h.cooker, sizeof h.cooker, "vgeo_cook %s meshopt %u.%u", kCookerVersion, static_cast<u32>(MESHOPTIMIZER_VERSION / 1000), static_cast<u32>((MESHOPTIMIZER_VERSION / 10) % 100));

    // ── 統計 ─────────────────────────────────────────────────────────────────
    {
        u64 lod0c = 0, lod0Tris = 0, allTris = 0, allVerts = 0, rootC = 0, coneValid = 0;
        f64 coneCutSum = 0;
        f64 bppSum = 0;
        for (u32 ci = 0; ci < C; ++ci)
        {
            const Cl& c = st.cl[ci];
            allTris += c.tc; allVerts += c.vc;
            if (c.level == 0)
            {
                ++lod0c; lod0Tris += c.tc;
                const i8 cut = ConeByte(c.cone, 3);
                if (cut < 127) { ++coneValid; coneCutSum += static_cast<f64>(cut) / 127.0; }
            }
            if (IsPosInf(c.parErr)) ++rootC;
            u32 lo[3] = {kGridMax, kGridMax, kGridMax}, hi[3] = {0, 0, 0};
            for (u32 k = 0; k < c.vc; ++k)
                for (int a = 0; a < 3; ++a) { const u32 q = g.v[st.vpool[c.vOff + k]].q[a]; lo[a] = std::min(lo[a], q); hi[a] = std::max(hi[a], q); }
            u32 bpp = 0;
            for (int a = 0; a < 3; ++a) bpp += static_cast<u32>(std::bit_width(hi[a] - lo[a]));
            bppSum += static_cast<f64>(bpp) * c.vc;
        }
        stats.clusters = C; stats.groups = GN; stats.pages = pageCount; stats.nodes = out.nodes.size(); stats.rootClusters = rootC; stats.lod0Clusters = lod0c;
        stats.levels = static_cast<u32>(levels.size());
        stats.pinnedPages = h.pinnedPageCount;
        stats.pageBytes = static_cast<u64>(pageCount) * kPageSize;
        stats.avgTrisPerCluster = C ? static_cast<f64>(allTris) / C : 0.0;
        stats.avgVertsPerCluster = C ? static_cast<f64>(allVerts) / C : 0.0;
        stats.avgFillLod0 = lod0c ? static_cast<f64>(lod0Tris) / (static_cast<f64>(lod0c) * opt.maxTris) : 0.0;
        stats.avgFillAll = C ? static_cast<f64>(allTris) / (static_cast<f64>(C) * opt.maxTris) : 0.0;
        stats.avgPosBits = allVerts ? bppSum / static_cast<f64>(allVerts) : 0.0;
        stats.coneValidFrac = lod0c ? static_cast<f64>(coneValid) / static_cast<f64>(lod0c) : 0.0;
        stats.coneMeanCutoff = coneValid ? coneCutSum / static_cast<f64>(coneValid) : 0.0;
        u64 used = 0;
        for (u32 p = 0; p < pageCount; ++p) used += ReadPageHeader(out.pages.data() + static_cast<size_t>(p) * kPageSize).usedBytes;
        stats.pageFill = pageCount ? static_cast<f64>(used) / (static_cast<f64>(pageCount) * kPageSize) : 0.0;
        // レベル別の誤差
        std::vector<f64> sumErr(levels.size(), 0.0);
        std::vector<u64> nErr(levels.size(), 0);
        for (const Cl& c : st.cl)
        {
            LevelStat& ls = levels[c.level];
            ls.maxLodErr = std::max<f64>(ls.maxLodErr, c.lodErr);
            sumErr[c.level] += c.lodErr; ++nErr[c.level];
        }
        for (size_t i = 0; i < levels.size(); ++i)
        {
            levels[i].meanLodErr = nErr[i] ? sumErr[i] / static_cast<f64>(nErr[i]) : 0.0;
            if (levels[i].groups) levels[i].meanSimplifyErr /= static_cast<f64>(levels[i].groups);
        }
        stats.perLevel = levels;
        if (opt.collectStats && vgTris) stats.lodCurve = ComputeLodCurve(st, h.boundingSphere, vgTris);
    }
    // debugJson（決定的: 時刻・所要時間は入れない）
    {
        std::string j;
        char buf[512];
        std::snprintf(buf, sizeof buf, "{\"tool\":\"vgeo_cook\",\"version\":\"%s\",\"label\":\"%s\",\"params\":\"%s\",\"sourceTriangles\":%llu,\"clusters\":%u,\"groups\":%u,\"pages\":%u,\"levels\":[",
                      kCookerVersion, opt.label.c_str(), params.c_str(), static_cast<unsigned long long>(vgTris), C, GN, pageCount);
        j += buf;
        for (size_t i = 0; i < stats.perLevel.size(); ++i)
        {
            const LevelStat& ls = stats.perLevel[i];
            std::snprintf(buf, sizeof buf, "%s{\"l\":%u,\"clusters\":%llu,\"tris\":%llu,\"groups\":%llu,\"maxErr\":%.6g}", i ? "," : "", ls.level, static_cast<unsigned long long>(ls.clusters),
                          static_cast<unsigned long long>(ls.tris), static_cast<unsigned long long>(ls.groups), ls.maxLodErr);
            j += buf;
        }
        j += "]}";
        out.debugJson = j;
    }
    stats.msTotal = MsSince(tStart);
    stats.peakWorkingSet = std::max(stats.peakWorkingSet, ProcessPeakWorkingSetBytes());
    if (statsOut) *statsOut = std::move(stats);
    return VgeoError{};
}

VgeoError Cook(VgsrcData&& in, const CookOptions& opt, VgeoContent& out, CookStats* stats)
{
    try
    {
        return CookImpl(std::move(in), opt, out, stats);
    }
    catch (const std::bad_alloc&)
    {
        return MakeErr(Errc::LimitExceeded, "out of memory while cooking");
    }
    catch (const std::exception& e)
    {
        return MakeErr(Errc::LimitExceeded, std::string("internal error: ") + e.what());
    }
}

} // namespace dx12e::vg::cook
