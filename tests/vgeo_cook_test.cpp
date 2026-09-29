// 仮想ジオメトリ P1: オフライン cooker（tools/vgeo_cook）のテスト。
//
//   ・クラックなし（本テストの核）: cook した DAG から「一貫したカット」を作り（距離スイープ / ランダムな一貫カット）、三角形を
//     量子化座標の溶接 ID に直して、全ての有向エッジがちょうど 1 回・逆向きが 1 回ずつ出現する（閉じた形状では境界エッジ 0）ことを確かめる。
//     開いた形状は境界をロックする設定で「境界エッジ集合が LOD0 と一致」を確かめる。材質境界（2 セクション）も。
//     陰性対照: ロックを切った cook ではクラックが検出される / 一貫しないカットでは検出される（検査が効いていることの証拠）。
//   ・誤差の単調性 / 全 LOD のクラスタが上限内 / LOD0 のカバレッジ（入力の全三角形が LOD0 クラスタにちょうど 1 回）
//   ・決定論（スレッド数 1 / 2 / 5 で同じバイト列）/ VgeoFormat の検証（strict + deep）を通る
//   ・GPU 写像: 仕様 §14 の HLSL デコードを C++ で忠実に移植し、cook した全ページ・全クラスタを CPU 参照と突き合わせる（P2 が起点にできる）
//   ・回帰: 球 / トーラス / グリッド / 箱（UV・法線の継ぎ目）/ 非多様体 / 縮退三角形 / 孤立頂点 / 島 / 極端な座標 / NONVG / 法線・UV 無し、
//     不正入力は構造化エラー
//   ・グループ化 4 方式が有効な分割を返す / 巨大ベンチ生成が決定的で外向き / スタブ出力との互換
// meshoptimizer に依存するので統合ビルドでのみビルドされる（tests/CMakeLists.txt）。

#include "CookBench.h"
#include "CookGroup.h"
#include "CookObj.h"
#include "VgeoCook.h"
#include "VgeoStubGen.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dx12e::vg;
using namespace dx12e::vg::cook;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace
{

// ── 小道具 ────────────────────────────────────────────────────────────────────
bool EnvSet(const char* name)
{
    char* buf = nullptr;
    size_t len = 0;
    const bool set = (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr);
    std::free(buf);
    return set;
}

CookOptions TestOpts(u32 threads = 2)
{
    CookOptions o;
    o.threads = threads;
    o.collectStats = true;
    {
        char* buf = nullptr;
        size_t len = 0;
        if (_dupenv_s(&buf, &len, "VG_TEST_PERMISSIVE") == 0 && buf) o.permissive = static_cast<u32>(std::atoi(buf));
        std::free(buf);
    }
    {
        char* buf = nullptr;
        size_t len = 0;
        if (_dupenv_s(&buf, &len, "VG_TEST_NW") == 0 && buf) o.normalWeight = static_cast<f32>(std::atof(buf));
        std::free(buf); buf = nullptr;
        if (_dupenv_s(&buf, &len, "VG_TEST_UW") == 0 && buf) o.uvWeight = static_cast<f32>(std::atof(buf));
        std::free(buf); buf = nullptr;
        if (_dupenv_s(&buf, &len, "VG_TEST_REG") == 0 && buf) o.regularize = std::atoi(buf) != 0;
        std::free(buf);
    }
    return o;
}

VgsrcData Bench(const char* kind, u64 tris, u32 seed = 1, u32 materials = 1)
{
    BenchOptions bo;
    bo.kind = kind; bo.tris = tris; bo.seed = seed; bo.materials = materials; bo.threads = 2;
    VgsrcData d;
    const VgeoError e = GenerateBench(bo, d);
    if (!e.ok()) std::printf("  bench %s failed: %s\n", kind, e.ToString().c_str());
    CHECK(e.ok());
    return d;
}

std::vector<u8> WriteBytes(const VgeoContent& c)
{
    std::vector<u8> b;
    const VgeoError e = WriteVgeoToMemory(c, b);
    CHECK(e.ok());
    return b;
}

bool CookOk(VgsrcData d, const CookOptions& o, VgeoContent& c, CookStats* st = nullptr, const char* name = "")
{
    const VgeoError e = Cook(std::move(d), o, c, st);
    if (!e.ok()) { std::printf("  cook %s failed: %s\n", name, e.ToString().c_str()); return false; }
    return true;
}

void CheckValid(const VgeoContent& c, const char* name)
{
    const ValidationReport rep = ValidateContent(c);
    if (!rep.ok()) for (const auto& i : rep.issues) std::printf("  %s: %s\n", name, i.ToString().c_str());
    CHECK(rep.ok());
}

// ── 箱（面ごとに頂点を分ける = UV の継ぎ目 + 法線の継ぎ目。閉じた多様体）────────────────────
VgsrcData MakeBox(u32 n)
{
    VgsrcData d;
    d.flags = kVgsrcHasNormals | kVgsrcHasUV0;
    struct Face { f32 o[3], u[3], v[3]; };
    // 各面: 原点 + u 軸 + v 軸。法線 = cross(u, v) が外向きになる向き
    const Face faces[6] = {
        {{-1, -1, -1}, {2, 0, 0}, {0, 2, 0}},   // -Z 側だが cross(u,v) = +Z → 後で反転判定
        {{-1, -1, 1}, {0, 2, 0}, {2, 0, 0}},
        {{-1, -1, -1}, {0, 0, 2}, {2, 0, 0}},
        {{-1, 1, -1}, {2, 0, 0}, {0, 0, 2}},
        {{-1, -1, -1}, {0, 2, 0}, {0, 0, 2}},
        {{1, -1, -1}, {0, 0, 2}, {0, 2, 0}},
    };
    for (const Face& f : faces)
    {
        const u32 base = static_cast<u32>(d.positions.size() / 3);
        f32 nrm[3] = {f.u[1] * f.v[2] - f.u[2] * f.v[1], f.u[2] * f.v[0] - f.u[0] * f.v[2], f.u[0] * f.v[1] - f.u[1] * f.v[0]};
        const f32 cx = f.o[0] + 0.5f * (f.u[0] + f.v[0]), cy = f.o[1] + 0.5f * (f.u[1] + f.v[1]), cz = f.o[2] + 0.5f * (f.u[2] + f.v[2]);
        const bool flip = (nrm[0] * cx + nrm[1] * cy + nrm[2] * cz) < 0;
        const f32 l = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
        for (int k = 0; k < 3; ++k) nrm[k] = (flip ? -nrm[k] : nrm[k]) / l;
        for (u32 i = 0; i <= n; ++i)
            for (u32 j = 0; j <= n; ++j)
            {
                const f32 s = static_cast<f32>(i) / n, t = static_cast<f32>(j) / n;
                for (int k = 0; k < 3; ++k) { d.positions.push_back(f.o[k] + s * f.u[k] + t * f.v[k]); d.normals.push_back(nrm[k]); }
                d.uv0.push_back(s); d.uv0.push_back(t);
            }
        for (u32 i = 0; i < n; ++i)
            for (u32 j = 0; j < n; ++j)
            {
                const u32 a = base + i * (n + 1) + j, b = a + 1, c = a + (n + 1), e = c + 1;
                if (!flip) { d.indices.insert(d.indices.end(), {a, c, b, b, c, e}); }
                else { d.indices.insert(d.indices.end(), {a, b, c, b, e, c}); }
            }
    }
    StringPool pool;
    MaterialRecord m = MakeDefaultMaterial();
    m.nameOff = pool.Add("box");
    d.materials.push_back(m);
    d.strings = pool.Data();
    d.sections.push_back(VgsrcSection{0, 0, static_cast<u32>(d.indices.size()), 0});
    return d;
}

// ── デコード済みアセット + カット ─────────────────────────────────────────────────
struct KeyHash
{
    size_t operator()(const std::array<u32, 3>& k) const
    {
        u64 h = k[0] * 0x9E3779B97F4A7C15ull;
        h ^= (k[1] + 0x7F4A7C15u) * 0xC2B2AE3D27D4EB2Full;
        h ^= (k[2] + 0x165667B1u) * 0x85EBCA77C2B2AE63ull;
        return static_cast<size_t>(h ^ (h >> 29));
    }
};

struct Asset
{
    struct Cl { ClusterHeader h{}; std::vector<u32> wid; std::vector<u8> tri; std::vector<u32> q; u32 level = 0, consumer = kNone, producer = kNone; };
    struct Gp { u32 page = 0, first = 0, count = 0, level = 0; bool root = false; };
    std::vector<Cl> cl;
    std::vector<u32> pageBase;
    std::vector<Gp> groups;
    u32 nWeld = 0;
    std::vector<std::array<u32, 3>> weldQ;   // 溶接 ID → 格子座標
    f32 bsphere[4] = {0, 0, 0, 1};
};

Asset Decode(const VgeoContent& c)
{
    Asset a;
    const u32 pageCount = static_cast<u32>(c.pages.size() / kPageSize);
    std::unordered_map<std::array<u32, 3>, u32, KeyHash> weld;
    a.pageBase.assign(pageCount + 1, 0);
    for (u32 p = 0; p < pageCount; ++p)
    {
        const u8* page = c.pages.data() + static_cast<size_t>(p) * kPageSize;
        const PageHeader ph = ReadPageHeader(page);
        a.pageBase[p] = static_cast<u32>(a.cl.size());
        for (u32 i = 0; i < ph.clusterCount; ++i)
        {
            DecodedCluster dc;
            const bool ok = DecodeCluster(page, i, c.header.posOrigin, c.header.posStep, dc);
            CHECK(ok);
            Asset::Cl cl;
            cl.h = dc.h;
            cl.tri = dc.tri;
            cl.q = dc.q;
            cl.level = ClusterLevel(dc.h);
            for (u32 v = 0; v < dc.vertexCount; ++v)
            {
                const std::array<u32, 3> k = {dc.q[v * 3], dc.q[v * 3 + 1], dc.q[v * 3 + 2]};
                auto it = weld.find(k);
                if (it == weld.end()) { it = weld.emplace(k, static_cast<u32>(weld.size())).first; a.weldQ.push_back(k); }
                cl.wid.push_back(it->second);
            }
            a.cl.push_back(std::move(cl));
        }
    }
    a.pageBase[pageCount] = static_cast<u32>(a.cl.size());
    a.nWeld = static_cast<u32>(weld.size());
    std::memcpy(a.bsphere, c.header.boundingSphere, 16);
    // グループ（BVH の葉）
    u32 leaves = 0;
    for (const HierNode& n : c.nodes) for (const HierChild& ch : n.child) if (NodeRefIsLeaf(ch.ref)) ++leaves;
    a.groups.assign(leaves, Asset::Gp{});
    std::unordered_map<u32, u32> keyToGroup;
    for (const HierNode& n : c.nodes)
        for (const HierChild& ch : n.child)
            if (NodeRefIsLeaf(ch.ref))
            {
                const u32 gid = ch.ref & 0x7FFFFFFFu;
                Asset::Gp g;
                g.page = GroupPage(ch.groupPacked); g.first = GroupFirst(ch.groupPacked); g.count = GroupCount(ch.groupPacked);
                const Asset::Cl& m0 = a.cl[a.pageBase[g.page] + g.first];
                g.level = m0.level;
                g.root = IsPosInf(m0.h.parentLodError);
                a.groups[gid] = g;
                keyToGroup[(g.page << 8) | g.first] = gid;
                for (u32 k = 0; k < g.count; ++k) a.cl[a.pageBase[g.page] + g.first + k].consumer = gid;
            }
    for (Asset::Cl& cl : a.cl)
        if (cl.h.childGroup != kNone) cl.producer = keyToGroup[(GroupPage(cl.h.childGroup) << 8) | GroupFirst(cl.h.childGroup)];
    return a;
}

// LOD0 のカット
std::vector<u8> CutLod0(const Asset& a)
{
    std::vector<u8> d(a.cl.size(), 0);
    for (size_t i = 0; i < a.cl.size(); ++i) d[i] = a.cl[i].level == 0 ? 1 : 0;
    return d;
}

// 実行時と同じ規則（描く = 親が粗すぎ && 自分が十分細かい）。カメラは境界球中心から dir 方向に dist 離れる。
std::vector<u8> CutDistance(const Asset& a, f64 distOverRadius, const f64 dir[3], f64 tauPx, f64 projScale = 1000.0)
{
    const f64 R = std::max<f64>(a.bsphere[3], 1e-6);
    const f64 dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    const f64 cam[3] = {a.bsphere[0] + dir[0] / dl * distOverRadius * R, a.bsphere[1] + dir[1] / dl * distOverRadius * R, a.bsphere[2] + dir[2] / dl * distOverRadius * R};
    auto proj = [&](f32 err, const f32* s) -> f64
    {
        if (IsPosInf(err)) return 1e300;
        const f64 dx = cam[0] - s[0], dy = cam[1] - s[1], dz = cam[2] - s[2];
        const f64 d = std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - static_cast<f64>(s[3]), 1e-4);
        return static_cast<f64>(err) * projScale / d;
    };
    std::vector<u8> d(a.cl.size(), 0);
    for (size_t i = 0; i < a.cl.size(); ++i)
    {
        const ClusterHeader& h = a.cl[i].h;
        d[i] = (proj(h.lodError, h.lodSphere) <= tauPx && proj(h.parentLodError, h.parentLodSphere) > tauPx) ? 1 : 0;
    }
    return d;
}

