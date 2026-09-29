// `.vgeo` v1.0 形式（renderer/vg/VgeoFormat.h / VgeoBvh.h / VgsrcFormat.h）のテスト。
//
// 守っているもの:
//   ・構造体のサイズ/オフセット（static_assert に加えて実行時にも値を固定）と CRC/FNV の既知値
//   ・書く → 読む → 書く が同一バイト列（往復）と、デコード結果が入力と一致すること（位置 ≤ posStep/2、UV、法線角、三角形）
//   ・境界値: 0 クラスタ / 1 ページ 256 クラスタ / 頂点 128・三角形 128・位置 72bit / 15 クラスタのグループ
//   ・破損入力が「構造化エラー」で落ちること（ヘッダ切れ・オフセット範囲外・カウント不整合・バージョン違い・CRC 不一致 ほか）
//   ・手書き 3 レベル DAG の不変条件（誤差の単調性・球の包含・グループ内一致・依存表）と、それを壊したときの検出
//   ・決定論（同じ入力 → 同じバイト列）と、フォーマット変更の検知（金型ハッシュ）
//   ・VGSRC（中間形式）の往復・破損・座標変換
// 仕様の正本は docs/VGEO_SPEC.md。形式を意図して変えたときは、仕様書 → このテストの金型ハッシュ の順に直す。
// スタブ生成器（meshoptimizer 依存）の検査は vgeo_stub_test.cpp。

#include "renderer/vg/VgeoBvh.h"
#include "renderer/vg/VgeoFormat.h"
#include "renderer/vg/VgsrcFormat.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace dx12e::vg;

namespace { int g_failures = 0, g_checks = 0; bool g_trace = false; int g_fuzzIters = 2500; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_ERR(rep, code)                                                                                   \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if ((rep).first() != (code)) {                                                                         \
            std::printf("FAIL %s:%d: expected %s, got %s\n", __FILE__, __LINE__, ErrcName(code),               \
                        (rep).issues.empty() ? "OK" : (rep).issues.front().ToString().c_str());                \
            ++g_failures;                                                                                      \
        }                                                                                                      \
    } while (0)

namespace
{

u32 g_rng = 20260930u;
u32 Rnd() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }
f32 Rnd01() { return static_cast<f32>(Rnd() & 0xFFFFFF) / 16777216.0f; }

// ── テスト用の小さなジオメトリ: 1 クラスタ = 1 枚の四角形（4 頂点・2 三角形）─────────────────
struct Quad { f32 x, y, z, size; };
struct Grid { f32 origin[3]; f32 step; };

Grid MakeGrid(const std::vector<Quad>& qs)
{
    f64 lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (const Quad& q : qs)
    {
        const f64 mn[3] = {q.x, q.y, q.z}, mx[3] = {q.x + q.size, q.y, q.z + q.size};
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], mn[a]); hi[a] = std::max(hi[a], mx[a]); }
    }
    Grid g{};
    f64 ext = 0;
    for (int a = 0; a < 3; ++a) { g.origin[a] = static_cast<f32>(lo[a]); ext = std::max(ext, hi[a] - static_cast<f64>(g.origin[a])); }
    g.step = ComputePosStep(ext);
    return g;
}

ClusterSource FromQuad(const Grid& g, const Quad& q)
{
    ClusterSource c;
    const f32 P[4][3] = {{q.x, q.y, q.z}, {q.x + q.size, q.y, q.z}, {q.x + q.size, q.y, q.z + q.size}, {q.x, q.y, q.z + q.size}};
    const f32 UV[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    f32 dp[4][3];
    for (int v = 0; v < 4; ++v)
    {
        for (int a = 0; a < 3; ++a) { c.q.push_back(QuantizeCoord(P[v][a], g.origin[a], g.step)); dp[v][a] = DequantizeCoord(c.q.back(), g.origin[a], g.step); }
        c.normalOct.push_back(EncodeOct16(0, 1, 0));
        c.uv.push_back(UV[v][0]); c.uv.push_back(UV[v][1]);
    }
    c.tri = {0, 2, 1, 0, 3, 2};   // (0,2,1): cross が +Y（外向き）
    // 量子化後の点を包む球
    f64 ctr[3] = {0, 0, 0};
    for (int v = 0; v < 4; ++v) for (int a = 0; a < 3; ++a) ctr[a] += dp[v][a] / 4.0;
    f64 r = 0;
    for (int v = 0; v < 4; ++v) r = std::max(r, std::sqrt((dp[v][0] - ctr[0]) * (dp[v][0] - ctr[0]) + (dp[v][1] - ctr[1]) * (dp[v][1] - ctr[1]) + (dp[v][2] - ctr[2]) * (dp[v][2] - ctr[2])));
    f32 s[4] = {static_cast<f32>(ctr[0]), static_cast<f32>(ctr[1]), static_cast<f32>(ctr[2]), std::nextafter(static_cast<f32>(r) * 1.0001f, 1e30f)};
    std::memcpy(c.cullSphere, s, 16); std::memcpy(c.lodSphere, s, 16); std::memcpy(c.parentLodSphere, s, 16);
    c.maxEdgeLength = q.size * 1.4142135f;
    c.coneS8 = PackConeS8(0, 127, 0, -64);   // 法線 +Y、広いコーン
    return c;
}

void FillHeaderMeta(VgeoContent& c, const Grid& g, const std::vector<Quad>& qs, u64 tris)
{
    VgeoHeader& h = c.header;
    h.maxClusterVerts = 128; h.maxClusterTris = 128;
    f64 hi[3] = {-1e300, -1e300, -1e300};
    for (const Quad& q : qs) { hi[0] = std::max(hi[0], static_cast<f64>(q.x + q.size)); hi[1] = std::max(hi[1], static_cast<f64>(q.y)); hi[2] = std::max(hi[2], static_cast<f64>(q.z + q.size)); }
    for (int a = 0; a < 3; ++a) { h.aabbMin[a] = g.origin[a]; h.posOrigin[a] = g.origin[a]; h.aabbMax[a] = static_cast<f32>(hi[a]); }
    h.posStep = g.step;
    h.sourceTriangleCount = tris; h.sourceVertexCount = tris * 2;
    h.sourceHash = 0x1234567890ABCDEFull; h.cookParamsHash = 0xC0FFEE01u;
    std::snprintf(h.cooker, sizeof h.cooker, "vgeo_format_test 1.0");
}

MaterialRecord MakeMat(u32 nameOff, u32 albedoOff, u32 kind = 0)
{
    MaterialRecord m = MakeDefaultMaterial();
    m.nameOff = nameOff; m.albedoPathOff = albedoOff; m.sectionKind = kind;
    return m;
}

// LOD0 のみ・全クラスタが root のコンテンツ（スタブ生成器と同じ形）。groupSize 個ずつを 1 グループにして BVH を作る。
VgeoContent MakeLod0(u32 clusters, u32 groupSize, bool withProxy = true)
{
    std::vector<Quad> qs;
    const u32 W = 32;
    for (u32 i = 0; i < clusters; ++i) qs.push_back({static_cast<f32>(i % W) * 1.5f, 0.25f * static_cast<f32>((i / W) % 5), static_cast<f32>(i / W) * 1.5f, 1.0f});
    if (qs.empty()) qs.push_back({0, 0, 0, 1});
    const Grid g = MakeGrid(qs);
    VgeoContent c;
    FillHeaderMeta(c, g, qs, clusters * 2ull);
    StringPool pool;
    c.materials.push_back(MakeMat(pool.Add("stone"), pool.Add("tex/stone_albedo.dds")));
    c.strings = pool.Data();
    PageBuilder pb(0);
    u32 pageIndex = 0;
    std::vector<BvhLeaf> leaves;
    std::vector<ClusterSource> grp;
    auto flush = [&]()
    {
        if (grp.empty()) return;
        u32 first = 0;
        auto r = pb.TryAddGroup(grp.data(), static_cast<u32>(grp.size()), &first);
        if (r == PageBuilder::AddResult::NoFit)
        {
            pb.FinishAppend(c.pages, true); pb.Reset(++pageIndex);
            r = pb.TryAddGroup(grp.data(), static_cast<u32>(grp.size()), &first);
        }
        CHECK(r == PageBuilder::AddResult::Added);
        std::vector<f32> sph;
        for (auto& k : grp) sph.insert(sph.end(), k.cullSphere, k.cullSphere + 4);
        BvhLeaf lf;
        EnclosingSphere(sph.data(), static_cast<u32>(grp.size()), lf.cullSphere);
        std::memcpy(lf.lodSphere, lf.cullSphere, 16);
        lf.minOwnError = 0; lf.maxParentError = PosInf();
        lf.groupId = static_cast<u32>(leaves.size());
        lf.groupPacked = MakeGroupPacked(pageIndex, first, static_cast<u32>(grp.size()));
        leaves.push_back(lf);
        grp.clear();
    };
    if (clusters > 0)
    {
        for (u32 i = 0; i < clusters; ++i) { grp.push_back(FromQuad(g, qs[i])); if (grp.size() == groupSize) flush(); }
        flush();
        pb.FinishAppend(c.pages, true);
        c.nodes = BuildBvh4(leaves);
        c.header.pinnedPageCount = pageIndex + 1;
        std::vector<f32> sph;
        for (const HierChild& ch : c.nodes[0].child) if (ch.ref != kNone) sph.insert(sph.end(), ch.cullSphere, ch.cullSphere + 4);
        EnclosingSphere(sph.data(), static_cast<u32>(sph.size() / 4), c.header.boundingSphere);
    }
    if (withProxy && clusters > 0)
    {
        ProxySectionData ps;
        ps.materialIndex = 0; ps.flags = kProxyFlagExact; ps.error = 0.0f;
        const f32 P[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}};
        for (int v = 0; v < 4; ++v)
        {
            ProxyVertex pv{};
            std::memcpy(pv.position, P[v], 12); pv.normal[1] = 1.0f; pv.color[0] = pv.color[1] = pv.color[2] = pv.color[3] = 1.0f; pv.tangent[3] = 1.0f;
            pv.texCoord[0] = P[v][0]; pv.texCoord[1] = P[v][2];
            ps.vertices.push_back(pv);
        }
        ps.indices = {0, 2, 1, 0, 3, 2};
        std::memcpy(ps.aabbMin, P[0], 12); std::memcpy(ps.aabbMax, P[2], 12);
        c.proxy.push_back(ps);
    }
    c.debugJson = "{\"test\":\"lod0\"}";
    return c;
}

