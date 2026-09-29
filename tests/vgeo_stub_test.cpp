// スタブ `.vgeo` 生成器（tools/vgeo_stub/VgeoStubGen.h）のテスト。
//
//   ・球 / トーラス / グリッド / 岩(blob) の出力が VgeoFormat の検証（厳密モード・深い検査つき）を通る
//   ・書く → 読む → 書くが同一バイト、同じ入力 → 同じバイト列（決定論）
//   ・デコードした三角形の集合が入力メッシュ（量子化後）と一致する（欠落・重複・巻き順の反転が無い）
//   ・LOD0 のみの規約: 全クラスタが lod0 + root、parentLodError = +INF、DAG ノードは葉だけ
//   ・法線コーンの向き（クラスタ中心から外向き）＝ 巻き順規約 cross(p1-p0, p2-p0) が外向き
//   ・プロキシ（PROXY セクション）の三角形数上限と誤差、オプション違反のエラー
// meshoptimizer に依存するので統合ビルドでのみビルドされる（tests/CMakeLists.txt）。純粋な形式のテストは vgeo_format_test.cpp。

#include "VgeoStubGen.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace dx12e::vg;
using namespace dx12e::vg::stub;

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

// 1 三角形 = 3 頂点の格子座標(u32 x3)。巻き順を保ったまま「最小の頂点から始まる」回転へ正規化する。
using Tri = std::array<u32, 9>;
Tri Canon(const u32 a[3], const u32 b[3], const u32 c[3])
{
    const u32* v[3] = {a, b, c};
    auto less = [](const u32* p, const u32* q) { return std::lexicographical_compare(p, p + 3, q, q + 3); };
    int start = 0;
    for (int i = 1; i < 3; ++i) if (less(v[i], v[start])) start = i;
    Tri t{};
    for (int k = 0; k < 3; ++k) std::memcpy(&t[k * 3], v[(start + k) % 3], 12);
    return t;
}

f64 SignedVolume(const MeshData& m)
{
    f64 vol = 0;
    for (size_t t = 0; t < m.idx.size(); t += 3)
    {
        const f32* p0 = &m.pos[static_cast<size_t>(m.idx[t]) * 3];
        const f32* p1 = &m.pos[static_cast<size_t>(m.idx[t + 1]) * 3];
        const f32* p2 = &m.pos[static_cast<size_t>(m.idx[t + 2]) * 3];
        vol += p0[0] * (p1[1] * p2[2] - p1[2] * p2[1]) - p0[1] * (p1[0] * p2[2] - p1[2] * p2[0]) + p0[2] * (p1[0] * p2[1] - p1[1] * p2[0]);
    }
    return vol / 6.0;
}

std::vector<u8> WriteBytes(const VgeoContent& c)
{
    std::vector<u8> b;
    const VgeoError e = WriteVgeoToMemory(c, b);
    CHECK(e.ok());
    return b;
}