struct Rng
{
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    u32 Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<u32>(s >> 16); }
    f64 Unit() { return static_cast<f64>(Next() & 0xFFFFFF) / 16777216.0; }
};

// ランダムな一貫カット: グループを細かい側から順に「畳む（親を描く）」か決める。畳めるのは、メンバー全員の生産グループが畳まれているときだけ。
// 描くクラスタ = (LOD0 または 生産グループが畳まれている) && 自分を消費するグループが畳まれていない。ルートグループは畳めない。
std::vector<u8> CutRandom(const Asset& a, u64 seed, f64 prob)
{
    Rng rng(seed);
    const u32 G = static_cast<u32>(a.groups.size());
    std::vector<u32> order(G);
    for (u32 i = 0; i < G; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](u32 x, u32 y) { return a.groups[x].level != a.groups[y].level ? a.groups[x].level < a.groups[y].level : x < y; });
    std::vector<u8> collapsed(G, 0);
    for (u32 gi : order)
    {
        const Asset::Gp& g = a.groups[gi];
        if (g.root) continue;
        bool allowed = true;
        for (u32 k = 0; k < g.count && allowed; ++k)
        {
            const Asset::Cl& m = a.cl[a.pageBase[g.page] + g.first + k];
            if (m.producer != kNone && !collapsed[m.producer]) allowed = false;
        }
        // 乱数は常に 1 回引く（許可されなくても）ので、カットは seed だけで決まる
        const bool coin = rng.Unit() < prob;
        collapsed[gi] = (allowed && coin) ? 1 : 0;
    }
    std::vector<u8> d(a.cl.size(), 0);
    for (size_t i = 0; i < a.cl.size(); ++i)
    {
        const Asset::Cl& c = a.cl[i];
        const bool lower = (c.producer == kNone) || collapsed[c.producer];
        d[i] = (lower && !collapsed[c.consumer]) ? 1 : 0;
    }
    return d;
}

struct Topo
{
    u64 tris = 0, degenerate = 0, unpaired = 0, dupDirected = 0;
    std::set<std::pair<u32, u32>> openEdges;
};

// 面積ゼロ（一直線）の三角形か（格子座標の整数演算で厳密に判定）
bool IsZeroArea(const Asset::Cl& c, const u8* t)
{
    const i64 p0[3] = {c.q[t[0] * 3], c.q[t[0] * 3 + 1], c.q[t[0] * 3 + 2]};
    const i64 e1[3] = {static_cast<i64>(c.q[t[1] * 3]) - p0[0], static_cast<i64>(c.q[t[1] * 3 + 1]) - p0[1], static_cast<i64>(c.q[t[1] * 3 + 2]) - p0[2]};
    const i64 e2[3] = {static_cast<i64>(c.q[t[2] * 3]) - p0[0], static_cast<i64>(c.q[t[2] * 3 + 1]) - p0[1], static_cast<i64>(c.q[t[2] * 3 + 2]) - p0[2]};
    return e1[1] * e2[2] - e1[2] * e2[1] == 0 && e1[2] * e2[0] - e1[0] * e2[2] == 0 && e1[0] * e2[1] - e1[1] * e2[0] == 0;
}

// 判定は 2 つ:
//  ・unpaired: 無向エッジごとに「順向きの本数 - 逆向きの本数」が 0 でない（= 境界がある / 穴がある = クラック）。面積ゼロの三角形の対（境界線上の
//    ヒレ）は本数が釣り合うので許容する（幾何学的に何も覆わない）。
//  ・dupDirected: 面積のある三角形だけで、同じ向きの有向エッジが 2 回以上出る（= 重なり / 折れ返し）。
Topo Analyze(const Asset& a, const std::vector<u8>& drawn)
{
    Topo t;
    std::unordered_map<u64, i32> net;         // 無向エッジ (min, max) → 順向き - 逆向き
    std::unordered_map<u64, u32> nondeg;      // 有向エッジ → 面積のある三角形での本数
    for (size_t i = 0; i < a.cl.size(); ++i)
    {
        if (!drawn[i]) continue;
        const Asset::Cl& c = a.cl[i];
        for (size_t k = 0; k + 2 < c.tri.size(); k += 3)
        {
            const u32 w[3] = {c.wid[c.tri[k]], c.wid[c.tri[k + 1]], c.wid[c.tri[k + 2]]};
            ++t.tris;
            const bool deg = IsZeroArea(c, &c.tri[k]);
            if (deg) ++t.degenerate;
            for (int e = 0; e < 3; ++e)
            {
                const u32 x = w[e], y = w[(e + 1) % 3];
                if (x == y) continue;
                net[(static_cast<u64>(std::min(x, y)) << 32) | std::max(x, y)] += (x < y) ? 1 : -1;
                if (!deg) ++nondeg[(static_cast<u64>(x) << 32) | y];
            }
        }
    }
    for (const auto& kv : net)
        if (kv.second != 0) { ++t.unpaired; t.openEdges.insert({static_cast<u32>(kv.first >> 32), static_cast<u32>(kv.first & 0xFFFFFFFFull)}); }
    for (const auto& kv : nondeg) if (kv.second > 1) ++t.dupDirected;
    return t;
}