// ── 手書き 3 レベル DAG ─────────────────────────────────────────────────────────
//   L0: c0..c3 (G0a), c4..c7 (G0b)   … 各グループが 2 クラスタ(L1)を生む
//   L1: d0,d1 (← G0a) d2,d3 (← G0b)  … 4 クラスタが 1 グループ G1 を成し、2 クラスタ(L2)を生む
//   L2: r0,r1 (← G1)                 … root グループ G2（parentLodError = +INF）
//   ページ順（粗い順）: page0 = G2 / page1 = G1 / page2 = G0a + G0b。依存: 1→0, 2→1。pinned = page0 のみ。
constexpr f32 kErrG0a = 0.010f, kErrG0b = 0.012f, kErrG1 = 0.050f;

VgeoContent MakeHandDag()
{
    std::vector<Quad> qs;
    for (int i = 0; i < 8; ++i) qs.push_back({static_cast<f32>(i) * 1.5f, 0, 0, 1.0f});     // c0..c7
    for (int i = 0; i < 4; ++i) qs.push_back({static_cast<f32>(i) * 3.0f, 1.0f, 0, 1.5f});  // d0..d3
    for (int i = 0; i < 2; ++i) qs.push_back({static_cast<f32>(i) * 6.0f, 2.0f, 0, 2.5f});  // r0,r1
    const Grid g = MakeGrid(qs);
    VgeoContent c;
    FillHeaderMeta(c, g, qs, 16);
    StringPool pool;
    c.materials.push_back(MakeMat(pool.Add("wall"), pool.Add("tex/wall.dds")));
    c.strings = pool.Data();

    std::vector<ClusterSource> C(8), D(4), R(2);
    for (int i = 0; i < 8; ++i) C[i] = FromQuad(g, qs[i]);
    for (int i = 0; i < 4; ++i) D[i] = FromQuad(g, qs[8 + i]);
    for (int i = 0; i < 2; ++i) R[i] = FromQuad(g, qs[12 + i]);
    auto enclose = [](const std::vector<ClusterSource>& v, size_t a, size_t n, f32 out[4], bool useLod)
    {
        std::vector<f32> s;
        for (size_t i = a; i < a + n; ++i) s.insert(s.end(), useLod ? v[i].lodSphere : v[i].cullSphere, (useLod ? v[i].lodSphere : v[i].cullSphere) + 4);
        EnclosingSphere(s.data(), static_cast<u32>(n), out);
    };
    f32 sG0a[4], sG0b[4], sG1[4];
    enclose(C, 0, 4, sG0a, false); enclose(C, 4, 4, sG0b, false);
    { f32 s[8]; std::memcpy(s, sG0a, 16); std::memcpy(s + 4, sG0b, 16); EnclosingSphere(s, 2, sG1); }

    // L0: 親 = G0a / G0b
    for (int i = 0; i < 8; ++i)
    {
        C[i].level = 0; C[i].root = false; C[i].lodError = 0;
        C[i].parentLodError = (i < 4) ? kErrG0a : kErrG0b;
        std::memcpy(C[i].parentLodSphere, (i < 4) ? sG0a : sG0b, 16);
    }
    // L1: 自分を生んだのは G0a/G0b（page2）。親 = G1
    for (int i = 0; i < 4; ++i)
    {
        D[i].level = 1; D[i].root = false;
        D[i].lodError = (i < 2) ? kErrG0a : kErrG0b;
        std::memcpy(D[i].lodSphere, (i < 2) ? sG0a : sG0b, 16);
        D[i].parentLodError = kErrG1; std::memcpy(D[i].parentLodSphere, sG1, 16);
        D[i].childPage = 2; D[i].childGroup = MakeGroupPacked(2, (i < 2) ? 0 : 4, 4);
        D[i].coneS8 = PackConeS8(0, 127, 0, -32);
    }
    // L2: 自分を生んだのは G1（page1）。root
    for (int i = 0; i < 2; ++i)
    {
        R[i].level = 2; R[i].root = true; R[i].lodError = kErrG1;
        std::memcpy(R[i].lodSphere, sG1, 16); std::memcpy(R[i].parentLodSphere, sG1, 16);
        R[i].parentLodError = PosInf();
        R[i].childPage = 1; R[i].childGroup = MakeGroupPacked(1, 0, 4);
    }
    PageBuilder p0(0), p1(1), p2(2);
    u32 f = 0;
    CHECK(p0.TryAddGroup(R.data(), 2, &f) == PageBuilder::AddResult::Added && f == 0);
    CHECK(p1.TryAddGroup(D.data(), 4, &f) == PageBuilder::AddResult::Added && f == 0);
    CHECK(p2.TryAddGroup(C.data(), 4, &f) == PageBuilder::AddResult::Added && f == 0);
    CHECK(p2.TryAddGroup(C.data() + 4, 4, &f) == PageBuilder::AddResult::Added && f == 4);
    p0.FinishAppend(c.pages, true); p1.FinishAppend(c.pages, false); p2.FinishAppend(c.pages, false);
    c.header.pinnedPageCount = 1;

    // BVH: 葉 4 個（group id は (page, first) 順: G2=0, G1=1, G0a=2, G0b=3）
    auto leaf = [](u32 id, u32 page, u32 first, u32 cnt, const std::vector<ClusterSource>& mem, size_t a) {
        BvhLeaf l;
        std::vector<f32> cs, ls;
        f32 mo = 1e30f, mp = 0;
        for (size_t i = a; i < a + cnt; ++i)
        {
            cs.insert(cs.end(), mem[i].cullSphere, mem[i].cullSphere + 4); ls.insert(ls.end(), mem[i].parentLodSphere, mem[i].parentLodSphere + 4);
            mo = std::min(mo, mem[i].lodError); mp = std::max(mp, mem[i].parentLodError);
        }
        EnclosingSphere(cs.data(), cnt, l.cullSphere); EnclosingSphere(ls.data(), cnt, l.lodSphere);
        l.minOwnError = mo; l.maxParentError = mp; l.groupId = id; l.groupPacked = MakeGroupPacked(page, first, cnt);
        return l;
    };
    std::vector<BvhLeaf> leaves = {leaf(0, 0, 0, 2, R, 0), leaf(1, 1, 0, 4, D, 0), leaf(2, 2, 0, 4, C, 0), leaf(3, 2, 4, 4, C, 4)};
    c.nodes = BuildBvh4(leaves);
    {
        std::vector<f32> sph;
        for (const HierChild& ch : c.nodes[0].child) if (ch.ref != kNone) sph.insert(sph.end(), ch.cullSphere, ch.cullSphere + 4);
        EnclosingSphere(sph.data(), static_cast<u32>(sph.size() / 4), c.header.boundingSphere);
    }
    c.debugJson = "{\"test\":\"hand_dag\"}";
    return c;
}

std::vector<u8> Bytes(const VgeoContent& c)
{
    std::vector<u8> b;
    const VgeoError e = WriteVgeoToMemory(c, b);
    CHECK(e.ok());
    if (!e.ok()) std::printf("  write error: %s\n", e.ToString().c_str());
    return b;
}

ValidationReport Validate(const std::vector<u8>& b, bool strict = false, u32 maxIssues = 1, bool deep = true)
{
    ReadOptions o; o.strictReserved = strict; o.maxIssues = maxIssues; o.deep = deep;
    return ValidateVgeo(MemorySource(b), o);
}

VgeoHeader GetHeader(const std::vector<u8>& b) { VgeoHeader h; std::memcpy(&h, b.data(), sizeof h); return h; }
void PutHeader(std::vector<u8>& b, const VgeoHeader& h) { std::memcpy(b.data(), &h, sizeof h); }

// 小セクションとページを書き換えた後に CRC を全部つけ直す（内容を壊すテスト用。CRC 由来の失敗と区別するため）
void Reseal(std::vector<u8>& b)
{
    if (b.size() < kHeaderSize) return;   // ファズで切り詰められた入力
    VgeoHeader h = GetHeader(b);
    if (h.pageCount > 0 && h.sections[kSecPageTable].size >= h.pageCount * sizeof(PageTableEntry) && h.sections[kSecPageTable].offset + h.sections[kSecPageTable].size <= b.size() &&
        h.sections[kSecPages].offset + static_cast<u64>(h.pageCount) * kPageSize <= b.size())
        for (u32 p = 0; p < h.pageCount; ++p)
        {
            PageTableEntry e;
            u8* ep = b.data() + h.sections[kSecPageTable].offset + p * sizeof e;
            std::memcpy(&e, ep, sizeof e);
            e.pageCrc32 = Crc32(b.data() + h.sections[kSecPages].offset + static_cast<u64>(p) * kPageSize, kPageSize);
            std::memcpy(ep, &e, sizeof e);
        }
    for (u32 i = 0; i < kSectionCount; ++i)
        if (i != kSecPages && h.sections[i].size > 0 && h.sections[i].offset + h.sections[i].size <= b.size())
            h.sections[i].crc32 = Crc32(b.data() + h.sections[i].offset, h.sections[i].size);
    h.headerCrc32 = Crc32(&h, kHeaderSize - 4);
    PutHeader(b, h);
}

