// vgeo_stub ― 合成メッシュから LOD0 のみの `.vgeo` を書き出す小さな CLI（仮想ジオメトリ P0）。
//
//   vgeo_stub --kind sphere|torus|grid|blob --tris N --out FILE.vgeo [--seed S] [--proxy-tris N] [--no-proxy]
//             [--group-size G] [--max-verts V] [--max-tris T] [--label NAME]
//   vgeo_stub --validate FILE.vgeo [--no-deep] [--no-crc] [--strict]     # 検証だけ（所要時間を出す）
//   vgeo_stub --info FILE.vgeo                                           # ヘッダの要約
//   vgeo_stub --emit-vgsrc FILE.vgsrc --kind sphere --tris N [--ue]      # VGSRC の見本（P6 の突き合わせ用。--ue で UE 座標・cm）
//   vgeo_stub --validate-vgsrc FILE.vgsrc                                # VGSRC の検証（ハッシュ込み）
//
// 出力は決定的（同じ引数なら同じバイト列）。終了コード: 0 = 成功 / 1 = 引数エラー / 2 = 生成・検証の失敗。
// 合成データ専用。実アセット・UE 由来の資産は扱わない。
#include "VgeoStubGen.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace dx12e::vg;

namespace
{
double MsSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int Usage()
{
    std::fprintf(stderr,
        "usage:\n"
        "  vgeo_stub --kind sphere|torus|grid|blob --tris N --out FILE.vgeo [--seed S] [--proxy-tris N] [--no-proxy]\n"
        "            [--group-size G] [--max-verts V] [--max-tris T] [--label NAME]\n"
        "  vgeo_stub --validate FILE.vgeo [--no-deep] [--no-crc] [--strict]\n"
        "  vgeo_stub --info FILE.vgeo\n"
        "  vgeo_stub --emit-vgsrc FILE.vgsrc --kind sphere --tris N [--ue]   (VGSRC sample for P6; --ue = UE coordinates in cm)\n"
        "  vgeo_stub --validate-vgsrc FILE.vgsrc\n");
    return 1;
}

int Validate(const std::string& path, const ReadOptions& opt)
{
    FileSource src;
    if (!src.Open(path)) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }
    const auto t0 = std::chrono::steady_clock::now();
    const ValidationReport rep = ValidateVgeo(src, opt);
    const double ms = MsSince(t0);
    std::printf("validate: %s  %.1f ms  (%.1f MB, deep=%d crc=%d strict=%d)\n", rep.ok() ? "OK" : "FAILED", ms,
                static_cast<double>(src.Size()) / (1024.0 * 1024.0), opt.deep ? 1 : 0, opt.verifyCrc ? 1 : 0, opt.strictReserved ? 1 : 0);
    for (const auto& i : rep.issues) std::printf("  %s\n", i.ToString().c_str());
    return rep.ok() ? 0 : 2;
}

// VGSRC を検証する（ハッシュも確認）。P6 の C# が書いたファイルの突き合わせ用。全体をメモリへ読むので小〜中規模向け。
int ValidateVgsrcFile(const std::string& path)
{
    FileSource src;
    if (!src.Open(path)) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }
    VgsrcData d;
    const auto t0 = std::chrono::steady_clock::now();
    const VgeoError e = ReadVgsrc(src, d, true);
    const double ms = MsSince(t0);
    if (!e.ok()) { std::printf("vgsrc: FAILED  %s\n", e.ToString().c_str()); return 2; }
    std::printf("vgsrc: OK  %.1f ms  vertices %zu  triangles %zu  sections %zu  materials %zu  coordSystem %u  unitScale %g  flags 0x%X\n", ms, d.positions.size() / 3,
                d.indices.size() / 3, d.sections.size(), d.materials.size(), d.coordSystem, static_cast<double>(d.unitScaleToMeters), d.flags);
    return 0;
}