// 診断: 最初の重複エッジを持つクラスタを列挙する（VG_TEST_DEBUG）
void ExplainDup(const Asset& a, const std::vector<u8>& drawn)
{
    std::unordered_map<u64, std::vector<std::pair<u32, u32>>> where;   // 有向エッジ → (クラスタ, 三角形)
    for (size_t i = 0; i < a.cl.size(); ++i)
    {
        if (!drawn[i]) continue;
        const Asset::Cl& c = a.cl[i];
        for (size_t k = 0; k + 2 < c.tri.size(); k += 3)
        {
            const u32 w[3] = {c.wid[c.tri[k]], c.wid[c.tri[k + 1]], c.wid[c.tri[k + 2]]};
            for (int e = 0; e < 3; ++e) where[(static_cast<u64>(w[e]) << 32) | w[(e + 1) % 3]].push_back({static_cast<u32>(i), static_cast<u32>(k / 3)});
        }
    }
    int shown = 0;
    for (const auto& kv : where)
    {
        if (kv.second.size() < 2 || shown >= 3) continue;
        ++shown;
        std::printf("    dup edge %u->%u in:\n", static_cast<u32>(kv.first >> 32), static_cast<u32>(kv.first & 0xFFFFFFFFull));
        for (const auto& cw : kv.second)
        {
            const Asset::Cl& c = a.cl[cw.first];
            const u8* t = &c.tri[cw.second * 3];
            std::printf("      cluster %u level %u producer %d consumer %u tri (%u %u %u) verts %zu | err %g parErr %g\n", cw.first, c.level, c.producer == kNone ? -1 : static_cast<int>(c.producer), c.consumer,
                        c.wid[t[0]], c.wid[t[1]], c.wid[t[2]], c.wid.size(), static_cast<double>(c.h.lodError), static_cast<double>(c.h.parentLodError));
            {
                std::printf("        q:");
                for (int k = 0; k < 3; ++k) { const u8 vi = t[k]; std::printf(" [%u %u %u]", c.q[vi * 3], c.q[vi * 3 + 1], c.q[vi * 3 + 2]); }
                std::printf("\n");
            }
        }
    }
}

// 全カットの検査。closed = 閉じた多様体（境界エッジ 0）/ borderLocked = 開いた形状で境界をロックした（境界エッジ集合が LOD0 と一致）。
// 戻り値 = 見つかったクラックの数（0 が正しい）。
u64 CountCracks(const Asset& a, bool closed, bool borderLocked, u32* cutsChecked = nullptr, u64* minTris = nullptr, u64* maxTris = nullptr, u64* dupOut = nullptr)
{
    u64 cracks = 0, dupTotal = 0;
    const Topo base = Analyze(a, CutLod0(a));
    if (EnvSet("VG_TEST_LEVELS"))
        for (u32 L = 0; L < 20; ++L)
        {
            std::vector<u8> cut(a.cl.size(), 0);
            u64 cnt = 0;
            for (size_t i = 0; i < a.cl.size(); ++i) if (a.cl[i].level == L) { cut[i] = 1; ++cnt; }
            if (!cnt) break;
            const Topo t = Analyze(a, cut);
            std::printf("    level %u alone: clusters %llu tris %llu unpaired %llu dup %llu\n", L, static_cast<unsigned long long>(cnt), static_cast<unsigned long long>(t.tris), static_cast<unsigned long long>(t.unpaired),
                        static_cast<unsigned long long>(t.dupDirected));
        }
    if (closed) { if (base.unpaired != 0 || base.dupDirected != 0) return ~0ull; }
    u32 n = 0;
    u64 lo = ~0ull, hi = 0;
    auto check = [&](const std::vector<u8>& cut)
    {
        const Topo t = Analyze(a, cut);
        ++n;
        lo = std::min(lo, t.tris); hi = std::max(hi, t.tris);
        if (t.tris == 0) ++cracks;
        if (t.dupDirected > 32 + t.tris) ++cracks;   // 面積のある三角形の同向きエッジの重複（折れ返し・重なり）が多い（二重被覆は三角形 1 枚あたり 3 本出る。1 本 / 枚を超えたら親子を両方描いた等）。境界の薄い折れ返しで少数出るのは許容する（クラックではない）
        dupTotal += t.dupDirected;
        if (closed) cracks += t.unpaired;
        else if (borderLocked && t.openEdges != base.openEdges) cracks += 1;
        if (EnvSet("VG_TEST_DEBUG") && t.dupDirected) ExplainDup(a, cut);
        if (EnvSet("VG_TEST_VERBOSE") && (t.dupDirected || (closed && t.unpaired)))
            std::printf("    cut %u: tris %llu unpaired %llu dup %llu\n", n, static_cast<unsigned long long>(t.tris), static_cast<unsigned long long>(t.unpaired), static_cast<unsigned long long>(t.dupDirected));
    };
    const f64 dirs[4][3] = {{1, 0, 0}, {0, 1, 0}, {0.3, 0.5, 0.8}, {-1, 0.2, -0.4}};
    static const f64 kDist[] = {0.6, 1.0, 1.2, 1.6, 2.5, 4.0, 7.0, 12.0, 30.0, 100.0, 500.0};
    static const f64 kTau[] = {0.25, 1.0, 3.0};
    int k = 0;
    for (f64 d : kDist)
        for (f64 tau : kTau) { check(CutDistance(a, d, dirs[k % 4], tau)); ++k; }
    for (u32 s = 0; s < 24; ++s) check(CutRandom(a, 100 + s, 0.25 + 0.5 * static_cast<f64>(s % 4) / 3.0));
    if (cutsChecked) *cutsChecked = n;
    if (dupOut) *dupOut = dupTotal;
    if (minTris) *minTris = lo;
    if (maxTris) *maxTris = hi;
    return cracks;
}

// ── LOD0 のカバレッジ ────────────────────────────────────────────────────────────
using Tri9 = std::array<u32, 9>;
Tri9 Canon(const u32* a, const u32* b, const u32* c)
{
    const u32* v[3] = {a, b, c};
    auto less = [](const u32* p, const u32* q) { return std::lexicographical_compare(p, p + 3, q, q + 3); };
    int start = 0;
    for (int i = 1; i < 3; ++i) if (less(v[i], v[start])) start = i;
    Tri9 t{};
    for (int k = 0; k < 3; ++k) std::memcpy(&t[k * 3], v[(start + k) % 3], 12);
    return t;
}

// cooker と同じ規則（エンジン空間へ変換 → 格子へ量子化 → 縮退を除く）で期待される LOD0 の三角形（VG セクションのみ）
std::map<Tri9, int> ExpectedLod0(const VgsrcData& in, const VgeoHeader& h, const std::set<u32>& nonvgSections, u64* dropped = nullptr)
{
    std::map<Tri9, int> out;
    const size_t V = in.positions.size() / 3;
    std::vector<u32> q(V * 3);
    std::vector<f32> dp(V * 3);
    for (size_t v = 0; v < V; ++v)
    {
        f32 p[3];
        ConvertPositionToEngine(in.coordSystem, in.unitScaleToMeters, &in.positions[v * 3], p);
        for (int a = 0; a < 3; ++a) { q[v * 3 + a] = QuantizeCoord(p[a], h.posOrigin[a], h.posStep); dp[v * 3 + a] = DequantizeCoord(q[v * 3 + a], h.posOrigin[a], h.posStep); }
    }
    u64 drop = 0;
    for (size_t si = 0; si < in.sections.size(); ++si)
    {
        if (nonvgSections.count(static_cast<u32>(si))) continue;
        const VgsrcSection& s = in.sections[si];
        for (u32 t = s.firstIndex; t < s.firstIndex + s.indexCount; t += 3)
        {
            const u32 a = in.indices[t], b = in.indices[t + 1], c = in.indices[t + 2];
            bool degenerate = (a == b || b == c || a == c);
            if (!degenerate)
            {
                auto eq = [&](u32 x, u32 y) { return std::memcmp(&dp[x * 3], &dp[y * 3], 12) == 0; };
                degenerate = eq(a, b) || eq(b, c) || eq(a, c);
            }
            if (!degenerate)
            {
                const f32* p0 = &dp[a * 3]; const f32* p1 = &dp[b * 3]; const f32* p2 = &dp[c * 3];
                const f64 e1[3] = {static_cast<f64>(p1[0]) - p0[0], static_cast<f64>(p1[1]) - p0[1], static_cast<f64>(p1[2]) - p0[2]};
                const f64 e2[3] = {static_cast<f64>(p2[0]) - p0[0], static_cast<f64>(p2[1]) - p0[1], static_cast<f64>(p2[2]) - p0[2]};
                degenerate = (e1[1] * e2[2] - e1[2] * e2[1]) == 0.0 && (e1[2] * e2[0] - e1[0] * e2[2]) == 0.0 && (e1[0] * e2[1] - e1[1] * e2[0]) == 0.0;
            }
            if (degenerate) { ++drop; continue; }
            ++out[Canon(&q[a * 3], &q[b * 3], &q[c * 3])];
        }
    }
    if (dropped) *dropped = drop;
    return out;
}

std::map<Tri9, int> GotLod0(const Asset& a)
{
    std::map<Tri9, int> got;
    for (const Asset::Cl& c : a.cl)
    {
        if (c.level != 0) continue;
        for (size_t k = 0; k + 2 < c.tri.size(); k += 3)
            ++got[Canon(&c.q[c.tri[k] * 3], &c.q[c.tri[k + 1] * 3], &c.q[c.tri[k + 2] * 3])];
    }
    return got;
}