u8* PagePtr(std::vector<u8>& b, u32 p) { return b.data() + GetHeader(b).sections[kSecPages].offset + static_cast<u64>(p) * kPageSize; }
ClusterHeader GetCluster(std::vector<u8>& b, u32 p, u32 i) { return ReadClusterHeader(PagePtr(b, p), i); }
void PutCluster(std::vector<u8>& b, u32 p, u32 i, const ClusterHeader& h) { std::memcpy(PagePtr(b, p) + kPageHeaderSize + static_cast<size_t>(i) * kClusterHeaderSize, &h, sizeof h); }
HierNode* NodePtr(std::vector<u8>& b, u32 n) { return reinterpret_cast<HierNode*>(b.data() + GetHeader(b).sections[kSecNodes].offset) + n; }

// ── 1. レイアウトと既知値 ──────────────────────────────────────────────────────
void TestLayoutAndConstants()
{
    CHECK(sizeof(VgeoHeader) == 512 && sizeof(SectionEntry) == 32 && sizeof(MaterialRecord) == 96);
    CHECK(sizeof(HierChild) == 48 && sizeof(HierNode) == 192 && sizeof(PageTableEntry) == 32 && sizeof(PageHeader) == 64);
    CHECK(sizeof(ClusterHeader) == 128 && sizeof(ProxyHeader) == 32 && sizeof(ProxySectionEntry) == 64 && sizeof(ProxyVertex) == 96);
    CHECK(offsetof(VgeoHeader, sections) == 200 && offsetof(VgeoHeader, headerCrc32) == 508);
    CHECK(offsetof(ClusterHeader, cullSphere) == 32 && offsetof(ClusterHeader, vertexOffset) == 64 && offsetof(ClusterHeader, childGroup) == 116);
    // クラスタヘッダの前半 64 B = カリングが読む値（float4 境界）
    CHECK(offsetof(ClusterHeader, lodSphere) % 16 == 0 && offsetof(ClusterHeader, parentLodSphere) % 16 == 0 && offsetof(ClusterHeader, cullSphere) % 16 == 0);
    CHECK(std::memcmp(&kVgeoMagic, "VGEO", 4) == 0);
    CHECK(std::memcmp(&kPageMagic, "VGPG", 4) == 0);
    CHECK(std::memcmp(&kVgsrcMagic, "VGSR", 4) == 0);
    CHECK(Crc32("123456789", 9) == 0xCBF43926u);
    CHECK(Crc32("", 0) == 0u);
    {   // スライスバイ8 と 1 バイトずつの結果が一致すること（長さ・アラインを変えて）
        std::vector<u8> d(1000);
        for (auto& v : d) v = static_cast<u8>(Rnd());
        for (size_t len : {0u, 1u, 7u, 8u, 9u, 63u, 64u, 65u, 999u})
        {
            u32 s = 0xFFFFFFFFu;
            for (size_t i = 0; i < len; ++i) s = Crc32Update(s, d.data() + i, 1);
            CHECK(~s == Crc32(d.data(), len));
        }
    }
    CHECK(Fnv1a64("", 0) == 0xcbf29ce484222325ull);
    CHECK(Fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(Fnv1a32("a", 1) == 0xE40C292Cu);
    CHECK(kPageHeaderSize + kMaxClustersPerPage * kClusterHeaderSize < kPageSize);
    // ブロックサイズ（仕様 §6.3 の例: 頂点 70・bpp 39・三角形 110 → 頂点ブロック 928 B・三角形 336 B）
    CHECK(VertexBlockBytes(70, 39) == 928 && TriBlockBytes(110) == 336);
    CHECK(VertexBlockBytes(70, 39) + TriBlockBytes(110) + kClusterHeaderSize == 1392);
}

// ── 2. 符号化部品 ─────────────────────────────────────────────────────────────
void TestCodecs()
{
    // oct16: 角度誤差 ≤ 0.05°
    f64 worst = 0;
    for (int i = 0; i < 20000; ++i)
    {
        f64 x = Rnd01() * 2 - 1, y = Rnd01() * 2 - 1, z = Rnd01() * 2 - 1;
        if (i < 6) { x = (i == 0) - (i == 1); y = (i == 2) - (i == 3); z = (i == 4) - (i == 5); }
        const f64 l = std::sqrt(x * x + y * y + z * z);
        if (l < 1e-3) continue;
        x /= l; y /= l; z /= l;
        f32 n[3];
        DecodeOct16(EncodeOct16(x, y, z), n);
        const f64 d = std::min(1.0, x * n[0] + y * n[1] + z * n[2]);
        worst = std::max(worst, std::acos(d) * 180.0 / 3.14159265358979);
        CHECK(std::fabs(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0) < 1e-5);
    }
    CHECK(worst <= 0.05);
    { f32 n[3]; DecodeOct16(EncodeOct16(0, 0, 0), n); CHECK(n[2] > 0.99f); }      // 零ベクトルは +Z
    // ビット詰め
    std::vector<u8> buf(4096, 0);
    struct Item { u64 pos; u32 v; u32 n; };
    std::vector<Item> items;
    u64 pos = 0;
    for (int i = 0; i < 400; ++i)
    {
        const u32 n = Rnd() % 25;   // 0..24
        const u32 v = n ? (Rnd() & ((1u << n) - 1)) : 0;
        PutBits(buf.data(), pos, v, n);
        items.push_back({pos, v, n});
        pos += n;
    }
    for (const Item& it : items) CHECK(GetBits(buf.data(), it.pos, it.n) == it.v);
    // 格子
    CHECK(ComputePosStep(0.0) == 1.0f);
    for (f64 ext : {1.0, 3.0, 12345.678, 0.001, 1e6})
    {
        const f32 s = ComputePosStep(ext);
        CHECK(static_cast<f64>(s) * kGridMax >= ext);
        CHECK(QuantizeCoord(static_cast<f32>(ext), 0.0f, s) <= kGridMax);
    }
    CHECK(QuantizeCoord(-5.0f, 0.0f, 1.0f) == 0);
    CHECK(QuantizeCoord(1e30f, 0.0f, 1.0f) == kGridMax);
    CHECK(QuantizeCoord(2.5f, 0.0f, 1.0f) == 3);     // half up
    CHECK(DequantizeCoord(7, 10.0f, 0.5f) == 13.5f);
    // 球
    { f32 a[4] = {0, 0, 0, 2}, b[4] = {1, 0, 0, 1}, c[4] = {5, 0, 0, 1};
      CHECK(SphereContains(a, b)); CHECK(!SphereContains(a, c)); CHECK(SphereContains(a, a)); }
    { f32 s[12] = {0, 0, 0, 1, 10, 0, 0, 1, 0, 10, 0, 2}, e[4]; EnclosingSphere(s, 3, e);
      for (int i = 0; i < 3; ++i) CHECK(SphereContains(e, s + i * 4, 0.0, 0.0)); }
    // packed
    CHECK(GroupPage(MakeGroupPacked(65534, 255, 15)) == 65534 && GroupFirst(MakeGroupPacked(65534, 255, 15)) == 255 && GroupCount(MakeGroupPacked(65534, 255, 15)) == 15);
    CHECK(ClusterRefPage(MakeClusterRef(1234, 200)) == 1234 && ClusterRefIndex(MakeClusterRef(1234, 200)) == 200);
    CHECK(NodeRefIsLeaf(MakeLeafRef(77)) && !NodeRefIsLeaf(77) && !NodeRefIsLeaf(kNone));
    CHECK(IsPosInf(PosInf()) && F2U(PosInf()) == kInfBits);
    // 文字列 / パス
    CHECK(detail::IsSafeRelativePath("tex/a.dds") && detail::IsSafeRelativePath("a.dds"));
    CHECK(!detail::IsSafeRelativePath("../a.dds") && !detail::IsSafeRelativePath("/abs/a.dds") && !detail::IsSafeRelativePath("C:/a.dds") &&
          !detail::IsSafeRelativePath("a\\b.dds") && !detail::IsSafeRelativePath("a//b") && !detail::IsSafeRelativePath("") && !detail::IsSafeRelativePath("a/./b"));
    CHECK(detail::IsValidUtf8("abc\xE3\x81\x82", 6) && !detail::IsValidUtf8("\xC0\x80", 2) && !detail::IsValidUtf8("\xE3\x81", 2));
}

// ── 3. 往復・デコード・ファイル ──────────────────────────────────────────────────
void TestRoundTrip()
{
    VgeoContent c = MakeLod0(300, 4);
    const std::vector<u8> b1 = Bytes(c);
    CHECK(b1.size() % 4096 == 0);
    auto rep = Validate(b1, true, 32);
    CHECK(rep.ok());
    for (auto& i : rep.issues) std::printf("  issue: %s\n", i.ToString().c_str());

    VgeoContent c2;
    CHECK(LoadContent(MemorySource(b1), c2).ok());
    CHECK(c2.pages == c.pages && c2.nodes.size() == c.nodes.size() && c2.materials.size() == 1 && c2.proxy.size() == 1 && c2.debugJson == c.debugJson);
    CHECK(c2.header.clusterCount == 300 && c2.header.rootClusterCount == 300 && c2.header.levelCount == 1 && c2.header.groupCount == 75);
    const std::vector<u8> b2 = Bytes(c2);
    CHECK(b1 == b2);   // 書く → 読む → 書く が同一バイト

    // メタの目次と文字列
    VgeoMeta m;
    CHECK(LoadMeta(MemorySource(b1), m).ok());
    CHECK(std::string(m.String(m.materials[0].nameOff)) == "stone" && std::string(m.String(m.materials[0].albedoPathOff)) == "tex/stone_albedo.dds");
    CHECK(std::string(m.header.cooker) == "vgeo_format_test 1.0");
    CHECK(m.header.sections[kSecPages].offset % 4096 == 0);
    CHECK(m.pageTable.size() == m.header.pageCount && m.pageDepOffsets.size() == m.header.pageCount + 1u && m.pageDeps.empty());

    // デコード結果が入力と一致（位置 ≤ posStep/2、UV、法線角、三角形）
    std::vector<Quad> qs;
    for (u32 i = 0; i < 300; ++i) qs.push_back({static_cast<f32>(i % 32) * 1.5f, 0.25f * static_cast<f32>((i / 32) % 5), static_cast<f32>(i / 32) * 1.5f, 1.0f});
    const Grid g = MakeGrid(qs);
    u32 checkedClusters = 0;
    for (u32 p = 0; p < m.header.pageCount; ++p)
    {
        std::vector<u8> page(kPageSize);
        CHECK(ReadPage(MemorySource(b1), m, p, page.data()).ok());
        const PageHeader ph = ReadPageHeader(page.data());
        for (u32 i = 0; i < ph.clusterCount; ++i)
        {
            DecodedCluster dc;
            CHECK(DecodeCluster(page.data(), i, m.header.posOrigin, m.header.posStep, dc));
            CHECK(dc.vertexCount == 4 && dc.triangleCount == 2);
            // 元のクアッドを特定（各クラスタの最小位置から）
            const Quad& q = qs[checkedClusters];   // グループ順 = 生成順（MakeLod0 は順に詰める）
            const f32 P[4][3] = {{q.x, q.y, q.z}, {q.x + q.size, q.y, q.z}, {q.x + q.size, q.y, q.z + q.size}, {q.x, q.y, q.z + q.size}};
            for (int v = 0; v < 4; ++v)
            {
                for (int a = 0; a < 3; ++a) CHECK(std::fabs(dc.pos[v * 3 + a] - P[v][a]) <= g.step * 0.5f + 2e-5f);
                CHECK(std::fabs(dc.normal[v * 3 + 1] - 1.0f) < 1e-3f);
                CHECK(std::fabs(dc.uv[v * 2] - static_cast<f32>((v == 1 || v == 2) ? 1 : 0)) <= 1.0f / 65535.0f + 1e-6f);
                CHECK(std::fabs(dc.uv[v * 2 + 1] - static_cast<f32>((v >= 2) ? 1 : 0)) <= 1.0f / 65535.0f + 1e-6f);
            }
            CHECK((dc.tri == std::vector<u8>{0, 2, 1, 0, 3, 2}));
            ++checkedClusters;
        }
    }
    CHECK(checkedClusters == 300);

    // プロキシ
    std::vector<ProxySectionData> px;
    CHECK(ReadProxySections(MemorySource(b1), m, kSecProxy, px).ok() && px.size() == 1 && px[0].vertices.size() == 4 && px[0].indices.size() == 6);
    CHECK(px[0].vertices[2].position[0] == 1.0f && px[0].vertices[2].position[2] == 1.0f);

    // ファイル経由（FileSource / FileSink）
    const auto tmp = std::filesystem::temp_directory_path() / "vgeo_format_test_roundtrip.vgeo";
    CHECK(WriteVgeoToFile(c, tmp.string()).ok());
    FileSource fs;
    CHECK(fs.Open(tmp.string()));
    CHECK(fs.Size() == b1.size());
    std::vector<u8> fb(b1.size());
    CHECK(fs.Read(0, fb.data(), fb.size()) && fb == b1);
    CHECK(ValidateVgeo(fs).ok());
    CHECK(!fs.Read(b1.size() - 1, fb.data(), 2));            // 範囲外は false
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    FileSource missing;
    CHECK(!missing.Open((std::filesystem::temp_directory_path() / "vgeo_no_such_file.vgeo").string()));
    MemorySource ms(b1);
    u8 one[1];
    CHECK(ms.Read(b1.size() - 1, one, 1) && !ms.Read(b1.size(), one, 1) && !ms.Read(~0ull, one, 1));
}

// ── 4. 決定論と金型ハッシュ ──────────────────────────────────────────────────────
void TestDeterminism()
{
    const std::vector<u8> a = Bytes(MakeHandDag()), b = Bytes(MakeHandDag());
    CHECK(a == b);
    const std::vector<u8> l1 = Bytes(MakeLod0(500, 4)), l2 = Bytes(MakeLod0(500, 4));
    CHECK(l1 == l2);
    // ★金型: 手書き 3 レベル DAG の全バイトの FNV-1a 64。値が変わった = 形式（バイト列）が変わった。
    //   意図した変更なら docs/VGEO_SPEC.md の「変更履歴」を更新してからこの値を差し替える。
    const u64 h = Fnv1a64(a.data(), a.size());
    constexpr u64 kGoldenHandDag = 0x10295F14C2901A31ull;   // 全 421888 バイトの FNV-1a 64
    if (kGoldenHandDag == 0) std::printf("  golden hand-dag: size=%zu fnv1a64=0x%016llXull\n", a.size(), static_cast<unsigned long long>(h));
    CHECK(kGoldenHandDag == h);
}

// ── 5. 手書き DAG の不変条件 ─────────────────────────────────────────────────────
void TestHandDag()
{
    VgeoContent c = MakeHandDag();
    const std::vector<u8> b = Bytes(c);
    auto rep = Validate(b, true, 32);
    CHECK(rep.ok());
    for (auto& i : rep.issues) std::printf("  issue: %s\n", i.ToString().c_str());
    VgeoMeta m;
    CHECK(LoadMeta(MemorySource(b), m).ok());
    const VgeoHeader& h = m.header;
    CHECK(h.clusterCount == 14 && h.groupCount == 4 && h.pageCount == 3 && h.nodeCount == 1 && h.levelCount == 3 && h.rootClusterCount == 2 && h.pinnedPageCount == 1);
    CHECK(m.pageDepOffsets.size() == 4 && m.pageDepOffsets[1] == 0 && m.pageDepOffsets[2] == 1 && m.pageDepOffsets[3] == 2);
    CHECK(m.pageDeps.size() == 2 && m.pageDeps[0] == 0 && m.pageDeps[1] == 1);
    CHECK(m.pageTable[0].priority == 0 && m.pageTable[1].priority == 1 && m.pageTable[2].priority == 2);
    CHECK(LevelMinOf(m.pageTable[0].levelFlags) == 2 && LevelMaxOf(m.pageTable[2].levelFlags) == 0);
    CHECK((PageFlagsOf(m.pageTable[0].levelFlags) & kPageFlagPinned) && !(PageFlagsOf(m.pageTable[1].levelFlags) & kPageFlagPinned));
    // BVH の集約値
    const HierNode& n0 = m.nodes[0];
    CHECK(n0.child[0].minOwnError == kErrG1 && IsPosInf(n0.child[0].maxParentError));
    CHECK(n0.child[1].minOwnError == kErrG0a && n0.child[1].maxParentError == kErrG1);
    CHECK(n0.child[2].minOwnError == 0.0f && n0.child[2].maxParentError == kErrG0a);
    CHECK(n0.child[3].minOwnError == 0.0f && n0.child[3].maxParentError == kErrG0b);
    // 単調性: どの視点でも 親の射影誤差 ≥ 子の射影誤差（球の包含 + 誤差の大小）
    for (u32 p = 0; p < 3; ++p)
    {
        std::vector<u8> pg(kPageSize);
        CHECK(ReadPage(MemorySource(b), m, p, pg.data()).ok());
        for (u32 i = 0; i < m.pageTable[p].clusterCount; ++i)
        {
            const ClusterHeader ch = ReadClusterHeader(pg.data(), i);
            CHECK(ch.parentLodError >= ch.lodError);
            CHECK(SphereContains(ch.parentLodSphere, ch.lodSphere, 0.0, 1e-4));
            for (int t = 0; t < 40; ++t)
            {
                const f32 cam[3] = {(Rnd01() - 0.5f) * 60, (Rnd01() - 0.5f) * 60, (Rnd01() - 0.5f) * 60};
                auto proj = [&](f32 err, const f32* s) {
                    const f64 d = std::max(std::sqrt(std::pow(static_cast<f64>(s[0]) - cam[0], 2) + std::pow(static_cast<f64>(s[1]) - cam[1], 2) + std::pow(static_cast<f64>(s[2]) - cam[2], 2)) - s[3], 0.1);
                    return static_cast<f64>(err) / d;
                };
                if (IsPosInf(ch.parentLodError)) continue;
                CHECK(proj(ch.parentLodError, ch.parentLodSphere) >= proj(ch.lodError, ch.lodSphere) * (1.0 - 1e-6));
            }
        }
    }
}

// 内容を壊して、期待どおりのエラーコードで落ちること（CRC は付け直してある = CRC 以外の検査が働く）
void TestDagViolations()
{
    const std::vector<u8> base = Bytes(MakeHandDag());
    auto run = [&](const char* name, Errc want, auto&& mutate, bool deep = true)
    {
        std::vector<u8> b = base;
        mutate(b);
        Reseal(b);
        const ValidationReport rep = Validate(b, false, 1, deep);
        ++g_checks;
        if (rep.first() != want) { std::printf("FAIL %s: expected %s, got %s\n", name, ErrcName(want), rep.issues.empty() ? "OK" : rep.issues.front().ToString().c_str()); ++g_failures; }
    };
    // d0 (page1 cluster 0): parentLodError < lodError
    run("error not monotonic", Errc::LodInvariant, [](auto& b) { ClusterHeader h = GetCluster(b, 1, 0); h.parentLodError = 0.001f; PutCluster(b, 1, 0, h); });
    // c0 (page2 cluster 0): parent sphere shrunk so it no longer contains the child sphere
    run("sphere containment", Errc::LodInvariant, [](auto& b) { ClusterHeader h = GetCluster(b, 2, 0); h.parentLodSphere[3] = 0.001f; PutCluster(b, 2, 0, h); });
    // 同じグループのメンバー間で parentLodError が食い違う（c1）
    run("group members disagree", Errc::LodInvariant, [](auto& b) { ClusterHeader h = GetCluster(b, 2, 1); h.parentLodError = 0.011f; PutCluster(b, 2, 1, h); });
    // d0 の childGroup が実在しないグループ（first=1）を指す
    run("child group not a group", Errc::LodInvariant, [](auto& b) { ClusterHeader h = GetCluster(b, 1, 0); h.childGroup = MakeGroupPacked(2, 1, 4); PutCluster(b, 1, 0, h); });
    // d0.lodError が子のグループの誤差と違う
    run("lodError != child group's parent error", Errc::LodInvariant, [](auto& b) { ClusterHeader h = GetCluster(b, 1, 0); h.lodError = 0.0101f; h.parentLodError = 0.05f; PutCluster(b, 1, 0, h); });
    // LOD0 なのに childPage を持つ
    run("lod0 with childPage", Errc::ClusterInvalid, [](auto& b) { ClusterHeader h = GetCluster(b, 2, 0); h.childPage = 1; PutCluster(b, 2, 0, h); });
    // root フラグと parentLodError の食い違い
    run("root flag mismatch", Errc::LodInvariant, [](auto& b) { ClusterHeader h = GetCluster(b, 0, 0); h.flags &= ~kClusterFlagRoot; PutCluster(b, 0, 0, h); });
    // 深い検査: 三角形インデックスが頂点数以上
    run("triangle index out of range (deep)", Errc::ClusterInvalid, [](auto& b) { ClusterHeader h = GetCluster(b, 2, 0); b[GetHeader(b).sections[kSecPages].offset + 2ull * kPageSize + h.triangleOffset] = 200; });
    // 深い検査: 頂点がクラスタ球の外
    run("vertex outside cullSphere (deep)", Errc::ClusterInvalid, [](auto& b) { ClusterHeader h = GetCluster(b, 2, 0); h.cullSphere[3] = 0.01f; std::memcpy(h.lodSphere, h.cullSphere, 16); PutCluster(b, 2, 0, h); });
    // 浅い検査（deep = false）では通る = deep フラグが効いている（球を小さくするだけなら包含関係は壊れない）
    {
        std::vector<u8> b = base;
        ClusterHeader h = GetCluster(b, 2, 0); h.cullSphere[3] = 0.01f; std::memcpy(h.lodSphere, h.cullSphere, 16);
        PutCluster(b, 2, 0, h); Reseal(b);
        CHECK(Validate(b, false, 8, false).ok());
        CHECK(Validate(b, false, 8, true).first() == Errc::ClusterInvalid);
    }
    // ノード集約: 葉の maxParentError が実際の最大と違う
    run("bvh aggregate min/max", Errc::LodInvariant, [](auto& b) { NodePtr(b, 0)->child[1].maxParentError = 0.5f; });
    run("bvh cullSphere too small", Errc::LodInvariant, [](auto& b) { NodePtr(b, 0)->child[2].cullSphere[3] = 0.001f; });
    // ノード構造
    run("node ref out of range", Errc::NodeInvalid, [](auto& b) { NodePtr(b, 0)->child[0].ref = 5; });
    run("group id out of range", Errc::NodeInvalid, [](auto& b) { NodePtr(b, 0)->child[0].ref = MakeLeafRef(99); });
    run("group id out of order", Errc::GroupInvalid, [](auto& b) { std::swap(NodePtr(b, 0)->child[0].ref, NodePtr(b, 0)->child[1].ref); });
    run("group range beyond page", Errc::GroupInvalid, [](auto& b) { NodePtr(b, 0)->child[0].groupPacked = MakeGroupPacked(0, 1, 2); });
    run("group not tiling", Errc::GroupInvalid, [](auto& b) { NodePtr(b, 0)->child[3].groupPacked = MakeGroupPacked(2, 5, 3); });
    // 依存表 / pinned
    run("deps mismatch", Errc::DepsInvalid, [](auto& b) { VgeoHeader h = GetHeader(b); u32* d = reinterpret_cast<u32*>(b.data() + h.sections[kSecPageDeps].offset); d[5] = 0; });   // page2 の依存を 1 → 0 に
    run("pinned count disagrees with page flags", Errc::DepsInvalid, [](auto& b) { VgeoHeader h = GetHeader(b); h.pinnedPageCount = 2; PutHeader(b, h); });
    // ページ
    run("usedBytes wrong", Errc::PageInvalid, [](auto& b) { PageHeader ph = ReadPageHeader(PagePtr(b, 1)); ph.usedBytes += 16; std::memcpy(PagePtr(b, 1), &ph, sizeof ph); });
    run("payload not zero after usedBytes", Errc::PageInvalid, [](auto& b) { PagePtr(b, 1)[kPageSize - 1] = 1; });
    run("vertexOffset not packed", Errc::ClusterInvalid, [](auto& b) { ClusterHeader h = GetCluster(b, 2, 1); h.vertexOffset += 16; PutCluster(b, 2, 1, h); });
    run("cluster material out of range", Errc::MaterialInvalid, [](auto& b) { ClusterHeader h = GetCluster(b, 2, 1); h.packedCounts |= 5u << 16; PutCluster(b, 2, 1, h); });
    run("posBits > 24", Errc::ClusterInvalid, [](auto& b) { ClusterHeader h = GetCluster(b, 2, 1); h.posBits |= 31u; PutCluster(b, 2, 1, h); });
    run("NaN sphere", Errc::BadFloat, [](auto& b) { ClusterHeader h = GetCluster(b, 2, 1); h.cullSphere[0] = std::nanf(""); PutCluster(b, 2, 1, h); });
    // 複数件収集: 独立した 2 つの問題を両方報告する
    {
        std::vector<u8> b = base;
        ClusterHeader h1 = GetCluster(b, 1, 0); h1.parentLodError = 0.001f; PutCluster(b, 1, 0, h1);
        ClusterHeader h2 = GetCluster(b, 2, 1); h2.posBits |= 31u; PutCluster(b, 2, 1, h2);
        Reseal(b);
        const auto rep = Validate(b, false, 8);
        CHECK(rep.issues.size() >= 2 && rep.Has(Errc::LodInvariant) && rep.Has(Errc::ClusterInvalid));
    }
}

// ── 6. ファイル / ヘッダ / セクションの破損 ────────────────────────────────────────
void TestCorruption()
{
    const std::vector<u8> base = Bytes(MakeLod0(120, 4));
    CHECK(Validate(base, true, 8).ok());

    { std::vector<u8> b(base.begin(), base.begin() + 100);  CHECK_ERR(Validate(b), Errc::Truncated); }       // ヘッダ切れ
    { std::vector<u8> b(base.begin(), base.begin() + 8);    CHECK_ERR(Validate(b), Errc::Truncated); }
    { std::vector<u8> b;                                    CHECK_ERR(Validate(b), Errc::Truncated); }
    { std::vector<u8> b(base.begin(), base.begin() + 511);  CHECK_ERR(Validate(b), Errc::Truncated); }
    { std::vector<u8> b(base.begin(), base.begin() + base.size() / 2); CHECK_ERR(Validate(b), Errc::SectionRange); }  // 途中で切れた
    { std::vector<u8> b = base; b[0] = 'X';                 CHECK_ERR(Validate(b), Errc::BadMagic); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.versionMajor = 2; PutHeader(b, h); CHECK_ERR(Validate(b), Errc::UnsupportedMajor); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.versionMajor = 0; PutHeader(b, h); CHECK_ERR(Validate(b), Errc::UnsupportedMajor); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.versionMinor = 9; PutHeader(b, h); Reseal(b); CHECK(Validate(b).ok()); }   // minor 違いは読める
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.headerSize = 256; PutHeader(b, h); CHECK_ERR(Validate(b), Errc::BadHeaderSize); }
    { std::vector<u8> b = base; b[40] ^= 1;                 CHECK_ERR(Validate(b), Errc::HeaderCrcMismatch); }   // nodeCount を CRC 更新なしで変更
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.pageSize = 65536; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::BadPageSize); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.maxClusterVerts = 129; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::LimitExceeded); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.maxClusterTris = 129; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::LimitExceeded); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.pageCount = 65536; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::LimitExceeded); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.flags |= 1u << 3; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::UnsupportedFeature); }   // 未知の必須機能
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.flags |= 1u << 20; PutHeader(b, h); Reseal(b); CHECK(Validate(b).ok()); }                    // 任意ヒントは無視
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.clusterMaterialMode = 1; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::UnsupportedFeature); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.sections[kSecNodes].offset = 1ull << 40; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::SectionRange); }   // オフセット範囲外
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.sections[kSecNodes].offset += 1; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::SectionAlign); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.sections[kSecPageTable].offset = h.sections[kSecNodes].offset; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::SectionRange); }   // 重なり
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.sections[kSecNodes].size += 192; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::SectionShape); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.sections[kSecNodes].stride = 96; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::SectionShape); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.materialCount = 2; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::CountMismatch); }        // カウント不整合
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.nodeCount += 1; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::CountMismatch); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.clusterCount = 0; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::CountMismatch); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.rootNode = 3; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::NodeInvalid); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.posStep = 0.0f; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::BadFloat); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.aabbMax[0] = std::nanf(""); PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::BadFloat); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.flags &= ~kFlagHasProxy; PutHeader(b, h); Reseal(b); CHECK_ERR(Validate(b), Errc::CountMismatch); }
    // チェックサム
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); b[h.sections[kSecMaterials].offset + 10] ^= 0x55; CHECK_ERR(Validate(b), Errc::SectionCrcMismatch); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); b[h.sections[kSecNodes].offset + 100] ^= 0x01; CHECK_ERR(Validate(b), Errc::SectionCrcMismatch); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); b[h.sections[kSecPages].offset + 5000] ^= 0x01; CHECK_ERR(Validate(b), Errc::PageCrcMismatch); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); b[h.sections[kSecProxy].offset + 100] ^= 0x01; CHECK_ERR(Validate(b), Errc::SectionCrcMismatch); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); b[h.sections[kSecPages].offset + 5000] ^= 0x01;                       // CRC 検証を切れば構造検査だけが働く
      ReadOptions o; o.verifyCrc = false; CHECK(ValidateVgeo(MemorySource(b), o).first() != Errc::PageCrcMismatch); }
    // 材質・文字列
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); MaterialRecord m; std::memcpy(&m, b.data() + h.sections[kSecMaterials].offset, sizeof m);
      m.sectionKind = 7; std::memcpy(b.data() + h.sections[kSecMaterials].offset, &m, sizeof m); Reseal(b); CHECK_ERR(Validate(b), Errc::MaterialInvalid); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); MaterialRecord m; std::memcpy(&m, b.data() + h.sections[kSecMaterials].offset, sizeof m);
      m.flags |= kMatAlphaTest; std::memcpy(b.data() + h.sections[kSecMaterials].offset, &m, sizeof m); Reseal(b); CHECK_ERR(Validate(b), Errc::MaterialInvalid); }   // MASK は VG 材質になれない
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); MaterialRecord m; std::memcpy(&m, b.data() + h.sections[kSecMaterials].offset, sizeof m);
      m.albedoPathOff = 100000; std::memcpy(b.data() + h.sections[kSecMaterials].offset, &m, sizeof m); Reseal(b); CHECK_ERR(Validate(b), Errc::MaterialInvalid); }
    for (const char* evil : {"../evil.dds", "/abs/evil.dds", "C:/evil.dds", "a\b.dds", "a//b.dds", ""})   // 安全でないテクスチャパスは拒否
    {
        VgeoContent c = MakeLod0(10, 4);
        StringPool pool; c.materials[0] = MakeMat(pool.Add("m"), pool.Add(evil));
        c.strings = pool.Data();
        CHECK_ERR(Validate(Bytes(c)), Errc::MaterialInvalid);
    }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); b[h.sections[kSecStrings].offset] = 'x'; Reseal(b); CHECK_ERR(Validate(b), Errc::StringInvalid); }
    // プロキシ
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); ProxySectionEntry e; u8* p = b.data() + h.sections[kSecProxy].offset + kProxyHeaderSize;
      std::memcpy(&e, p, sizeof e); e.materialIndex = 9; std::memcpy(p, &e, sizeof e); Reseal(b); CHECK_ERR(Validate(b), Errc::ProxyInvalid); }
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); u32 idx = 99; std::memcpy(b.data() + h.sections[kSecProxy].offset + 32 + 64 + 4 * 96 + 0, &idx, 4);
      Reseal(b); CHECK_ERR(Validate(b), Errc::ProxyInvalid); }
    // 圧縮フラグ: 展開関数なし → UnsupportedFeature、恒等の展開関数なら（中身は無圧縮なので）通る
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.flags |= kFlagCompressedPages; PutHeader(b, h); Reseal(b);
      CHECK_ERR(Validate(b), Errc::UnsupportedFeature);
      ReadOptions o; o.decompress = [](const u8* s, u32 n, u8* d, u32 dn) { if (n != dn) return false; std::memcpy(d, s, n); return true; };
      CHECK(ValidateVgeo(MemorySource(b), o).ok()); }
    // strictReserved: 予約領域が非 0 でも通常は読める / 厳密モードでは拒否
    { std::vector<u8> b = base; VgeoHeader h = GetHeader(b); h.reserved1[3] = 7; h.reserved0 = 1; PutHeader(b, h); Reseal(b);
      CHECK(Validate(b, false).ok()); CHECK_ERR(Validate(b, true), Errc::ReservedNotZero); }
    // ローダ用の LoadMeta も同じエラーを返す（最初の 1 件）
    { std::vector<u8> b = base; b[0] = 'X'; VgeoMeta m; const VgeoError e = LoadMeta(MemorySource(b), m); CHECK(e.code == Errc::BadMagic); CHECK(!e.ok()); CHECK(e.ToString().find("BadMagic") != std::string::npos); }
    // ReadPage: 範囲外 / CRC
    { VgeoMeta m; CHECK(LoadMeta(MemorySource(base), m).ok()); std::vector<u8> pg(kPageSize);
      CHECK(ReadPage(MemorySource(base), m, 9999, pg.data()).code == Errc::PageInvalid);
      std::vector<u8> b = base; b[m.header.sections[kSecPages].offset + 77] ^= 1; CHECK(ReadPage(MemorySource(b), m, 0, pg.data()).code == Errc::PageCrcMismatch);
      ReadOptions o; o.verifyCrc = false; CHECK(ReadPage(MemorySource(b), m, 0, pg.data(), o).ok()); }
}