// 入力メッシュを量子化格子に載せた三角形の多重集合 と、.vgeo をデコードした三角形の多重集合を比べる。
void CheckGeometry(const MeshData& mesh, const std::vector<u8>& bytes, const char* name)
{
    VgeoMeta m;
    CHECK(LoadMeta(MemorySource(bytes), m).ok());
    const VgeoHeader& h = m.header;
    std::vector<u32> q(static_cast<size_t>(mesh.vertexCount()) * 3);
    for (size_t v = 0; v < mesh.vertexCount(); ++v)
        for (int a = 0; a < 3; ++a) q[v * 3 + a] = QuantizeCoord(mesh.pos[v * 3 + a], h.posOrigin[a], h.posStep);
    std::map<Tri, int> expect;
    for (size_t t = 0; t < mesh.idx.size(); t += 3) ++expect[Canon(&q[static_cast<size_t>(mesh.idx[t]) * 3], &q[static_cast<size_t>(mesh.idx[t + 1]) * 3], &q[static_cast<size_t>(mesh.idx[t + 2]) * 3])];
    std::map<Tri, int> got;
    u64 decodedTris = 0;
    f64 worstNormalErr = 0;
    std::vector<u8> page(kPageSize);
    for (u32 p = 0; p < h.pageCount; ++p)
    {
        CHECK(ReadPage(MemorySource(bytes), m, p, page.data()).ok());
        const PageHeader ph = ReadPageHeader(page.data());
        for (u32 i = 0; i < ph.clusterCount; ++i)
        {
            DecodedCluster dc;
            CHECK(DecodeCluster(page.data(), i, h.posOrigin, h.posStep, dc));
            for (u32 t = 0; t < dc.triangleCount; ++t)
            {
                const u32 a = dc.tri[t * 3], b = dc.tri[t * 3 + 1], c = dc.tri[t * 3 + 2];
                ++got[Canon(&dc.q[a * 3], &dc.q[b * 3], &dc.q[c * 3])];
                ++decodedTris;
            }
            // 法線は単位長
            for (u32 v = 0; v < dc.vertexCount; ++v)
            {
                const f32* n = &dc.normal[v * 3];
                worstNormalErr = std::max(worstNormalErr, std::fabs(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0));
            }
        }
    }
    if (expect != got) std::printf("  %s: triangle multiset differs (expected %zu distinct, got %zu distinct)\n", name, expect.size(), got.size());
    CHECK(expect == got);
    CHECK(decodedTris == mesh.triangleCount());
    CHECK(worstNormalErr < 1e-4);
}

void CheckStubInvariants(const VgeoContent& c, const StubStats& st, const char* name)
{
    std::vector<u8> b = WriteBytes(c);
    const ValidationReport rep = [&] { ReadOptions o; o.strictReserved = true; o.maxIssues = 8; return ValidateVgeo(MemorySource(b), o); }();
    if (!rep.ok()) for (auto& i : rep.issues) std::printf("  %s: %s\n", name, i.ToString().c_str());
    CHECK(rep.ok());
    VgeoMeta m;
    CHECK(LoadMeta(MemorySource(b), m).ok());
    const VgeoHeader& h = m.header;
    CHECK(h.clusterCount == st.clusters && h.groupCount == st.groups && h.pageCount == st.pages && h.nodeCount == st.nodes);
    CHECK(h.levelCount == 1 && h.rootClusterCount == h.clusterCount && h.pinnedPageCount == h.pageCount);
    CHECK(h.sourceTriangleCount == st.sourceTris && h.sourceVertexCount == st.sourceVerts);
    CHECK(m.pageDeps.empty());                                       // LOD0 のみ = 依存なし
    // 全クラスタが lod0 + root、親なし
    std::vector<u8> page(kPageSize);
    u32 maxV = 0, maxT = 0;
    for (u32 p = 0; p < h.pageCount; ++p)
    {
        CHECK(ReadPage(MemorySource(b), m, p, page.data()).ok());
        const PageHeader ph = ReadPageHeader(page.data());
        for (u32 i = 0; i < ph.clusterCount; ++i)
        {
            const ClusterHeader ch = ReadClusterHeader(page.data(), i);
            CHECK((ch.flags & kClusterFlagLod0) && (ch.flags & kClusterFlagRoot) && ClusterLevel(ch) == 0);
            CHECK(IsPosInf(ch.parentLodError) && ch.lodError == 0.0f && ch.childPage == kNone && ch.childGroup == kNone);
            maxV = std::max(maxV, ClusterVertexCount(ch)); maxT = std::max(maxT, ClusterTriangleCount(ch));
        }
    }
    CHECK(maxV <= h.maxClusterVerts && maxT <= h.maxClusterTris);
    // DAG ノードは「葉だけ・親なし」= 全部の葉が root グループ
    u32 leaves = 0;
    for (const HierNode& n : m.nodes)
        for (const HierChild& ch : n.child)
            if (ch.ref != kNone && NodeRefIsLeaf(ch.ref)) { ++leaves; CHECK(ch.minOwnError == 0.0f && IsPosInf(ch.maxParentError)); }
    CHECK(leaves == h.groupCount);
    // 往復
    VgeoContent c2;
    CHECK(LoadContent(MemorySource(b), c2).ok());
    CHECK(WriteBytes(c2) == b);
}