// ── DAG の不変条件（Validator とは別に、テスト側でも確認する）───────────────────────────────
void CheckDag(const VgeoContent& c, const Asset& a, const CookOptions& o, const char* name)
{
    u32 roots = 0;
    for (size_t i = 0; i < a.cl.size(); ++i)
    {
        const Asset::Cl& cl = a.cl[i];
        const ClusterHeader& h = cl.h;
        // ③ 全 LOD のクラスタが上限内
        CHECK(ClusterVertexCount(h) >= 1 && ClusterVertexCount(h) <= o.maxVerts && ClusterVertexCount(h) <= kMaxClusterVerts);
        CHECK(ClusterTriangleCount(h) >= 1 && ClusterTriangleCount(h) <= o.maxTris && ClusterTriangleCount(h) <= kMaxClusterTris);
        // ② 誤差の単調性（親 ≥ 子）
        CHECK(h.parentLodError >= h.lodError);
        if (IsPosInf(h.parentLodError)) ++roots;
        // 生産グループ（子）の誤差 = 生んだグループのメンバーの親誤差 ≥ そのメンバー自身の誤差
        if (cl.producer != kNone)
        {
            const Asset::Gp& g = a.groups[cl.producer];
            for (u32 k = 0; k < g.count; ++k)
            {
                const ClusterHeader& m = a.cl[a.pageBase[g.page] + g.first + k].h;
                CHECK(m.parentLodError == h.lodError);
                CHECK(m.lodError <= h.lodError);
                CHECK(ClusterLevel(m) + 1 == ClusterLevel(h));
            }
        }
    }
    CHECK(roots == c.header.rootClusterCount || c.header.rootClusterCount == 0);   // Writer が再計算する値。header は未設定のことがある
    (void)name;
}

// ── GPU 写像（仕様 §14 の HLSL デコードの C++ 移植）────────────────────────────────────────
// ByteAddressBuffer の Load / Load2 と同じ挙動（4 バイト整列のアドレスのみ）。範囲外は失敗として数える。
struct Pool
{
    const u8* p; size_t n;
    bool ok = true;
    // D3D12 の ByteAddressBuffer は範囲外読みが 0 を返す（定義された挙動）。整列違反だけを失敗にし、範囲外は 0 として数える。
    u64 oob = 0;
    u32 Load(u32 addr) { if (addr % 4 != 0) { ok = false; return 0; } if (static_cast<size_t>(addr) + 4 > n) { ++oob; return 0; } u32 v; std::memcpy(&v, p + addr, 4); return v; }
    std::pair<u32, u32> Load2(u32 addr) { return {Load(addr), Load(addr + 4)}; }
};
u32 LoadBitsRef(Pool& pool, u32 base, u32 bitPos, u32 n)
{
    const u32 byteAddr = base + (bitPos >> 3);
    const u32 sh = (byteAddr & 3u) * 8u + (bitPos & 7u);
    const auto w = pool.Load2(byteAddr & ~3u);
    const u32 v = (w.first >> sh) | (sh != 0u ? (w.second << (32u - sh)) : 0u);
    return v & ((1u << n) - 1u);
}
void DecodeOctRef(u32 p, f32 out[3])
{
    f32 fx = static_cast<f32>(static_cast<i32>(p << 16) >> 16) / 32767.0f, fy = static_cast<f32>(static_cast<i32>(p) >> 16) / 32767.0f;
    fx = std::max(fx, -1.0f); fy = std::max(fy, -1.0f);
    f32 nx = fx, ny = fy, nz = 1.0f - std::fabs(fx) - std::fabs(fy);
    if (nz < 0)
    {
        const f32 ox = (1.0f - std::fabs(ny)) * (nx >= 0 ? 1.0f : -1.0f), oy = (1.0f - std::fabs(nx)) * (ny >= 0 ? 1.0f : -1.0f);
        nx = ox; ny = oy;
    }
    const f32 l = std::sqrt(nx * nx + ny * ny + nz * nz);
    out[0] = nx / l; out[1] = ny / l; out[2] = nz / l;
}

void CheckGpuMapping(const VgeoContent& c, const char* name)
{
    std::vector<u8> bytes = WriteBytes(c);
    VgeoMeta m;
    CHECK(LoadMeta(MemorySource(bytes), m).ok());
    const VgeoHeader& h = m.header;
    // ファイルの並び: NODES はそのままコピーできる（192 B AoS）、PAGES は 131072 B スロット × pageCount で 4096 整列
    CHECK(sizeof(HierNode) == 192 && sizeof(ClusterHeader) == 128);
    CHECK(h.sections[kSecNodes].offset % 4096 == 0 && h.sections[kSecPages].offset % 4096 == 0);
    CHECK(h.sections[kSecNodes].size == static_cast<u64>(h.nodeCount) * 192);
    CHECK(std::memcmp(bytes.data() + h.sections[kSecNodes].offset, c.nodes.data(), c.nodes.size() * sizeof(HierNode)) == 0);
    CHECK(h.sections[kSecPages].size == static_cast<u64>(h.pageCount) * kPageSize);
    CHECK(h.pageCount <= 32768);                                   // ByteAddressBuffer 1 本（4 GiB）に収まるスロット数
    for (u32 p = 0; p < h.pageCount; ++p)
    {
        CHECK(m.pageTable[p].fileOffset == h.sections[kSecPages].offset + static_cast<u64>(p) * kPageSize);
        CHECK(m.pageTable[p].fileOffset % 4096 == 0);
    }
    // 全ページを 1 本のプールとして扱い、GPU と同じ手順でデコード
    const u8* pool = bytes.data() + h.sections[kSecPages].offset;
    Pool P{pool, static_cast<size_t>(h.sections[kSecPages].size)};
    u64 checked = 0, mismatches = 0;
    for (u32 p = 0; p < h.pageCount; ++p)
    {
        const u32 pageBase = p * kPageSize;
        const PageHeader ph = ReadPageHeader(pool + pageBase);
        for (u32 i = 0; i < ph.clusterCount; ++i)
        {
            // VgCluster（HLSL）= ClusterHeader: ページ先頭 + 64 + 128 * i。前半は float4 境界 3 本 + 1 本
            const u32 hb = pageBase + kPageHeaderSize + i * kClusterHeaderSize;
            CHECK(hb % 16 == 0);
            ClusterHeader hd;
            std::memcpy(&hd, pool + hb, sizeof hd);
            DecodedCluster dc;
            CHECK(DecodeCluster(pool + pageBase, i, h.posOrigin, h.posStep, dc));
            const u32 vc = ClusterVertexCount(hd), bx = hd.posBits & 31u, by = (hd.posBits >> 5) & 31u, bz = (hd.posBits >> 10) & 31u;
            const u32 bpp = bx + by + bz;
            const u32 vbase = pageBase + hd.vertexOffset;
            for (u32 v = 0; v < vc; ++v)
            {
                const u32 bit = v * bpp;
                const u32 q[3] = {LoadBitsRef(P, vbase, bit, bx) + static_cast<u32>(hd.posMin[0]), LoadBitsRef(P, vbase, bit + bx, by) + static_cast<u32>(hd.posMin[1]),
                                  LoadBitsRef(P, vbase, bit + bx + by, bz) + static_cast<u32>(hd.posMin[2])};
                for (int a = 0; a < 3; ++a)
                {
                    const f32 pos = h.posOrigin[a] + static_cast<f32>(q[a]) * h.posStep;
                    if (q[a] != dc.q[v * 3 + a] || std::memcmp(&pos, &dc.pos[v * 3 + a], 4) != 0) ++mismatches;
                }
                const u32 posBytes = PosBlockBytes(vc, bpp);
                f32 n[3];
                DecodeOctRef(P.Load(vbase + posBytes + 4u * v), n);
                for (int a = 0; a < 3; ++a) if (std::fabs(n[a] - dc.normal[v * 3 + a]) > 2e-5f) ++mismatches;
                const u32 uvp = P.Load(vbase + posBytes + Align16(4u * vc) + 4u * v);
                const f32 u = hd.uvBase[0] + static_cast<f32>(uvp & 0xFFFFu) / 65535.0f * hd.uvScale[0];
                if (std::fabs(u - dc.uv[v * 2]) > 1e-6f * std::max(1.0f, std::fabs(u))) ++mismatches;
            }
            // 三角形: 4 バイト境界をまたぐので 2 dword から取り出す
            for (u32 t = 0; t < ClusterTriangleCount(hd); ++t)
            {
                const u32 b = pageBase + hd.triangleOffset + t * 3u;
                const auto w = P.Load2(b & ~3u);
                const u32 sh = (b & 3u) * 8u;
                const u32 v = (w.first >> sh) | (sh != 0u ? (w.second << (32u - sh)) : 0u);
                const u32 tri[3] = {v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu};
                for (int k = 0; k < 3; ++k) if (tri[k] != dc.tri[t * 3 + k]) ++mismatches;
            }
            ++checked;
        }
    }
    CHECK(P.ok);                       // 2 dword ロードがプールの外へ出ない
    CHECK(mismatches == 0);
    CHECK(checked == h.clusterCount);
    (void)name;
}

// ─────────────────────────────────────────────────────────────────────────────
// テスト本体
// ─────────────────────────────────────────────────────────────────────────────