// ── 7. 境界値 ──────────────────────────────────────────────────────────────────
void TestBoundaries()
{
    // 0 クラスタ（VG ジオメトリ無し）: 最小ファイル = ヘッダ 1 セクタ
    {
        VgeoContent c;
        c.header.maxClusterVerts = 128; c.header.maxClusterTris = 128;
        c.header.posStep = 1.0f;
        const std::vector<u8> b = Bytes(c);
        CHECK(b.size() == 4096);
        CHECK(Validate(b, true, 8).ok());
        VgeoContent c2;
        CHECK(LoadContent(MemorySource(b), c2).ok() && c2.header.clusterCount == 0 && c2.pages.empty() && c2.nodes.empty());
        CHECK(Bytes(c2) == b);
        VgeoMeta m;
        CHECK(LoadMeta(MemorySource(b), m).ok() && m.pageDepOffsets.empty());
    }
    // 0 クラスタ + NONVG のみ（植生だけのアセット）
    {
        VgeoContent c;
        c.header.maxClusterVerts = 128; c.header.maxClusterTris = 128; c.header.posStep = 1.0f;
        StringPool pool;
        MaterialRecord m0 = MakeDefaultMaterial(); m0.nameOff = pool.Add("leaf"); m0.flags = kMatAlphaTest; m0.sectionKind = 1;
        c.materials.push_back(m0); c.strings = pool.Data();
        ProxySectionData ps; ps.materialIndex = 0;
        for (int v = 0; v < 3; ++v) { ProxyVertex pv{}; pv.position[0] = static_cast<f32>(v); pv.tangent[3] = 1.0f; ps.vertices.push_back(pv); }
        ps.indices = {0, 1, 2}; ps.aabbMax[0] = 2;
        c.nonvg.push_back(ps);
        const std::vector<u8> b = Bytes(c);
        auto rep = Validate(b, true, 8);
        CHECK(rep.ok());
        for (auto& i : rep.issues) std::printf("  issue: %s\n", i.ToString().c_str());
        VgeoContent c2; CHECK(LoadContent(MemorySource(b), c2).ok() && c2.nonvg.size() == 1 && Bytes(c2) == b);
        // NONVG セクションが VG 材質を参照するのは不可
        c.materials[0].sectionKind = 0; c.materials[0].flags = 0;
        CHECK_ERR(Validate(Bytes(c)), Errc::ProxyInvalid);
    }
    // 1 クラスタ・1 ページ
    { const auto b = Bytes(MakeLod0(1, 4)); CHECK(Validate(b, true, 8).ok()); VgeoMeta m; CHECK(LoadMeta(MemorySource(b), m).ok() && m.header.pageCount == 1 && m.header.nodeCount == 1 && m.header.groupCount == 1); }
    // BVH の形（葉 1 / 2 / 4 / 5 / 16 / 17 / 1000）: 検証が通る = 子 > 親・全ノードが 1 回だけ参照・集約が正しい
    for (u32 groups : {1u, 2u, 4u, 5u, 16u, 17u, 1000u})
    {
        const auto b = Bytes(MakeLod0(groups, 1, false));
        auto rep = Validate(b, true, 8);
        CHECK(rep.ok());
        for (auto& i : rep.issues) std::printf("  groups=%u issue: %s\n", groups, i.ToString().c_str());
    }
    // 複数ページにまたがる（500 グループ ≒ 数ページ。ページ境界でグループが割れない）
    {
        const auto b = Bytes(MakeLod0(20000, 4, false));
        VgeoMeta m; CHECK(LoadMeta(MemorySource(b), m).ok());
        CHECK(m.header.pageCount >= 2);
        CHECK(Validate(b, true, 8).ok());
    }
    // 1 ページ 256 クラスタ（最大）: 最小クラスタ（3 頂点・1 三角形）を 15 ずつのグループで詰める
    {
        auto tiny = []() { ClusterSource c; c.q = {0, 0, 0, 1, 0, 0, 0, 0, 1}; c.normalOct = {EncodeOct16(0, 1, 0), EncodeOct16(0, 1, 0), EncodeOct16(0, 1, 0)}; c.uv = {0, 0, 1, 0, 0, 1}; c.tri = {0, 1, 2}; return c; };
        PageBuilder pb(0);
        u32 added = 0, groups = 0;
        for (;;)
        {
            std::vector<ClusterSource> g(15, tiny());
            const u32 n = std::min<u32>(15, 256 - added);
            if (n == 0) break;
            u32 first = 0;
            CHECK(pb.TryAddGroup(g.data(), n, &first) == PageBuilder::AddResult::Added && first == added);
            added += n; ++groups;
        }
        CHECK(added == 256 && pb.ClusterCount() == 256 && pb.GroupCount() == groups);
        std::vector<ClusterSource> one(1, tiny());
        CHECK(pb.TryAddGroup(one.data(), 1) == PageBuilder::AddResult::NoFit);   // 257 個目は入らない
        std::vector<u8> pages;
        pb.FinishAppend(pages, true);
        CHECK(pages.size() == kPageSize);
        const PageHeader ph = ReadPageHeader(pages.data());
        CHECK(ph.clusterCount == 256 && ph.usedBytes <= kPageSize && ph.payloadOffset == 64 + 128 * 256);
    }
    // 最大クラスタ（128 頂点・128 三角形・位置 24+24+24bit）と、ページを目一杯まで詰める
    {
        VgeoContent c;
        c.header.maxClusterVerts = 128; c.header.maxClusterTris = 128;
        c.header.aabbMin[0] = c.header.aabbMin[1] = c.header.aabbMin[2] = 0; c.header.posOrigin[0] = c.header.posOrigin[1] = c.header.posOrigin[2] = 0;
        const f32 step = ComputePosStep(16777215.0);
        CHECK(step == 1.0f);
        c.header.aabbMax[0] = c.header.aabbMax[1] = c.header.aabbMax[2] = 16777215.0f;
        c.header.posStep = step;
        StringPool pool; c.materials.push_back(MakeMat(pool.Add("m"), kNone)); c.strings = pool.Data();
        auto big = [&](u32 seed) {
            ClusterSource k;
            g_rng = seed;
            for (u32 v = 0; v < 128; ++v)
            {
                const u32 xs[3] = {(v == 0) ? 0u : (v == 1) ? kGridMax : Rnd() % (kGridMax + 1), (v == 0) ? 0u : (v == 1) ? kGridMax : Rnd() % (kGridMax + 1), (v == 0) ? 0u : (v == 1) ? kGridMax : Rnd() % (kGridMax + 1)};
                k.q.insert(k.q.end(), xs, xs + 3);
                k.normalOct.push_back(EncodeOct16(Rnd01() - 0.5, Rnd01() - 0.5, Rnd01() - 0.5));
                k.uv.push_back(Rnd01() * 4); k.uv.push_back(Rnd01() * 4);
            }
            for (u32 t = 0; t < 128; ++t) { k.tri.push_back(static_cast<u8>(Rnd() % 128)); k.tri.push_back(static_cast<u8>(Rnd() % 128)); k.tri.push_back(static_cast<u8>(Rnd() % 128)); }
            // 巨大球（全点を含む）
            const f32 s[4] = {8388607.5f, 8388607.5f, 8388607.5f, 14600000.0f};
            std::memcpy(k.cullSphere, s, 16); std::memcpy(k.lodSphere, s, 16); std::memcpy(k.parentLodSphere, s, 16);
            k.maxEdgeLength = 1e7f;
            return k;
        };
        std::vector<BvhLeaf> leaves;
        PageBuilder pb(0);
        u32 pageIndex = 0;
        for (u32 i = 0; i < 100; ++i)
        {
            ClusterSource k = big(1000 + i);
            u32 first = 0;
            auto r = pb.TryAddGroup(&k, 1, &first);
            if (r == PageBuilder::AddResult::NoFit) { pb.FinishAppend(c.pages, true); pb.Reset(++pageIndex); r = pb.TryAddGroup(&k, 1, &first); }
            CHECK(r == PageBuilder::AddResult::Added);
            BvhLeaf lf; std::memcpy(lf.cullSphere, k.cullSphere, 16); std::memcpy(lf.lodSphere, k.lodSphere, 16); lf.minOwnError = 0; lf.groupId = i; lf.groupPacked = MakeGroupPacked(pageIndex, first, 1);
            leaves.push_back(lf);
        }
        pb.FinishAppend(c.pages, true);
        c.nodes = BuildBvh4(leaves);
        c.header.pinnedPageCount = pageIndex + 1;
        { std::vector<f32> sph; for (const HierChild& ch : c.nodes[0].child) if (ch.ref != kNone) sph.insert(sph.end(), ch.cullSphere, ch.cullSphere + 4);
          EnclosingSphere(sph.data(), static_cast<u32>(sph.size() / 4), c.header.boundingSphere); }
        CHECK(pageIndex >= 1);   // 1 ページ 48 個前後 → 100 個で 3 ページ
        const auto b = Bytes(c);
        auto rep = Validate(b, true, 8);
        CHECK(rep.ok());
        for (auto& i : rep.issues) std::printf("  issue: %s\n", i.ToString().c_str());
        // 位置が 24bit 格子の両端まで正確に往復する
        VgeoMeta m; CHECK(LoadMeta(MemorySource(b), m).ok());
        std::vector<u8> page(kPageSize); CHECK(ReadPage(MemorySource(b), m, 0, page.data()).ok());
        DecodedCluster dc; CHECK(DecodeCluster(page.data(), 0, m.header.posOrigin, m.header.posStep, dc));
        CHECK(dc.vertexCount == 128 && dc.triangleCount == 128 && ClusterBitsX(dc.h) == 24 && ClusterBitsY(dc.h) == 24 && ClusterBitsZ(dc.h) == 24);
        CHECK(dc.q[0] == 0 && dc.q[3] == kGridMax && dc.q[4] == kGridMax && dc.q[5] == kGridMax);
    }
    // エンコーダの入力検査（仕様外は Invalid）
    {
        auto mk = [](u32 vc, u32 tc) { ClusterSource c; for (u32 v = 0; v < vc; ++v) { c.q.insert(c.q.end(), {v, 0, 0}); c.normalOct.push_back(0); c.uv.push_back(0); c.uv.push_back(0); }
                                       for (u32 t = 0; t < tc; ++t) { c.tri.push_back(0); c.tri.push_back(1); c.tri.push_back(2); } return c; };
        PageBuilder pb(0);
        ClusterSource ok = mk(128, 128), tooManyV = mk(129, 4), tooManyT = mk(4, 129), noV = mk(0, 1), noT = mk(3, 0);
        CHECK(pb.TryAddGroup(&ok, 1) == PageBuilder::AddResult::Added);
        CHECK(pb.TryAddGroup(&tooManyV, 1) == PageBuilder::AddResult::Invalid);
        CHECK(pb.TryAddGroup(&tooManyT, 1) == PageBuilder::AddResult::Invalid);
        CHECK(pb.TryAddGroup(&noV, 1) == PageBuilder::AddResult::Invalid);
        CHECK(pb.TryAddGroup(&noT, 1) == PageBuilder::AddResult::Invalid);
        ClusterSource badIdx = mk(3, 1); badIdx.tri[2] = 3;   CHECK(pb.TryAddGroup(&badIdx, 1) == PageBuilder::AddResult::Invalid);
        ClusterSource badLvl = mk(3, 1); badLvl.level = 255;  CHECK(pb.TryAddGroup(&badLvl, 1) == PageBuilder::AddResult::Invalid);
        ClusterSource badQ = mk(3, 1); badQ.q[0] = kGridMax + 1; CHECK(pb.TryAddGroup(&badQ, 1) == PageBuilder::AddResult::Invalid);
        ClusterSource badMat = mk(3, 1); badMat.materialIndex = 70000; CHECK(pb.TryAddGroup(&badMat, 1) == PageBuilder::AddResult::Invalid);
        std::vector<ClusterSource> g16(16, mk(3, 1)); CHECK(pb.TryAddGroup(g16.data(), 16) == PageBuilder::AddResult::Invalid);   // 16 > 15
        CHECK(pb.TryAddGroup(g16.data(), 0) == PageBuilder::AddResult::Invalid);
        CHECK(pb.ClusterCount() == 1 && pb.GroupCount() == 1);   // Invalid は何も追加しない
    }
    // Writer の入力検査
    {
        VgeoContent c = MakeLod0(10, 4);
        c.pages.push_back(0);
        std::vector<u8> b; CHECK(WriteVgeoToMemory(c, b).code == Errc::PageInvalid);
        c = MakeLod0(10, 4); c.header.pinnedPageCount = 99; CHECK(WriteVgeoToMemory(c, b).code == Errc::CountMismatch);
        c = MakeLod0(10, 4); c.pages[0] = 'X'; CHECK(WriteVgeoToMemory(c, b).code == Errc::PageInvalid);
        c = MakeLod0(10, 4); c.header.maxClusterVerts = 200; CHECK(WriteVgeoToMemory(c, b).code == Errc::LimitExceeded);
    }
}