void RunKind(const char* kind, u64 tris, u32 seed)
{
    MeshData mesh;
    CHECK(MakeMeshByName(kind, tris, seed, mesh));
    std::printf("  %s: requested %llu tris -> %llu tris, %u verts\n", kind, static_cast<unsigned long long>(tris), static_cast<unsigned long long>(mesh.triangleCount()), mesh.vertexCount());
    CHECK(mesh.triangleCount() >= tris * 9 / 10 && mesh.triangleCount() <= tris * 11 / 10 + 8);
    for (size_t v = 0; v < mesh.vertexCount(); ++v)
    {
        const f32* n = &mesh.nrm[v * 3];
        CHECK(std::fabs(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0f) < 1e-3f);
    }
    if (std::string(kind) == "sphere" || std::string(kind) == "torus" || std::string(kind) == "blob") CHECK(SignedVolume(mesh) > 0);   // 外向きの巻き順

    StubOptions opt;
    VgeoContent c;
    StubStats st;
    const VgeoError e = BuildStubContent(mesh, opt, c, &st);
    if (!e.ok()) std::printf("  build error: %s\n", e.ToString().c_str());
    CHECK(e.ok());
    CheckStubInvariants(c, st, kind);
    CheckGeometry(mesh, WriteBytes(c), kind);
    CHECK(st.avgTrisPerCluster >= 40.0 && st.avgTrisPerCluster <= 128.0);
    std::printf("    clusters %u  groups %u  pages %u  nodes %u  %.1f tri/cluster  %.1f vert/cluster  file %zu B (%.1f B/tri)\n", st.clusters, st.groups, st.pages, st.nodes,
                st.avgTrisPerCluster, st.avgVertsPerCluster, WriteBytes(c).size(), static_cast<double>(WriteBytes(c).size()) / static_cast<double>(mesh.triangleCount()));
    // 決定論
    VgeoContent c2;
    CHECK(BuildStubContent(mesh, opt, c2).ok());
    CHECK(WriteBytes(c) == WriteBytes(c2));
    // 法線コーン: 外向きの立体では、有効なコーンの軸はクラスタ中心から外向き（巻き順規約の検証）
    if (std::string(kind) == "sphere" || std::string(kind) == "torus")
    {
        VgeoMeta m; std::vector<u8> b = WriteBytes(c);
        CHECK(LoadMeta(MemorySource(b), m).ok());
        std::vector<u8> page(kPageSize);
        u32 valid = 0, outward = 0;
        for (u32 p = 0; p < m.header.pageCount; ++p)
        {
            CHECK(ReadPage(MemorySource(b), m, p, page.data()).ok());
            const PageHeader ph = ReadPageHeader(page.data());
            for (u32 i = 0; i < ph.clusterCount; ++i)
            {
                const ClusterHeader ch = ReadClusterHeader(page.data(), i);
                if (ConeByte(ch.coneS8, 3) >= 127) continue;   // コーン無効
                ++valid;
                const f32 ax = ConeByte(ch.coneS8, 0) / 127.0f, ay = ConeByte(ch.coneS8, 1) / 127.0f, az = ConeByte(ch.coneS8, 2) / 127.0f;
                // 球: 中心 = 原点なので、外向きの軸は cullSphere 中心方向と同じ向き。トーラスは管の中心線からの外向きで判定する。
                f32 cx = ch.cullSphere[0], cy = ch.cullSphere[1], cz = ch.cullSphere[2];
                if (std::string(kind) == "torus") { const f32 l = std::sqrt(cx * cx + cz * cz); if (l > 1e-4f) { cx -= cx / l; cz -= cz / l; } }
                if (ax * cx + ay * cy + az * cz > 0) ++outward;
            }
        }
        CHECK(valid > 0);
        CHECK(outward * 100 >= valid * 95);   // 95% 以上が外向き（トーラスの内側の狭い帯は許容）
    }
}