int Info(const std::string& path)
{
    FileSource src;
    if (!src.Open(path)) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }
    VgeoMeta m;
    const auto t0 = std::chrono::steady_clock::now();
    const VgeoError e = LoadMeta(src, m);
    const double msMeta = MsSince(t0);
    if (!e.ok()) { std::printf("error: %s\n", e.ToString().c_str()); return 2; }
    const VgeoHeader& h = m.header;
    std::printf("vgeo v%u.%u  cooker=\"%s\"  (LoadMeta %.2f ms)\n", h.versionMajor, h.versionMinor, h.cooker, msMeta);
    std::printf("  file %.2f MB  pages %u  clusters %u  groups %u  nodes %u  levels %u  roots %u  pinned %u\n",
                static_cast<double>(m.fileSize) / (1024.0 * 1024.0), h.pageCount, h.clusterCount, h.groupCount, h.nodeCount, h.levelCount, h.rootClusterCount, h.pinnedPageCount);
    std::printf("  source tris %llu  verts %llu  file bytes/tri %.2f  page bytes/tri %.2f\n", static_cast<unsigned long long>(h.sourceTriangleCount), static_cast<unsigned long long>(h.sourceVertexCount),
                h.sourceTriangleCount ? static_cast<double>(m.fileSize) / static_cast<double>(h.sourceTriangleCount) : 0.0,
                h.sourceTriangleCount ? static_cast<double>(h.pageCount) * kPageSize / static_cast<double>(h.sourceTriangleCount) : 0.0);
    std::printf("  aabb (%.3f %.3f %.3f) - (%.3f %.3f %.3f)  posStep %.3g  proxy %u sections (error %.4g)  nonvg %u\n", h.aabbMin[0], h.aabbMin[1], h.aabbMin[2],
                h.aabbMax[0], h.aabbMax[1], h.aabbMax[2], h.posStep, h.proxySectionCount, h.proxyError, h.nonVgSectionCount);
    // 統計: 平均の位置ビット数 / ページ充填率（ページを 1 枚ずつ読む）
    std::vector<u8> page(kPageSize);
    double bppSum = 0, vertSum = 0, used = 0;
    for (u32 p = 0; p < h.pageCount; ++p)
    {
        if (!ReadPage(src, m, p, page.data(), ReadOptions{}).ok()) return 2;
        const PageHeader ph = ReadPageHeader(page.data());
        used += ph.usedBytes;
        for (u32 i = 0; i < ph.clusterCount; ++i)
        {
            const ClusterHeader ch = ReadClusterHeader(page.data(), i);
            bppSum += static_cast<double>(ClusterBitsX(ch) + ClusterBitsY(ch) + ClusterBitsZ(ch)) * ClusterVertexCount(ch);
            vertSum += ClusterVertexCount(ch);
        }
    }
    if (h.pageCount) std::printf("  avg position bits/vertex %.1f  page fill %.1f%%\n", vertSum > 0 ? bppSum / vertSum : 0.0, 100.0 * used / (static_cast<double>(h.pageCount) * kPageSize));
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    std::string kind = "sphere", out, validate, info, label = "stub", validateVgsrc, emitVgsrc;
    bool ueCoords = false;
    unsigned long long tris = 10000;
    unsigned seed = 1;
    stub::StubOptions so;
    ReadOptions ro;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--kind" && (v = next())) kind = v;
        else if (a == "--tris" && (v = next())) tris = std::strtoull(v, nullptr, 10);
        else if (a == "--out" && (v = next())) out = v;
        else if (a == "--seed" && (v = next())) seed = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        else if (a == "--proxy-tris" && (v = next())) so.proxyMaxTris = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (a == "--no-proxy") so.proxy = false;
        else if (a == "--group-size" && (v = next())) so.groupSize = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (a == "--max-verts" && (v = next())) so.maxVerts = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (a == "--max-tris" && (v = next())) so.maxTris = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (a == "--label" && (v = next())) label = v;
        else if (a == "--validate" && (v = next())) validate = v;
        else if (a == "--info" && (v = next())) info = v;
        else if (a == "--validate-vgsrc" && (v = next())) validateVgsrc = v;
        else if (a == "--emit-vgsrc" && (v = next())) emitVgsrc = v;
        else if (a == "--ue") ueCoords = true;
        else if (a == "--no-deep") ro.deep = false;
        else if (a == "--no-crc") ro.verifyCrc = false;
        else if (a == "--strict") ro.strictReserved = true;
        else return Usage();
    }
    if (!validate.empty()) return Validate(validate, ro);
    if (!info.empty()) return Info(info);
    if (!validateVgsrc.empty()) return ValidateVgsrcFile(validateVgsrc);
    if ((out.empty() && emitVgsrc.empty()) || tris == 0) return Usage();
    so.label = label;

    const auto t0 = std::chrono::steady_clock::now();
    stub::MeshData mesh;
    if (!stub::MakeMeshByName(kind, tris, seed, mesh)) { std::fprintf(stderr, "unknown --kind %s\n", kind.c_str()); return 1; }
    const double msGen = MsSince(t0);

    if (!emitVgsrc.empty())
    {
        // VGSRC の見本を書く（P6 の C# 実装の突き合わせ用）。--ue で UE 座標（Z up・cm）にして出す。
        FileSink fs;
        if (!fs.Open(emitVgsrc)) { std::fprintf(stderr, "cannot open %s for write\n", emitVgsrc.c_str()); return 2; }
        const VgeoError ve = WriteVgsrc(stub::MeshToVgsrc(mesh, ueCoords, label), fs);
        if (!fs.Close() || !ve.ok()) { std::fprintf(stderr, "vgsrc write failed: %s\n", ve.ToString().c_str()); return 2; }
        std::printf("wrote %s (VGSRC, %s, %llu tris, %u verts)\n", emitVgsrc.c_str(), ueCoords ? "UE coords cm" : "engine coords m",
                    static_cast<unsigned long long>(mesh.triangleCount()), mesh.vertexCount());
        if (out.empty()) return 0;
    }

    VgeoContent content;
    stub::StubStats st;
    VgeoError e = stub::BuildStubContent(mesh, so, content, &st);
    if (!e.ok()) { std::fprintf(stderr, "build failed: %s\n", e.ToString().c_str()); return 2; }

    const auto tw = std::chrono::steady_clock::now();
    e = WriteVgeoToFile(content, out);
    const double msWrite = MsSince(tw);
    if (!e.ok()) { std::fprintf(stderr, "write failed: %s\n", e.ToString().c_str()); return 2; }

    std::printf("wrote %s\n", out.c_str());
    std::printf("  kind=%s tris=%llu verts=%llu -> clusters=%u groups=%u pages=%u nodes=%u  (%.1f tri/cluster, %.1f vert/cluster)\n", kind.c_str(),
                static_cast<unsigned long long>(st.sourceTris), static_cast<unsigned long long>(st.sourceVerts), st.clusters, st.groups, st.pages, st.nodes,
                st.avgTrisPerCluster, st.avgVertsPerCluster);
    std::printf("  time ms: meshgen %.0f  quantize %.0f  meshlets %.0f  pack+bvh %.0f  proxy %.0f  write %.0f  | build total %.0f  (%.2f Mtri/s)\n", msGen, st.msQuantize,
                st.msMeshlets, st.msPack, st.msProxy, msWrite, st.msTotal + msWrite,
                (st.msTotal + msWrite) > 0 ? static_cast<double>(st.sourceTris) / 1e6 / ((st.msTotal + msWrite) / 1000.0) : 0.0);
    return 0;
}