// ── 8. VGSRC ───────────────────────────────────────────────────────────────────
VgsrcData MakeVgsrc()
{
    VgsrcData d;
    d.positions = {0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 2, 0, 0};
    d.normals   = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
    d.uv0       = {0, 0, 1, 0, 1, 1, 0, 1, 2, 0};
    d.indices   = {0, 2, 1, 0, 3, 2, 1, 4, 2};
    d.sections  = {{0, 0, 6, 0}, {1, 6, 3, kVgsrcSectionForceNonVg}};
    StringPool pool;
    d.materials.push_back(MakeMat(pool.Add("a"), pool.Add("tex/a.dds")));
    d.materials.push_back(MakeMat(pool.Add("b"), kNone, 1));
    d.strings = pool.Data();
    d.unitScaleToMeters = 0.01f; d.coordSystem = kCoordUE;
    return d;
}

void TestVgsrc()
{
    VgsrcData d = MakeVgsrc();
    std::vector<u8> b;
    { VectorSink s(b); CHECK(WriteVgsrc(d, s).ok()); }
    CHECK(std::memcmp(b.data(), "VGSR", 4) == 0);
    VgsrcHeader h; std::memcpy(&h, b.data(), sizeof h);
    CHECK(h.vertexCount == 5 && h.indexCount == 9 && h.sectionCount == 2 && h.materialCount == 2 && h.sourceHash != 0);
    const VgsrcLayout l = ComputeVgsrcLayoutUpToStrings(h);
    CHECK(l.positions == 64 && l.normals % 16 == 0 && l.uv0 % 16 == 0 && l.indices % 16 == 0 && l.sections % 16 == 0 && l.materials % 16 == 0 && l.strings % 16 == 0);
    CHECK(l.normals >= l.positions + 60 && l.uv0 >= l.normals + 60);
    VgsrcData r;
    CHECK(ReadVgsrc(MemorySource(b), r, true).ok());
    CHECK(r.positions == d.positions && r.normals == d.normals && r.uv0 == d.uv0 && r.indices == d.indices && r.strings == d.strings && r.sections.size() == 2);
    CHECK(r.coordSystem == kCoordUE && r.unitScaleToMeters == 0.01f && r.materials.size() == 2 && r.materials[1].sectionKind == 1);
    // 同じ入力 → 同じバイト（決定論）
    { std::vector<u8> b2; VectorSink s(b2); CHECK(WriteVgsrc(d, s).ok()); CHECK(b == b2); }
    // 法線 / UV なし（フラグ）
    { VgsrcData e = d; e.flags = 0; e.normals.clear(); e.uv0.clear(); std::vector<u8> b3; VectorSink s(b3); CHECK(WriteVgsrc(e, s).ok());
      VgsrcData r3; CHECK(ReadVgsrc(MemorySource(b3), r3).ok() && r3.normals.empty() && r3.uv0.empty() && r3.positions.size() == 15); }

    // 破損
    VgsrcData tmp;
    { std::vector<u8> t(b.begin(), b.begin() + 30); CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::Truncated); }
    { std::vector<u8> t = b; t[0] = 'X'; CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::BadMagic); }
    { std::vector<u8> t = b; t[4] = 9; CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::UnsupportedMajor); }
    { std::vector<u8> t = b; t[8] |= 0x80; CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::UnsupportedFeature); }
    { std::vector<u8> t = b; t[44] = 5; CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::UnsupportedFeature); }                     // coordSystem
    { std::vector<u8> t = b; t[32] = 8; CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::CountMismatch); }                          // indexCount % 3 != 0
    { std::vector<u8> t(b.begin(), b.end() - 5); CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::Truncated); }                     // 文字列プールが切れた
    { std::vector<u8> t = b; t.push_back(0); CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::SectionRange); }                      // 末尾ゴミ
    { std::vector<u8> t = b; t[l.positions + 3] ^= 0x7F; t[l.positions + 2] ^= 0x80;
      CHECK(ReadVgsrc(MemorySource(t), tmp, true).code == Errc::SectionCrcMismatch); }                                                  // ハッシュ不一致
    { std::vector<u8> t = b; const u32 bad = 77; std::memcpy(t.data() + l.indices, &bad, 4); CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::ClusterInvalid); }   // index >= vertexCount
    { std::vector<u8> t = b; const f32 nan = std::nanf(""); std::memcpy(t.data() + l.positions, &nan, 4); CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::BadFloat); }
    { VgsrcData e = d; e.sections[1].firstIndex = 7; std::vector<u8> t; VectorSink s(t); WriteVgsrc(e, s); CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::SectionShape); }   // 隙間
    { VgsrcData e = d; e.sections[1].materialIndex = 5; std::vector<u8> t; VectorSink s(t); WriteVgsrc(e, s); CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::MaterialInvalid); }
    { VgsrcData e = d; e.sections.pop_back(); std::vector<u8> t; VectorSink s(t); WriteVgsrc(e, s); CHECK(ReadVgsrc(MemorySource(t), tmp).code == Errc::SectionShape); }   // 全体を覆わない

    // 座標変換: UE (X前,Y右,Z上, cm) → エンジン (x=Y, y=Z, z=X) × 0.01。巡回置換なので手系・巻き順は変わらない。
    f32 out[3];
    const f32 ue[3] = {100.0f, 200.0f, 300.0f};   // 前 1m・右 2m・上 3m
    ConvertPositionToEngine(kCoordUE, 0.01f, ue, out);
    CHECK(std::fabs(out[0] - 2.0f) < 1e-5f && std::fabs(out[1] - 3.0f) < 1e-5f && std::fabs(out[2] - 1.0f) < 1e-5f);
    ConvertPositionToEngine(kCoordEngine, 1.0f, ue, out);
    CHECK(out[0] == 100.0f && out[1] == 200.0f && out[2] == 300.0f);
    {   // 巻き順保存: 三角形の符号付き体積（原点基準）が変換で不変
        const f32 A[3] = {1, 0, 0}, B[3] = {0, 1, 0}, C[3] = {0, 0, 1};
        auto vol = [](const f32* a, const f32* b, const f32* c) { return a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]); };
        f32 a2[3], b2[3], c2[3];
        ConvertPositionToEngine(kCoordUE, 1.0f, A, a2); ConvertPositionToEngine(kCoordUE, 1.0f, B, b2); ConvertPositionToEngine(kCoordUE, 1.0f, C, c2);
        CHECK(vol(A, B, C) > 0 && vol(a2, b2, c2) > 0);
        f32 n[3]; const f32 nin[3] = {0, 0, 1}; ConvertNormalToEngine(kCoordUE, nin, n); CHECK(n[1] == 1.0f && n[0] == 0.0f && n[2] == 0.0f);
    }
}