void TestShape(const char* name, VgsrcData d, bool closed, bool lockBorders, u64 expectMinLevels, u64 maxRoots = 2)
{
    CookOptions o = TestOpts();
    o.lockOpenBorders = lockBorders;
    const VgsrcData keep = d;   // カバレッジ用にコピーを残す
    VgeoContent c;
    CookStats st;
    if (!CookOk(std::move(d), o, c, &st, name)) { CHECK(false); return; }
    CheckValid(c, name);
    const Asset a = Decode(c);
    CheckDag(c, a, o, name);
    // ④ カバレッジ
    const VgeoHeader h = c.header;
    u64 dropped = 0;
    const auto expect = ExpectedLod0(keep, h, {}, &dropped);
    const auto got = GotLod0(a);
    if (expect != got) std::printf("  %s: LOD0 triangle multiset differs (expected %zu distinct, got %zu)\n", name, expect.size(), got.size());
    CHECK(expect == got);
    CHECK(st.degenerateDropped == dropped);
    // ① クラックなし
    u32 cuts = 0;
    u64 lo = 0, hi = 0, dup = 0;
    const u64 cracks = CountCracks(a, closed, lockBorders, &cuts, &lo, &hi, &dup);
    if (cracks) std::printf("  %s: %llu crack(s) over %u cuts\n", name, static_cast<unsigned long long>(cracks), cuts);
    CHECK(cracks == 0);
    // 構造
    CHECK(st.rootClusters >= 1 && st.rootClusters <= maxRoots);
    CHECK(st.levels >= expectMinLevels);
    std::printf("  %-14s tris %7llu -> clusters %6llu levels %2u roots %llu | fill LOD0 %.1f%% all %.1f%% | pages B/tri %.1f | cuts %u (tris %llu..%llu, overlap edges %llu) OK\n", name, static_cast<unsigned long long>(st.vgTris),
                static_cast<unsigned long long>(st.clusters), st.levels, static_cast<unsigned long long>(st.rootClusters), 100.0 * st.avgFillLod0, 100.0 * st.avgFillAll,
                st.vgTris ? static_cast<double>(st.pageBytes) / static_cast<double>(st.vgTris) : 0.0, cuts, static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi), static_cast<unsigned long long>(dup));
    // LOD の深さ ≈ log2(クラスタ数) ± 2（LOD0 が十分大きいとき）
    if (st.lod0Clusters >= 64)
    {
        const double lg = std::log2(static_cast<double>(st.lod0Clusters));
        CHECK(static_cast<double>(st.levels) >= lg - 2.0 && static_cast<double>(st.levels) <= lg + 3.5);
    }
}

void TestCrackFree()
{
    std::printf("[crack-free] sphere / torus / box(UV+normal seams) / blob / knot / rock / grid(open) / 2 materials\n");
    TestShape("sphere", Bench("sphere", 30000), true, false, 5);
    TestShape("torus", Bench("torus", 30000), true, false, 5);
    TestShape("box", MakeBox(24), true, false, 3);
    TestShape("blob", Bench("blob", 50000), true, false, 5);
    TestShape("knot", Bench("knot", 40000), true, false, 5);
    TestShape("rock", Bench("rock", 40000), true, false, 5);
    TestShape("grid(open)", Bench("grid", 20000), false, false, 4);
    TestShape("grid(locked)", Bench("grid", 3200), false, true, 3, 15);
    TestShape("sphere x3 mat", Bench("sphere", 30000, 1, 3), true, false, 3, 15);
    TestShape("torus x2 mat", Bench("torus", 20000, 1, 2), true, false, 3, 15);
}

// 開いた格子（境界をロックしない既定設定）: 境界エッジ（穴）が元の外周の辺に沿ってしか現れない = 内部にクラックが無い。
// 外周は q 空間で qx / qz が最小または最大の 4 辺。境界エッジの両端が同じ辺の上にあることを、全カットで確かめる。
void TestOpenGridBorder()
{
    std::printf("[open border] an unlocked open grid never gets a hole in its interior\n");
    VgeoContent c;
    CHECK(CookOk(Bench("grid", 30000), TestOpts(), c, nullptr, "grid-open"));
    const Asset a = Decode(c);
    u32 qmax[3] = {0, 0, 0};
    for (const auto& q : a.weldQ) for (int k = 0; k < 3; ++k) qmax[k] = std::max(qmax[k], q[k]);
    auto onSide = [&](u32 w, int axis, bool hi) { return a.weldQ[w][axis] == (hi ? qmax[axis] : 0u); };
    u64 badEdges = 0, openTotal = 0;
    u32 n = 0;
    auto check = [&](const std::vector<u8>& cut)
    {
        const Topo t = Analyze(a, cut);
        ++n;
        for (const auto& e : t.openEdges)
        {
            ++openTotal;
            bool ok = false;
            for (int axis : {0, 2})
                for (bool hi : {false, true})
                    if (onSide(e.first, axis, hi) && onSide(e.second, axis, hi)) ok = true;
            if (!ok) ++badEdges;
        }
    };
    const f64 dirs[3][3] = {{1, 0, 0}, {0.3, 0.5, 0.8}, {0, 1, 0.2}};
    int k = 0;
    for (f64 d : {0.6, 1.0, 1.6, 2.5, 4.0, 7.0, 12.0, 30.0, 100.0})
        for (f64 tau : {0.5, 2.0}) check(CutDistance(a, d, dirs[k++ % 3], tau));
    for (u32 s = 0; s < 16; ++s) check(CutRandom(a, 900 + s, 0.5));
    std::printf("  %u cuts: %llu boundary edges in total, %llu of them off the original border\n", n, static_cast<unsigned long long>(openTotal), static_cast<unsigned long long>(badEdges));
    CHECK(badEdges == 0);
    CHECK(openTotal > 0);
}

void TestNegativeControls()
{
    std::printf("[negative controls] the crack detector must fire when locks are off / cuts are inconsistent\n");
    // ロックを切った cook ではクラックが出る
    {
        CookOptions o = TestOpts();
        o.debugNoLocks = true;
        VgeoContent c;
        CHECK(CookOk(Bench("sphere", 30000), o, c, nullptr, "nolocks"));
        const Asset a = Decode(c);
        const u64 cracks = CountCracks(a, true, false);
        std::printf("  boundary locks OFF: %llu crack(s) detected (must be > 0)\n", static_cast<unsigned long long>(cracks));
        CHECK(cracks > 0);
    }
    // 一貫しないカット（クラスタごとにランダム）ではクラックが出る
    {
        VgeoContent c;
        CHECK(CookOk(Bench("sphere", 30000), TestOpts(), c, nullptr, "inconsistent"));
        const Asset a = Decode(c);
        Rng rng(7);
        std::vector<u8> cut(a.cl.size(), 0);
        for (size_t i = 0; i < a.cl.size(); ++i) cut[i] = (rng.Next() % 3 == 0) ? 1 : 0;
        const Topo t = Analyze(a, cut);
        std::printf("  random per-cluster selection: %llu unpaired / %llu duplicated edges (must be > 0)\n", static_cast<unsigned long long>(t.unpaired), static_cast<unsigned long long>(t.dupDirected));
        CHECK(t.unpaired > 0 || t.dupDirected > 0);
    }
}

void TestDeterminism()
{
    std::printf("[determinism] same bytes for 1 / 2 / 5 / 8 threads and across runs\n");
    for (const char* kind : {"blob", "torus"})
    {
        VgsrcData d = Bench(kind, 60000, 3, 2);
        std::vector<u8> ref;
        for (u32 th : {1u, 2u, 5u, 8u, 2u})
        {
            CookOptions o = TestOpts(th);
            VgeoContent c;
            CHECK(CookOk(d, o, c, nullptr, kind));
            const std::vector<u8> b = WriteBytes(c);
            if (ref.empty()) ref = b;
            else if (b != ref) std::printf("  %s: output differs at %u threads\n", kind, th);
            CHECK(b == ref);
        }
        std::printf("  %s: %zu bytes identical\n", kind, ref.size());
    }
    // 生成器もスレッド数に依らない
    BenchOptions a, b;
    a.kind = b.kind = "knot"; a.tris = b.tris = 50000; a.threads = 1; b.threads = 6;
    VgsrcData da, db;
    CHECK(GenerateBench(a, da).ok() && GenerateBench(b, db).ok());
    CHECK(da.positions == db.positions && da.indices == db.indices && da.normals == db.normals);
}

void TestQualityAndCurve()
{
    std::printf("[quality] fill / size / hierarchy / LOD curve on a 200k blob\n");
    VgeoContent c;
    CookStats st;
    CHECK(CookOk(Bench("blob", 200000), TestOpts(), c, &st, "blob200k"));
    CheckValid(c, "blob200k");
    const double lg = std::log2(static_cast<double>(st.lod0Clusters));
    std::printf("  clusters %llu (LOD0 %llu) levels %u (log2 = %.1f) roots %llu | fill LOD0 %.1f%% all %.1f%% | pages B/tri %.2f\n", static_cast<unsigned long long>(st.clusters),
                static_cast<unsigned long long>(st.lod0Clusters), st.levels, lg, static_cast<unsigned long long>(st.rootClusters), 100.0 * st.avgFillLod0, 100.0 * st.avgFillAll,
                static_cast<double>(st.pageBytes) / static_cast<double>(st.vgTris));
    CHECK(st.rootClusters <= 2);
    CHECK(st.avgFillAll >= 0.85);                                   // 平均充填率 >= 85%
    CHECK(static_cast<double>(st.levels) >= lg - 2.0 && static_cast<double>(st.levels) <= lg + 2.0);   // レベル数 ≈ log2(クラスタ数) ± 2
    CHECK(static_cast<double>(st.pageBytes) / static_cast<double>(st.vgTris) <= 40.0);   // 実測は約 33 B/tri（設計書見積もり 25〜30 より大きい。VGEO_SPEC / P1 レポートで報告）
    // 各レベルで三角形がほぼ半分になる
    for (const LevelStat& l : st.perLevel)
        if (l.trisIn > 2000) CHECK(static_cast<double>(l.trisOut) / static_cast<double>(l.trisIn) <= 0.65);
    // 誤差は単調（レベルを上がるほど最大誤差が増える）
    for (size_t i = 1; i < st.perLevel.size(); ++i) CHECK(st.perLevel[i].maxLodErr >= st.perLevel[i - 1].maxLodErr);
    // LOD 曲線: 遠いほど描く三角形が減る（単調非増加）。1 px しきい値で遠方は大幅に減る
    CHECK(st.lodCurve.size() >= 8);
    for (size_t i = 1; i < st.lodCurve.size(); ++i) CHECK(st.lodCurve[i].drawnTris <= st.lodCurve[i - 1].drawnTris);
    CHECK(st.lodCurve.back().reduction > 0.97);   // 誤差は累積上界（子の最大 + 今回）なので保守的。遠方（境界球半径の 160 倍）で 97% 以上削減
    for (const LodCurvePoint& p : st.lodCurve) std::printf("    %6.1fx  %8llu tris  reduction %.2f%%\n", p.distanceOverRadius, static_cast<unsigned long long>(p.drawnTris), 100.0 * p.reduction);
    CheckGpuMapping(c, "blob200k");
}