void TestOptionsAndErrors()
{
    MeshData mesh = MakeSphere(2000);
    VgeoContent c;
    { StubOptions o; o.maxTris = 129; CHECK(BuildStubContent(mesh, o, c).code == Errc::LimitExceeded); }
    { StubOptions o; o.maxTris = 130; CHECK(BuildStubContent(mesh, o, c).code == Errc::LimitExceeded); }
    { StubOptions o; o.maxVerts = 129; CHECK(BuildStubContent(mesh, o, c).code == Errc::LimitExceeded); }
    { StubOptions o; o.groupSize = 16; CHECK(BuildStubContent(mesh, o, c).code == Errc::LimitExceeded); }
    { StubOptions o; o.groupSize = 0; CHECK(BuildStubContent(mesh, o, c).code == Errc::LimitExceeded); }
    { MeshData e; CHECK(BuildStubContent(e, StubOptions{}, c).code == Errc::CountMismatch); }
    { MeshData bad = mesh; bad.idx[3] = bad.vertexCount() + 5; CHECK(BuildStubContent(bad, StubOptions{}, c).code == Errc::ClusterInvalid); }
    { MeshData bad = mesh; bad.pos[4] = std::nanf(""); CHECK(BuildStubContent(bad, StubOptions{}, c).code == Errc::BadFloat); }
    { MeshData bad = mesh; bad.nrm.pop_back(); CHECK(BuildStubContent(bad, StubOptions{}, c).code == Errc::CountMismatch); }
    { MeshData none; CHECK(!MakeMeshByName("cube", 100, 1, none)); }

    // 別の上限（NVIDIA 推奨の 64 頂点 / 124 三角形）と、グループサイズ 1 / 15
    for (auto vt : {std::pair<u32, u32>{64, 124}, std::pair<u32, u32>{128, 128}, std::pair<u32, u32>{32, 64}})
        for (u32 gs : {1u, 4u, 15u})
        {
            StubOptions o; o.maxVerts = vt.first; o.maxTris = vt.second; o.groupSize = gs;
            StubStats st;
            const VgeoError e = BuildStubContent(mesh, o, c, &st);
            CHECK(e.ok());
            if (!e.ok()) { std::printf("  %s\n", e.ToString().c_str()); continue; }
            CheckStubInvariants(c, st, "options");
            CHECK(c.header.maxClusterVerts == vt.first && c.header.maxClusterTris == vt.second);
            CheckGeometry(mesh, WriteBytes(c), "options");
        }
    // プロキシ無し / 上限つき
    {
        StubOptions o; o.proxy = false;
        StubStats st;
        CHECK(BuildStubContent(mesh, o, c, &st).ok());
        CHECK(c.proxy.empty());
        auto b = WriteBytes(c);
        CHECK(ValidateVgeo(MemorySource(b)).ok());
        VgeoMeta pm; CHECK(LoadMeta(MemorySource(b), pm).ok() && !(pm.header.flags & kFlagHasProxy) && pm.header.proxySectionCount == 0);
    }
    // 1 三角形 / 2 三角形の極小メッシュ
    {
        MeshData tri;
        tri.pos = {0, 0, 0, 1, 0, 0, 0, 1, 0}; tri.nrm = {0, 0, 1, 0, 0, 1, 0, 0, 1}; tri.uv = {0, 0, 1, 0, 0, 1}; tri.idx = {0, 1, 2};
        StubStats st;
        CHECK(BuildStubContent(tri, StubOptions{}, c, &st).ok());
        CHECK(st.clusters == 1 && st.groups == 1 && st.pages == 1 && st.nodes == 1);
        CheckStubInvariants(c, st, "1 tri");
        CheckGeometry(tri, WriteBytes(c), "1 tri");
        MeshData flat = MakeGrid(2);
        CHECK(BuildStubContent(flat, StubOptions{}, c, &st).ok());
        CheckStubInvariants(c, st, "flat grid");
        // 平面（Y 方向の広がりが小さい）でも格子の step が正しい
    }
    // 位置が 1 点に潰れた（extent 0）メッシュ → step = 1.0 で通る
    {
        MeshData deg;
        deg.pos = {5, 5, 5, 5, 5, 5, 5, 5, 5}; deg.nrm = {0, 1, 0, 0, 1, 0, 0, 1, 0}; deg.uv = {0, 0, 1, 0, 0, 1}; deg.idx = {0, 1, 2};
        StubStats st;
        CHECK(BuildStubContent(deg, StubOptions{}, c, &st).ok());
        CHECK(c.header.posStep == 1.0f);
        CheckStubInvariants(c, st, "degenerate");
    }
}