// ── 9. ファズ: 壊れた入力でクラッシュ / 無限ループ / 巨大確保をしない（ローダは未署名の平置きファイルを読む）────
void TestFuzz()
{
    const std::vector<std::vector<u8>> bases = {Bytes(MakeHandDag()), Bytes(MakeLod0(200, 4)), Bytes(MakeLod0(40, 15))};
    u32 rejected = 0, accepted = 0;
    for (const auto& base : bases)
    {
        const VgeoHeader bh = GetHeader(base);
        const u64 smallEnd = bh.sections[kSecPages].offset;           // ヘッダ + 小セクション（材質 / 文字列 / ノード / ページ表 / 依存表）
        for (int it = 0; it < g_fuzzIters; ++it)
        {
            std::vector<u8> b = base;
            const int nmut = 1 + static_cast<int>(Rnd() % 6);
            for (int k = 0; k < nmut; ++k)
            {
                const bool small = (Rnd() % 3) != 0;
                const u64 pos = small ? (Rnd() % (kHeaderSize + 6000)) + (Rnd() % 2 ? 0 : (Rnd() % (smallEnd > 6000 ? smallEnd - 6000 : 1))) : Rnd() % b.size();
                if (pos >= b.size()) continue;
                switch (Rnd() % 4) { case 0: b[pos] ^= static_cast<u8>(1u << (Rnd() % 8)); break; case 1: b[pos] = static_cast<u8>(Rnd()); break; case 2: b[pos] = 0; break; default: b[pos] = 0xFF; break; }
            }
            if (Rnd() % 8 == 0) b.resize(Rnd() % b.size());
            if (Rnd() % 3 == 0) Reseal(b);                              // CRC を通して構造検査の奥まで届かせる
            ReadOptions o; o.maxIssues = 1 + Rnd() % 6; o.verifyCrc = (Rnd() % 2) != 0; o.deep = (Rnd() % 4) != 0; o.strictReserved = (Rnd() % 4) == 0;
            if (g_trace)
            {
                std::fprintf(stderr, "it=%d base=%d opt: maxIssues=%u crc=%d deep=%d strict=%d\n", it, static_cast<int>(&base - &bases[0]), o.maxIssues, o.verifyCrc, o.deep, o.strictReserved);
                std::fflush(stderr);
                std::ofstream f((std::filesystem::temp_directory_path() / "vgeo_fuzz_last.vgeo").string(), std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
            }
            const ValidationReport rep = ValidateVgeo(MemorySource(b), o);
            (rep.ok() ? accepted : rejected)++;
            VgeoContent c; (void)LoadContent(MemorySource(b), c, o);
            VgeoMeta m; (void)LoadMeta(MemorySource(b), m, o);
        }
    }
    CHECK(rejected > 0);
    std::printf("  fuzz: %u rejected, %u accepted (mutation was harmless)\n", rejected, accepted);
    // VGSRC
    std::vector<u8> vb; { VectorSink s(vb); VgsrcData d = MakeVgsrc(); (void)WriteVgsrc(d, s); }
    for (int it = 0; it < 3000; ++it)
    {
        std::vector<u8> b = vb;
        for (u32 k = 0, n = 1 + Rnd() % 4; k < n; ++k) b[Rnd() % b.size()] = static_cast<u8>(Rnd());
        if (Rnd() % 6 == 0) b.resize(Rnd() % b.size());
        VgsrcData d; (void)ReadVgsrc(MemorySource(b), d, (Rnd() % 2) != 0);
    }
    CHECK(true);
}

} // namespace

int main(int argc, char** argv)
{
    // 引数: 数字 = ファズの反復数（既定 2500。ASAN で長く回す用）/ trace = 各反復を stderr へ出し入力を temp に書く（クラッシュした入力の特定用）
    if (argc > 1) { if (std::string(argv[1]) == "trace") g_trace = true; else g_fuzzIters = std::max(1, std::atoi(argv[1])); }
    TestLayoutAndConstants();
    TestCodecs();
    TestRoundTrip();
    TestDeterminism();
    TestHandDag();
    TestDagViolations();
    TestCorruption();
    TestBoundaries();
    TestVgsrc();
    TestFuzz();
    std::printf("vgeo_format_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