void TestRegression()
{
    std::printf("[regression] degenerate / isolated / non-manifold / seams / tiny / huge / far / NONVG / no normals\n");
    // 縮退三角形 + 孤立頂点 + 重複三角形 を混ぜた球
    {
        VgsrcData d = Bench("sphere", 8000);
        const u32 v0 = static_cast<u32>(d.positions.size() / 3);
        // 孤立頂点 2 個（どの三角形も参照しない）
        for (int k = 0; k < 2; ++k) { d.positions.insert(d.positions.end(), {50.0f + k, 0.0f, 0.0f}); d.normals.insert(d.normals.end(), {0.0f, 1.0f, 0.0f}); d.uv0.insert(d.uv0.end(), {0.0f, 0.0f}); }
        // 縮退: 同じ添字 / 同じ位置 / 一直線
        d.positions.insert(d.positions.end(), {0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
        d.normals.insert(d.normals.end(), {0, 1, 0, 0, 1, 0, 0, 1, 0});
        d.uv0.insert(d.uv0.end(), {0, 0, 0, 0, 0, 0});
        const u32 c0 = v0 + 2;
        d.indices.insert(d.indices.end(), {0, 0, 1, 5, 5, 5, c0, c0 + 1, c0 + 2, d.indices[0], d.indices[0], d.indices[1]});
        d.indices.insert(d.indices.end(), {d.indices[0], d.indices[1], d.indices[2]});   // 重複三角形（多様体ではなくなる。落ちないこと）
        d.sections[0].indexCount = static_cast<u32>(d.indices.size());
        VgsrcData keep = d;
        VgeoContent c; CookStats st;
        CHECK(CookOk(std::move(d), TestOpts(), c, &st, "degenerate"));
        CheckValid(c, "degenerate");
        u64 dropped = 0;
        const auto expect = ExpectedLod0(keep, c.header, {}, &dropped);
        CHECK(expect == GotLod0(Decode(c)));
        CHECK(dropped >= 4 && st.degenerateDropped == dropped);
        std::printf("  degenerate+isolated+duplicate: dropped %llu\n", static_cast<unsigned long long>(dropped));
    }
    // 非多様体（1 本のエッジを 3 枚の三角形が共有する「ヒレ」）
    {
        VgsrcData d = Bench("sphere", 8000);
        const u32 a = d.indices[0], b = d.indices[1];
        const u32 nv = static_cast<u32>(d.positions.size() / 3);
        d.positions.insert(d.positions.end(), {3.0f, 3.0f, 3.0f});
        d.normals.insert(d.normals.end(), {0, 1, 0}); d.uv0.insert(d.uv0.end(), {0, 0});
        d.indices.insert(d.indices.end(), {a, b, nv});
        d.sections[0].indexCount = static_cast<u32>(d.indices.size());
        VgeoContent c;
        CHECK(CookOk(std::move(d), TestOpts(), c, nullptr, "fin"));
        CheckValid(c, "fin");
    }
    // 小さい入力: 1 三角形 / 12 三角形の立方体 / ちょうど 128 / 129 三角形
    {
        VgsrcData tri = MakeVgsrcFromTriangles({0, 0, 0, 1, 0, 0, 0, 1, 0}, {0, 1, 2});
        VgeoContent c; CookStats st;
        CHECK(CookOk(std::move(tri), TestOpts(), c, &st, "1 tri"));
        CheckValid(c, "1 tri");
        CHECK(st.clusters == 1 && st.levels == 1 && st.rootClusters == 1 && c.pages.size() == kPageSize);
        const Asset a = Decode(c);
        CHECK(a.cl.size() == 1 && a.cl[0].level == 0 && IsPosInf(a.cl[0].h.parentLodError));   // LOD0 かつルート
    }
    {
        VgeoContent c; CookStats st;
        CHECK(CookOk(MakeBox(1), TestOpts(), c, &st, "cube"));
        CheckValid(c, "cube");
        CHECK(st.vgTris == 12 && st.clusters == 1 && st.levels == 1);
    }
    for (u64 want : {128ull, 129ull, 256ull, 257ull, 1000ull})
    {
        // 格子 n x n（2 n^2 三角形）から先頭 want 三角形だけを取る
        BenchOptions bo; bo.kind = "grid"; bo.tris = 2000; bo.threads = 2;
        VgsrcData d;
        CHECK(GenerateBench(bo, d).ok());
        d.indices.resize(static_cast<size_t>(want) * 3);
        d.sections[0].indexCount = static_cast<u32>(d.indices.size());
        VgsrcData keep = d;
        VgeoContent c; CookStats st;
        CHECK(CookOk(std::move(d), TestOpts(), c, &st, "small"));
        CheckValid(c, "small");
        const auto expect = ExpectedLod0(keep, c.header, {});
        CHECK(expect == GotLod0(Decode(c)));
        CHECK(st.rootClusters >= 1 && st.rootClusters <= 2);
        std::printf("  %llu tris: clusters %llu levels %u roots %llu\n", static_cast<unsigned long long>(want), static_cast<unsigned long long>(st.clusters), st.levels, static_cast<unsigned long long>(st.rootClusters));
    }
    // 島（切り離された球 3 個）+ 三角形スープ
    {
        VgsrcData a = Bench("sphere", 6000, 1);
        VgsrcData b = Bench("blob", 6000, 2);
        const u32 off = static_cast<u32>(a.positions.size() / 3);
        for (size_t v = 0; v < b.positions.size() / 3; ++v) { b.positions[v * 3] += 5.0f; }
        a.positions.insert(a.positions.end(), b.positions.begin(), b.positions.end());
        a.normals.insert(a.normals.end(), b.normals.begin(), b.normals.end());
        a.uv0.insert(a.uv0.end(), b.uv0.begin(), b.uv0.end());
        for (u32 i : b.indices) a.indices.push_back(i + off);
        a.sections[0].indexCount = static_cast<u32>(a.indices.size());
        VgsrcData keep = a;
        VgeoContent c; CookStats st;
        CHECK(CookOk(std::move(a), TestOpts(), c, &st, "islands"));
        CheckValid(c, "islands");
        const Asset as = Decode(c);
        CHECK(ExpectedLod0(keep, c.header, {}) == GotLod0(as));
        CHECK(CountCracks(as, true, false) == 0);
        CHECK(st.rootClusters >= 1 && st.rootClusters <= 2);
    }
    {
        // 三角形スープ: 互いに離れた 3000 枚の三角形（簡略化がほぼ効かない病的入力）。成功するか、構造化エラーで止まること（落ちない・止まらないこと）
        std::vector<f32> pos; std::vector<u32> idx;
        for (u32 i = 0; i < 3000; ++i)
        {
            const f32 x = static_cast<f32>(i % 20) * 3.0f, y = static_cast<f32>((i / 20) % 20) * 3.0f, z = static_cast<f32>(i / 400) * 3.0f;
            const u32 b = static_cast<u32>(pos.size() / 3);
            pos.insert(pos.end(), {x, y, z, x + 1, y, z, x, y + 1, z});
            idx.insert(idx.end(), {b, b + 1, b + 2});
        }
        VgeoContent c;
        const VgeoError e = Cook(MakeVgsrcFromTriangles(pos, idx), TestOpts(), c);
        if (e.ok()) CheckValid(c, "soup"); else std::printf("  triangle soup: structured error (%s)\n", e.ToString().c_str());
        CHECK(e.ok() || e.code == Errc::LimitExceeded);
        if (e.ok()) std::printf("  triangle soup: OK (%zu pages)\n", c.pages.size() / kPageSize);
    }
    // 座標の極端: 原点から遠い / とても小さい / とても大きい / UE 座標
    for (const char* mode : {"far", "tiny", "huge"})
    {
        VgsrcData d = Bench("blob", 20000, 5);
        for (size_t v = 0; v < d.positions.size() / 3; ++v)
            for (int k = 0; k < 3; ++k)
            {
                f32& p = d.positions[v * 3 + k];
                if (std::string(mode) == "far") p += (k == 0 ? 10000.0f : k == 1 ? -20000.0f : 30000.0f);
                else if (std::string(mode) == "tiny") p *= 1e-4f;
                else p *= 1e6f;
            }
        VgeoContent c; CookStats st;
        CHECK(CookOk(std::move(d), TestOpts(), c, &st, mode));
        CheckValid(c, mode);
        std::printf("  %s: posStep %.3g levels %u\n", mode, static_cast<double>(c.header.posStep), st.levels);
    }
    {
        // UE 座標（Z up・cm）の入力 = エンジン座標と同じ形（AABB の大きさが一致）
        stub::MeshData mesh;
        CHECK(stub::MakeMeshByName("torus", 12000, 1, mesh));
        VgeoContent ce, cu;
        CHECK(CookOk(stub::MeshToVgsrc(mesh, false), TestOpts(), ce, nullptr, "engine"));
        CHECK(CookOk(stub::MeshToVgsrc(mesh, true), TestOpts(), cu, nullptr, "ue"));
        CheckValid(cu, "ue");
        for (int a = 0; a < 3; ++a)
        {
            CHECK(std::fabs(ce.header.aabbMin[a] - cu.header.aabbMin[a]) < 1e-3f);
            CHECK(std::fabs(ce.header.aabbMax[a] - cu.header.aabbMax[a]) < 1e-3f);
        }
    }
    // NONVG: 2 セクション目を alphaTest 材質にする → プロキシではなく NONVG セクションへ
    {
        VgsrcData d = Bench("sphere", 20000, 1, 2);
        CHECK(d.materials.size() == 2);
        d.materials[1].flags |= kMatAlphaTest;
        VgeoContent c; CookStats st;
        CHECK(CookOk(std::move(d), TestOpts(), c, &st, "nonvg"));
        CheckValid(c, "nonvg");
        CHECK(c.nonvg.size() == 1 && st.nonVgTris > 0 && st.vgTris > 0);
        CHECK(c.materials.size() == 2);
        CHECK(c.materials[c.nonvg[0].materialIndex].sectionKind == 1);
        for (const auto& pr : c.proxy) CHECK(c.materials[pr.materialIndex].sectionKind == 0);
        // 予算を絞ると簡略化される（誤差 > 0、Exact でない）
        CookOptions o = TestOpts();
        o.nonVgMaxTris = 2000;
        VgsrcData d2 = Bench("sphere", 20000, 1, 2);
        d2.materials[1].flags |= kMatAlphaTest;
        VgeoContent c2;
        CHECK(CookOk(std::move(d2), o, c2, nullptr, "nonvg2"));
        CheckValid(c2, "nonvg2");
        CHECK(c2.nonvg.size() == 1 && c2.nonvg[0].indices.size() / 3 <= 2000 + 64 && c2.nonvg[0].error > 0.0f && !(c2.nonvg[0].flags & kProxyFlagExact));
        std::printf("  NONVG: %zu tris exact -> %zu tris (error %.4g)\n", c.nonvg[0].indices.size() / 3, c2.nonvg[0].indices.size() / 3, static_cast<double>(c2.nonvg[0].error));
    }
    // 全部 NONVG（VG クラスタが 0 個）: ヘッダ + 材質 + NONVG だけの有効なファイルになる
    {
        VgsrcData d = Bench("sphere", 5000);
        d.materials[0].flags |= kMatAlphaTest;
        VgeoContent c; CookStats st;
        CHECK(CookOk(std::move(d), TestOpts(), c, &st, "all-nonvg"));
        CheckValid(c, "all-nonvg");
        CHECK(c.pages.empty() && c.nodes.empty() && c.proxy.empty() && c.nonvg.size() == 1 && st.clusters == 0 && st.nonVgTris > 0);
    }
    // 法線 / UV 無しの入力: 位置から滑らかな法線を作る（球なので法線 ≈ 位置方向）
    {
        VgsrcData d = Bench("sphere", 10000);
        d.normals.clear(); d.uv0.clear();
        d.flags = 0;
        VgeoContent c;
        CHECK(CookOk(std::move(d), TestOpts(), c, nullptr, "no-normals"));
        CheckValid(c, "no-normals");
        const Asset a = Decode(c);
        const u8* page0 = c.pages.data();
        double minDot = 1.0, sumDot = 0; u64 n = 0;
        for (u32 i = 0; i < ReadPageHeader(page0).clusterCount; ++i)
        {
            DecodedCluster dc;
            CHECK(DecodeCluster(page0, i, c.header.posOrigin, c.header.posStep, dc));
            for (u32 v = 0; v < dc.vertexCount; ++v)
            {
                const f32* p = &dc.pos[v * 3]; const f32* nn = &dc.normal[v * 3];
                const f64 l = std::sqrt(static_cast<f64>(p[0]) * p[0] + static_cast<f64>(p[1]) * p[1] + static_cast<f64>(p[2]) * p[2]);
                if (l < 1e-6) continue;
                const f64 dot = (p[0] * nn[0] + p[1] * nn[1] + p[2] * nn[2]) / l;
                minDot = std::min(minDot, dot); sumDot += dot; ++n;
            }
        }
        (void)a;
        std::printf("  synthesized normals: mean dot %.4f min %.3f\n", n ? sumDot / static_cast<double>(n) : 0.0, minDot);
        CHECK(n > 0 && sumDot / static_cast<double>(n) > 0.99 && minDot > 0.5);
    }
    // リポジトリ同梱の小さな OBJ（Blender の既定立方体。実アセットではない）を読んで cook できる
    {
        VgsrcData d;
        const VgeoError e = LoadObjFile(std::string(DX12E_TEST_DATA_DIR) + "/cube1m.obj", d);
        CHECK(e.ok());
        if (e.ok())
        {
            VgeoContent c; CookStats st;
            CHECK(CookOk(std::move(d), TestOpts(), c, &st, "cube1m.obj"));
            CheckValid(c, "cube1m.obj");
            CHECK(st.vgTris == 12 && st.clusters == 1);
        }
    }
    // OBJ（四角形・負の添字・usemtl）
    {
        const std::string obj =
            "# cube-ish\nv -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
            "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 -1\nvn 0 0 1\n"
            "usemtl a\nf 1/1/1 4/4/1 3/3/1 2/2/1\nusemtl b\nf 5/1/2 6/2/2 7/3/2 8/4/2\nf -4 -3 -2 -1\n";
        VgsrcData d;
        const VgeoError e = ParseObj(obj, d);
        CHECK(e.ok());
        CHECK(d.sections.size() == 2 && d.materials.size() == 3);   // default（空のまま消える runs を除いても材質は 3 つ作られる）
        VgeoContent c;
        CHECK(CookOk(std::move(d), TestOpts(), c, nullptr, "obj"));
        CheckValid(c, "obj");
        VgsrcData bad;
        CHECK(!ParseObj("f 1 2 3\n", bad).ok());               // 頂点無し = 添字範囲外
        CHECK(!ParseObj("v 0 0 0\n", bad).ok());               // 面が無い
    }
}

void TestInvalid()
{
    std::printf("[invalid input] structured errors, no crash\n");
    auto expectErr = [&](const char* what, VgsrcData d, Errc code, const CookOptions& o = TestOpts())
    {
        VgeoContent c;
        const VgeoError e = Cook(std::move(d), o, c);
        if (e.ok() || e.code != code) std::printf("  %s: expected %s got %s\n", what, ErrcName(code), e.ok() ? "OK" : e.ToString().c_str());
        CHECK(!e.ok() && e.code == code);
        CHECK(!e.message.empty());
    };
    const VgsrcData good = Bench("sphere", 3000);
    { VgsrcData d = good; d.positions.clear(); expectErr("empty positions", std::move(d), Errc::CountMismatch); }
    { VgsrcData d = good; d.indices.clear(); d.sections.clear(); expectErr("empty indices", std::move(d), Errc::CountMismatch); }
    { VgsrcData d = good; d.indices.pop_back(); expectErr("index count % 3", std::move(d), Errc::CountMismatch); }
    { VgsrcData d = good; d.indices[5] = static_cast<u32>(d.positions.size() / 3) + 10; expectErr("index out of range", std::move(d), Errc::ClusterInvalid); }
    { VgsrcData d = good; d.positions[7] = std::numeric_limits<f32>::quiet_NaN(); expectErr("NaN position", std::move(d), Errc::BadFloat); }
    { VgsrcData d = good; d.normals[3] = std::numeric_limits<f32>::infinity(); expectErr("inf normal", std::move(d), Errc::BadFloat); }
    { VgsrcData d = good; d.uv0[1] = std::numeric_limits<f32>::quiet_NaN(); expectErr("NaN uv", std::move(d), Errc::BadFloat); }
    { VgsrcData d = good; d.normals.pop_back(); expectErr("normals size", std::move(d), Errc::CountMismatch); }
    { VgsrcData d = good; d.sections[0].indexCount -= 3; expectErr("sections do not tile", std::move(d), Errc::SectionShape); }
    { VgsrcData d = good; d.sections[0].materialIndex = 9; expectErr("material out of range", std::move(d), Errc::MaterialInvalid); }
    { VgsrcData d = good; d.unitScaleToMeters = 0.0f; expectErr("unit scale 0", std::move(d), Errc::BadFloat); }
    { VgsrcData d = good; d.coordSystem = 7; expectErr("coord system", std::move(d), Errc::UnsupportedFeature); }
    { VgsrcData d = good; for (f32& p : d.positions) p *= 100.0f; d.unitScaleToMeters = std::numeric_limits<f32>::max(); expectErr("overflow after scale", std::move(d), Errc::BadFloat); }
    { // 全部縮退
        VgsrcData d = MakeVgsrcFromTriangles({0, 0, 0, 1, 0, 0, 2, 0, 0}, {0, 1, 2});
        expectErr("all degenerate", std::move(d), Errc::CountMismatch);
    }
    { CookOptions o = TestOpts(); o.maxTris = 5; expectErr("maxTris 5", good, Errc::LimitExceeded, o); }
    { CookOptions o = TestOpts(); o.maxVerts = 200; expectErr("maxVerts 200", good, Errc::LimitExceeded, o); }
    { CookOptions o = TestOpts(); o.groupSize = 1; expectErr("groupSize 1", good, Errc::LimitExceeded, o); }
    { CookOptions o = TestOpts(); o.reduction = 1.0f; expectErr("reduction 1", good, Errc::LimitExceeded, o); }
    { // 材質のパスが安全でない（.. を含む）→ cooker が MaterialInvalid で拒否する（検査を通らないファイルを黙って出さない）
        VgsrcData d = good;
        StringPool pool;
        MaterialRecord m = MakeDefaultMaterial();
        m.nameOff = pool.Add("m");
        m.albedoPathOff = pool.Add("../evil.dds");
        d.materials[0] = m;
        d.strings = pool.Data();
        VgeoContent c;
        const VgeoError e = Cook(std::move(d), TestOpts(), c);
        CHECK(!e.ok() && e.code == Errc::MaterialInvalid);
    }
}

void TestGrouping()
{
    std::printf("[grouping] 4 methods produce valid partitions (sizes, coverage, determinism)\n");
    // 球面上の 20x20 のクラスタ（格子）: 隣接 = 上下左右（重み 8）
    const u32 W = 20, H = 20, N = W * H;
    std::vector<f32> centers(N * 3);
    ClusterGraph g;
    g.n = N;
    g.off.assign(N + 1, 0);
    std::vector<std::vector<std::pair<u32, u32>>> adj(N);
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x)
        {
            const u32 i = y * W + x;
            centers[i * 3] = static_cast<f32>(x); centers[i * 3 + 1] = static_cast<f32>(y); centers[i * 3 + 2] = 0.0f;
            if (x > 0) adj[i].push_back({i - 1, 8}); if (x + 1 < W) adj[i].push_back({i + 1, 8});
            if (y > 0) adj[i].push_back({i - W, 8}); if (y + 1 < H) adj[i].push_back({i + W, 8});
        }
    for (u32 i = 0; i < N; ++i) { g.off[i + 1] = g.off[i] + static_cast<u32>(adj[i].size()); for (auto& e : adj[i]) { g.nbr.push_back(e.first); g.w.push_back(e.second); } }
    GroupingInput in;
    in.n = N; in.centers = centers.data(); in.graph = &g;
    u64 cuts[4] = {};
    int mi = 0;
    for (GroupMethod m : {GroupMethod::Morton, GroupMethod::Bisect, GroupMethod::Greedy, GroupMethod::Meshopt})
    {
        std::vector<u32> gof, gof2;
        const u32 G1 = PartitionClusters(m, 4, in, gof);
        const u32 G2 = PartitionClusters(m, 4, in, gof2);
        CHECK(G1 == G2 && gof == gof2);                                  // 決定的
        CHECK(gof.size() == N);
        std::vector<u32> size(G1, 0);
        for (u32 v : gof) { CHECK(v < G1); ++size[v]; }
        u32 mn = 1000, mx = 0;
        for (u32 s : size) { mn = std::min(mn, s); mx = std::max(mx, s); CHECK(s >= 1); }
        CHECK(mn >= 2 && mx <= kMaxGroupClusters);                       // 1 クラスタだけのグループは併合される
        cuts[mi] = CutWeight(g, gof);
        std::printf("  %-8s groups %3u (sizes %u..%u, avg %.2f) cut weight %llu\n", GroupMethodName(m), G1, mn, mx, static_cast<double>(N) / G1, static_cast<unsigned long long>(cuts[mi]));
        ++mi;
    }
    // 隣接を見る方式（Greedy）は、隣接を見ない Morton 順より境界が少ない（格子で 2 次元的にまとまるため）
    CHECK(cuts[2] <= cuts[0]);
    // 小さい入力
    for (u32 n : {1u, 2u, 3u, 5u, 9u})
    {
        GroupingInput s;
        s.n = n; s.centers = centers.data(); s.graph = nullptr;
        for (GroupMethod m : {GroupMethod::Morton, GroupMethod::Bisect, GroupMethod::Greedy, GroupMethod::Meshopt})
        {
            std::vector<u32> gof;
            const u32 Gn = PartitionClusters(m, 4, s, gof);
            CHECK(gof.size() == n && Gn >= 1);
            std::vector<u32> sz(Gn, 0);
            for (u32 v : gof) { CHECK(v < Gn); ++sz[v]; }
            for (u32 x : sz) CHECK(x >= (n >= 2 ? 2u : 1u) || Gn == 1);
        }
    }
}

void TestBench()
{
    std::printf("[bench generators] deterministic, outward, VGSRC round trip, cook ok\n");
    for (const char* kind : {"blob", "knot", "rock", "torus", "sphere", "grid"})
    {
        VgsrcData d = Bench(kind, 40000, 4);
        const u64 tris = d.indices.size() / 3;
        CHECK(tris >= 36000 && tris <= 44000);
        for (size_t v = 0; v < d.normals.size() / 3; ++v)
        {
            const f32* n = &d.normals[v * 3];
            CHECK(std::fabs(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0f) < 1e-3f);
        }
        // 閉じた形状は外向き（符号付き体積 > 0 = cross(p1-p0, p2-p0) が外向き）
        if (std::string(kind) != "grid")
        {
            f64 vol = 0;
            for (size_t t = 0; t < d.indices.size(); t += 3)
            {
                const f32* p0 = &d.positions[static_cast<size_t>(d.indices[t]) * 3]; const f32* p1 = &d.positions[static_cast<size_t>(d.indices[t + 1]) * 3]; const f32* p2 = &d.positions[static_cast<size_t>(d.indices[t + 2]) * 3];
                vol += p0[0] * (p1[1] * p2[2] - p1[2] * p2[1]) - p0[1] * (p1[0] * p2[2] - p1[2] * p2[0]) + p0[2] * (p1[0] * p2[1] - p1[1] * p2[0]);
            }
            if (!(vol > 0)) std::printf("  %s: signed volume %g (expected > 0)\n", kind, vol);
            CHECK(vol > 0);
        }
        // VGSRC の往復（ハッシュ検証つき）
        std::vector<u8> bytes;
        VectorSink sink(bytes);
        CHECK(WriteVgsrc(d, sink).ok());
        VgsrcData back;
        CHECK(ReadVgsrc(MemorySource(bytes), back, true).ok());
        CHECK(back.positions == d.positions && back.indices == d.indices && back.normals == d.normals && back.uv0 == d.uv0 && back.sections.size() == d.sections.size());
        // 同じ引数 → 同じデータ。seed を変えると変わる（ノイズ系）
        VgsrcData d2 = Bench(kind, 40000, 4);
        CHECK(d2.positions == d.positions);
        if (std::string(kind) == "blob" || std::string(kind) == "rock" || std::string(kind) == "knot")
        {
            VgsrcData d3 = Bench(kind, 40000, 5);
            CHECK(d3.positions != d.positions);
        }
    }
    BenchOptions bo; bo.kind = "nonexistent"; bo.tris = 100;
    VgsrcData d;
    CHECK(!GenerateBench(bo, d).ok());
    bo.kind = "blob"; bo.tris = 0;
    CHECK(!GenerateBench(bo, d).ok());
}

void TestStubCompat()
{
    std::printf("[stub compat] a cooked file and a P0 stub file share one reader / validator\n");
    stub::MeshData mesh;
    CHECK(stub::MakeMeshByName("torus", 20000, 1, mesh));
    stub::StubOptions so;
    VgeoContent stubC, cookC;
    CHECK(stub::BuildStubContent(mesh, so, stubC).ok());
    CHECK(CookOk(stub::MeshToVgsrc(mesh, false), TestOpts(), cookC, nullptr, "compat"));
    for (const VgeoContent* c : {&stubC, &cookC})
    {
        const std::vector<u8> b = WriteBytes(*c);
        VgeoMeta m;
        CHECK(LoadMeta(MemorySource(b), m).ok());
        ReadOptions ro; ro.strictReserved = true;
        CHECK(ValidateVgeo(MemorySource(b), ro).ok());
        VgeoContent back;
        CHECK(LoadContent(MemorySource(b), back).ok());
        CHECK(WriteBytes(back) == b);                                    // 往復で同一バイト
    }
    // 同じ入力メッシュ → LOD0 の三角形集合は一致（量子化格子も同じ AABB から作られる）
    const Asset as = Decode(stubC), ac = Decode(cookC);
    CHECK(stubC.header.posStep == cookC.header.posStep);
    for (int a = 0; a < 3; ++a) CHECK(stubC.header.posOrigin[a] == cookC.header.posOrigin[a]);
    {
        VgeoMeta ms, mc;
        CHECK(LoadMeta(MemorySource(WriteBytes(stubC)), ms).ok() && LoadMeta(MemorySource(WriteBytes(cookC)), mc).ok());
        CHECK(mc.header.levelCount > 1 && ms.header.levelCount == 1);
        CHECK(mc.header.pageCount > 0 && (mc.header.flags & kFlagHasProxy));
    }
    CHECK(GotLod0(as) == GotLod0(ac));
}

} // namespace

int main()
{
    std::printf("VgeoCookTests (cooker version %s)\n", kCookerVersion);
    TestGrouping();
    TestBench();
    TestCrackFree();
    TestOpenGridBorder();
    TestNegativeControls();
    TestDeterminism();
    TestQualityAndCurve();
    TestRegression();
    TestInvalid();
    TestStubCompat();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