// プロキシ: 上限を超えるメッシュは簡略化され、誤差が正、フラグ Exact は立たない
void TestProxy()
{
    MeshData mesh = MakeBlob(120000, 7);
    StubOptions o; o.proxyMaxTris = 20000;
    VgeoContent c; StubStats st;
    CHECK(BuildStubContent(mesh, o, c, &st).ok());
    CHECK(c.proxy.size() == 1);
    CHECK(c.proxy[0].indices.size() / 3 <= 20000 + 200);
    CHECK(c.proxy[0].error > 0.0f && !(c.proxy[0].flags & kProxyFlagExact));
    CHECK(c.proxy[0].error < 0.5f);   // 半径 ~1 の岩に対して現実的な誤差
    auto b = WriteBytes(c);
    CHECK(ValidateVgeo(MemorySource(b)).ok());
    VgeoMeta m; CHECK(LoadMeta(MemorySource(b), m).ok());
    CHECK(m.header.proxyError == c.proxy[0].error);
    // 上限以下ならそのまま（Exact）
    MeshData small = MakeSphere(3000);
    CHECK(BuildStubContent(small, StubOptions{}, c, &st).ok());
    CHECK(c.proxy.size() == 1 && (c.proxy[0].flags & kProxyFlagExact) && c.proxy[0].error == 0.0f && c.proxy[0].indices.size() / 3 == small.triangleCount());
}

// VGSRC の見本: エンジン座標 / UE 座標（cm）の両方が読め、UE のほうは ConvertPositionToEngine で元の位置に戻る
void TestVgsrcSample()
{
    const MeshData mesh = MakeTorus(2000);
    for (bool ue : {false, true})
    {
        const VgsrcData d = MeshToVgsrc(mesh, ue);
        std::vector<u8> bytes;
        VectorSink sink(bytes);
        CHECK(WriteVgsrc(d, sink).ok());
        VgsrcData r;
        CHECK(ReadVgsrc(MemorySource(bytes), r, true).ok());
        CHECK(r.indices == mesh.idx && r.uv0 == mesh.uv && r.positions.size() == mesh.pos.size() && r.sections.size() == 1 && r.materials.size() == 1);
        CHECK(r.coordSystem == (ue ? kCoordUE : kCoordEngine) && r.unitScaleToMeters == (ue ? 0.01f : 1.0f));
        f32 worst = 0;
        for (size_t v = 0; v < mesh.vertexCount(); ++v)
        {
            f32 e[3], n[3];
            ConvertPositionToEngine(r.coordSystem, r.unitScaleToMeters, &r.positions[v * 3], e);
            ConvertNormalToEngine(r.coordSystem, &r.normals[v * 3], n);
            for (int a = 0; a < 3; ++a) worst = std::max(worst, std::max(std::fabs(e[a] - mesh.pos[v * 3 + a]), std::fabs(n[a] - mesh.nrm[v * 3 + a])));
        }
        CHECK(worst < 1e-5f);
        // 変換後の三角形の向きが変わらない（外向き）: 符号付き体積が正のまま
        MeshData conv = mesh;
        for (size_t v = 0; v < mesh.vertexCount(); ++v) ConvertPositionToEngine(r.coordSystem, r.unitScaleToMeters, &r.positions[v * 3], &conv.pos[v * 3]);
        CHECK(SignedVolume(conv) > 0);
    }
}

} // namespace

int main()
{
    // 種類ごとに（球・トーラス・グリッド・岩）
    RunKind("sphere", 5000, 1);
    RunKind("torus", 20000, 1);
    RunKind("grid", 20000, 1);
    RunKind("blob", 100000, 3);
    // blob は seed で形が変わり、同じ seed なら同じ
    { MeshData a = MakeBlob(4000, 1), b = MakeBlob(4000, 1), c = MakeBlob(4000, 2); CHECK(a.pos == b.pos && a.pos != c.pos && a.idx == b.idx); }
    TestOptionsAndErrors();
    TestProxy();
    TestVgsrcSample();
    std::printf("vgeo_stub_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
