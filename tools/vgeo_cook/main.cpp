// vgeo_cook ― 仮想ジオメトリ（Nanite 風）のオフライン cooker CLI（P1）。VGSRC / OBJ → 階層 LOD の完全な `.vgeo`。
//
//   vgeo_cook IN.vgsrc|IN.obj OUT.vgeo [オプション]           cook（書いたファイルを厳密検証する）
//   vgeo_cook --gen-bench blob|knot|rock|torus|sphere|grid --tris N [--seed S] [--materials M] --out FILE.vgsrc   巨大ベンチメッシュ（VGSRC）
//   vgeo_cook --gen-bench KIND --tris N --cook OUT.vgeo [オプション]                                            生成してそのまま cook（VGSRC を経由しない）
//   vgeo_cook --validate FILE.vgeo [--no-deep] [--no-crc]      検証だけ
//   vgeo_cook --info FILE.vgeo                                ヘッダ / レベル別の要約
//   vgeo_cook --sweep group|lod0|weights IN.vgsrc|--gen-bench … 方式の比較表（入力を作り直して方式ごとに cook する）
//
//   オプション:
//     --threads N            使うスレッド数（既定 = 論理コア / 4。上限 = 論理コア）
//     --normal-priority      BelowNormal にしない（既定は BelowNormal。PC 操作を邪魔しない）
//     --mem-limit-gb G       作業メモリがこれを超えたら中止する（既定 10。0 = 無効）
//     --group-method morton|greedy|bisect|meshopt   --group-size G   --reduction R   --target-fill F
//     --normal-weight W --uv-weight W               --lock-borders    --no-dedupe
//     --lod0 classic|flex|spatial   --vcache        --max-verts V --max-tris T --cone-weight W
//     --proxy-tris N --nonvg-tris N --no-proxy      --pin-pages N     --label NAME
//     --no-validate --json FILE --levels --curve    --flip-winding --scale S（OBJ 用）
//
//   終了コード: 0 = 成功 / 1 = 引数エラー / 2 = 入出力・cook・検証の失敗 / 3 = メモリ上限で中止
//   合成データ・開発用 OBJ 専用。実アセット / UE 由来の資産は扱わない。
#include "CookBench.h"
#include "CookObj.h"
#include "VgeoCook.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace dx12e::vg;
using namespace dx12e::vg::cook;

namespace
{

int Usage()
{
    std::fprintf(stderr,
        "usage:\n"
        "  vgeo_cook IN.vgsrc|IN.obj OUT.vgeo [options]\n"
        "  vgeo_cook --gen-bench blob|knot|rock|torus|sphere|grid --tris N [--seed S] [--materials M] --out FILE.vgsrc\n"
        "  vgeo_cook --gen-bench KIND --tris N --cook OUT.vgeo [options]\n"
        "  vgeo_cook --validate FILE.vgeo [--no-deep] [--no-crc]\n"
        "  vgeo_cook --info FILE.vgeo\n"
        "  vgeo_cook --sweep group|lod0|weights (IN.vgsrc | --gen-bench KIND --tris N) [options]\n"
        "options: --threads N --normal-priority --mem-limit-gb G --group-method M --group-size G --reduction R --target-fill F --normal-weight W --uv-weight W\n"
        "         --lock-borders --no-dedupe --lod0 classic|flex|spatial --vcache --max-verts V --max-tris T --cone-weight W --proxy-tris N --nonvg-tris N --no-proxy\n"
        "         --pin-pages N --label NAME --no-validate --json FILE --levels --curve --flip-winding --scale S\n");
    return 1;
}

double Mb(u64 b) { return static_cast<double>(b) / (1024.0 * 1024.0); }
double Gb(u64 b) { return static_cast<double>(b) / (1024.0 * 1024.0 * 1024.0); }

std::atomic<bool> g_watchStop{false};
std::atomic<u64> g_peakSeen{0};

// 作業メモリ（ワーキングセット）の監視。上限を超えたら即中止（PC を固めない）。
void WatchMemory(double limitGb)
{
    const u64 limit = static_cast<u64>(limitGb * 1024.0 * 1024.0 * 1024.0);
    while (!g_watchStop.load())
    {
        const u64 ws = ProcessWorkingSetBytes();
        u64 prev = g_peakSeen.load();
        while (ws > prev && !g_peakSeen.compare_exchange_weak(prev, ws)) {}
        if (limit && ws > limit)
        {
            std::fprintf(stderr, "\n[vgeo_cook] ABORT: working set %.2f GB exceeded the limit %.2f GB\n", Gb(ws), limitGb);
            std::fflush(stderr);
            std::_Exit(3);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

int Validate(const std::string& path, const ReadOptions& opt)
{
    FileSource src;
    if (!src.Open(path)) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }
    const auto t0 = std::chrono::steady_clock::now();
    const ValidationReport rep = ValidateVgeo(src, opt);
    std::printf("validate: %s  %.1f ms  (%.1f MB, deep=%d crc=%d strict=%d)\n", rep.ok() ? "OK" : "FAILED", MsSince(t0), Mb(src.Size()), opt.deep ? 1 : 0, opt.verifyCrc ? 1 : 0,
                opt.strictReserved ? 1 : 0);
    for (const auto& i : rep.issues) std::printf("  %s\n", i.ToString().c_str());
    return rep.ok() ? 0 : 2;
}

void PrintLevels(const CookStats& st)
{
    std::printf("  level  clusters      tris  groups  avgGrp  cutEdges/shared  locked%%  tris out/in  reach%%  simplifyErr(mean/max)  lodErr(mean/max)\n");
    for (const LevelStat& l : st.perLevel)
        std::printf("  %5u %9llu %9llu %7llu %7.2f %8llu/%-8llu %6.1f %10.3f %7.1f  %9.3g / %-9.3g  %9.3g / %-9.3g\n", l.level, static_cast<unsigned long long>(l.clusters),
                    static_cast<unsigned long long>(l.tris), static_cast<unsigned long long>(l.groups), l.groups ? static_cast<double>(l.groupClusters) / static_cast<double>(l.groups) : 0.0,
                    static_cast<unsigned long long>(l.cutEdges), static_cast<unsigned long long>(l.sharedEdges), l.groupVerts ? 100.0 * static_cast<double>(l.lockedVerts) / static_cast<double>(l.groupVerts) : 0.0,
                    l.trisIn ? static_cast<double>(l.trisOut) / static_cast<double>(l.trisIn) : 0.0, l.groups ? 100.0 * static_cast<double>(l.reachedTarget) / static_cast<double>(l.groups) : 0.0,
                    l.meanSimplifyErr, l.maxSimplifyErr, l.meanLodErr, l.maxLodErr);
}

void PrintCurve(const CookStats& st)
{
    std::printf("  LOD curve (tau = 1 px, 1080p, fovY 60deg; distance / bounding radius -> drawn tris):\n");
    for (const LodCurvePoint& p : st.lodCurve)
        std::printf("    %6.1f x  %12llu tris  (reduction %.2f%%)\n", p.distanceOverRadius, static_cast<unsigned long long>(p.drawnTris), 100.0 * p.reduction);
}

void PrintSummary(const CookStats& st, u64 fileBytes)
{
    std::printf("  input: %llu tris, %llu verts (merged %llu), degenerate dropped %llu | VG %llu tris, NONVG %llu tris\n", static_cast<unsigned long long>(st.inputTris),
                static_cast<unsigned long long>(st.inputVerts), static_cast<unsigned long long>(st.verticesMerged), static_cast<unsigned long long>(st.degenerateDropped),
                static_cast<unsigned long long>(st.vgTris), static_cast<unsigned long long>(st.nonVgTris));
    if (st.stagnatedRoots) std::printf("  NOTE: %u level(s) could not be simplified further (boundary locks); that level became the roots\n", st.stagnatedRoots);
    std::printf("  output: clusters %llu (LOD0 %llu, roots %llu) groups %llu pages %llu nodes %llu levels %u pinned %u | %.1f tri/cluster (LOD0 fill %.1f%%, all %.1f%%) %.1f vert/cluster, pos %.1f bit/vert, page fill %.1f%%\n",
                static_cast<unsigned long long>(st.clusters), static_cast<unsigned long long>(st.lod0Clusters), static_cast<unsigned long long>(st.rootClusters),
                static_cast<unsigned long long>(st.groups), static_cast<unsigned long long>(st.pages), static_cast<unsigned long long>(st.nodes), st.levels, st.pinnedPages,
                st.avgTrisPerCluster, 100.0 * st.avgFillLod0, 100.0 * st.avgFillAll, st.avgVertsPerCluster, st.avgPosBits, 100.0 * st.pageFill);
    if (fileBytes && st.vgTris)
        std::printf("  size: file %.2f MB = %.2f B / LOD0 tri | pages %.2f MB = %.2f B / LOD0 tri\n", Mb(fileBytes), static_cast<double>(fileBytes) / static_cast<double>(st.vgTris),
                    Mb(st.pageBytes), static_cast<double>(st.pageBytes) / static_cast<double>(st.vgTris));
    std::printf("  time ms: ingest %.0f  lod0 %.0f  dag %.0f (graph %.0f, group %.0f, simplify %.0f)  pack %.0f  bvh %.0f  proxy %.0f | cook total %.0f (%.2f Mtri/s, %u threads)\n", st.msIngest, st.msLod0,
                st.msDag, st.msGraph, st.msGroup, st.msSimplify, st.msPack, st.msBvh, st.msProxy, st.msTotal,
                st.msTotal > 0 ? static_cast<double>(st.vgTris) / 1e6 / (st.msTotal / 1000.0) : 0.0, st.threadsUsed);
}

void WriteJson(const std::string& path, const CookStats& st, u64 fileBytes, double msWrite, double msValidate)
{
    std::FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return;
    std::fprintf(f, "{\n  \"inputTris\":%llu,\"inputVerts\":%llu,\"vgTris\":%llu,\"nonVgTris\":%llu,\"degenerateDropped\":%llu,\"verticesMerged\":%llu,\n",
                 static_cast<unsigned long long>(st.inputTris), static_cast<unsigned long long>(st.inputVerts), static_cast<unsigned long long>(st.vgTris), static_cast<unsigned long long>(st.nonVgTris),
                 static_cast<unsigned long long>(st.degenerateDropped), static_cast<unsigned long long>(st.verticesMerged));
    std::fprintf(f, "  \"clusters\":%llu,\"lod0Clusters\":%llu,\"rootClusters\":%llu,\"groups\":%llu,\"pages\":%llu,\"nodes\":%llu,\"levels\":%u,\"pinnedPages\":%u,\n", static_cast<unsigned long long>(st.clusters),
                 static_cast<unsigned long long>(st.lod0Clusters), static_cast<unsigned long long>(st.rootClusters), static_cast<unsigned long long>(st.groups), static_cast<unsigned long long>(st.pages),
                 static_cast<unsigned long long>(st.nodes), st.levels, st.pinnedPages);
    std::fprintf(f, "  \"fileBytes\":%llu,\"pageBytes\":%llu,\"bytesPerTri\":%.4f,\"pageBytesPerTri\":%.4f,\"avgTrisPerCluster\":%.3f,\"fillLod0\":%.4f,\"fillAll\":%.4f,\"avgPosBits\":%.2f,\"pageFill\":%.4f,\n",
                 static_cast<unsigned long long>(fileBytes), static_cast<unsigned long long>(st.pageBytes), st.vgTris ? static_cast<double>(fileBytes) / static_cast<double>(st.vgTris) : 0.0,
                 st.vgTris ? static_cast<double>(st.pageBytes) / static_cast<double>(st.vgTris) : 0.0, st.avgTrisPerCluster, st.avgFillLod0, st.avgFillAll, st.avgPosBits, st.pageFill);
    std::fprintf(f, "  \"msIngest\":%.1f,\"msLod0\":%.1f,\"msDag\":%.1f,\"msGraph\":%.1f,\"msGroup\":%.1f,\"msSimplify\":%.1f,\"msPack\":%.1f,\"msBvh\":%.1f,\"msProxy\":%.1f,\"msCook\":%.1f,\"msWrite\":%.1f,\"msValidate\":%.1f,\n",
                 st.msIngest, st.msLod0, st.msDag, st.msGraph, st.msGroup, st.msSimplify, st.msPack, st.msBvh, st.msProxy, st.msTotal, msWrite, msValidate);
    std::fprintf(f, "  \"threads\":%u,\"peakWorkingSetBytes\":%llu,\n  \"perLevel\":[\n", st.threadsUsed, static_cast<unsigned long long>(st.peakWorkingSet));
    for (size_t i = 0; i < st.perLevel.size(); ++i)
    {
        const LevelStat& l = st.perLevel[i];
        std::fprintf(f, "    {\"level\":%u,\"clusters\":%llu,\"tris\":%llu,\"groups\":%llu,\"groupClusters\":%llu,\"cutEdges\":%llu,\"sharedEdges\":%llu,\"trisIn\":%llu,\"trisOut\":%llu,\"lockedVerts\":%llu,\"groupVerts\":%llu,"
                     "\"meanSimplifyErr\":%.6g,\"maxSimplifyErr\":%.6g,\"meanLodErr\":%.6g,\"maxLodErr\":%.6g,\"reached\":%llu}%s\n",
                     l.level, static_cast<unsigned long long>(l.clusters), static_cast<unsigned long long>(l.tris), static_cast<unsigned long long>(l.groups), static_cast<unsigned long long>(l.groupClusters),
                     static_cast<unsigned long long>(l.cutEdges), static_cast<unsigned long long>(l.sharedEdges), static_cast<unsigned long long>(l.trisIn), static_cast<unsigned long long>(l.trisOut),
                     static_cast<unsigned long long>(l.lockedVerts), static_cast<unsigned long long>(l.groupVerts), l.meanSimplifyErr, l.maxSimplifyErr, l.meanLodErr, l.maxLodErr,
                     static_cast<unsigned long long>(l.reachedTarget), i + 1 < st.perLevel.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"lodCurve\":[\n");
    for (size_t i = 0; i < st.lodCurve.size(); ++i)
        std::fprintf(f, "    {\"distOverRadius\":%.2f,\"drawnTris\":%llu,\"reduction\":%.5f}%s\n", st.lodCurve[i].distanceOverRadius, static_cast<unsigned long long>(st.lodCurve[i].drawnTris),
                     st.lodCurve[i].reduction, i + 1 < st.lodCurve.size() ? "," : "");
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
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
    std::printf("  file %.2f MB  pages %u  clusters %u  groups %u  nodes %u  levels %u  roots %u  pinned %u\n", Mb(m.fileSize), h.pageCount, h.clusterCount, h.groupCount, h.nodeCount, h.levelCount,
                h.rootClusterCount, h.pinnedPageCount);
    std::printf("  source tris %llu  verts %llu  file bytes/tri %.2f  page bytes/tri %.2f\n", static_cast<unsigned long long>(h.sourceTriangleCount), static_cast<unsigned long long>(h.sourceVertexCount),
                h.sourceTriangleCount ? static_cast<double>(m.fileSize) / static_cast<double>(h.sourceTriangleCount) : 0.0,
                h.sourceTriangleCount ? static_cast<double>(h.pageCount) * kPageSize / static_cast<double>(h.sourceTriangleCount) : 0.0);
    std::printf("  aabb (%.3f %.3f %.3f) - (%.3f %.3f %.3f)  posStep %.3g  proxy %u sections (error %.4g)  nonvg %u\n", h.aabbMin[0], h.aabbMin[1], h.aabbMin[2], h.aabbMax[0], h.aabbMax[1], h.aabbMax[2],
                h.posStep, h.proxySectionCount, h.proxyError, h.nonVgSectionCount);
    if (!m.debugJson.empty()) std::printf("  debug: %.400s%s\n", m.debugJson.c_str(), m.debugJson.size() > 400 ? " ..." : "");
    return 0;
}

struct Args
{
    std::string in, out, jsonPath, genKind, genOut, genCook, sweep;
    u64 genTris = 0;
    u32 genSeed = 1, genMaterials = 1;
    double memLimitGb = 10.0;
    bool noValidate = false, levels = false, curve = false, flipWinding = false, normalPriority = false;
    f32 scale = 1.0f;
    CookOptions co;
};

VgeoError LoadInput(const Args& a, VgsrcData& d)
{
    if (!a.genKind.empty())
    {
        BenchOptions bo;
        bo.kind = a.genKind; bo.tris = a.genTris; bo.seed = a.genSeed; bo.materials = a.genMaterials; bo.threads = a.co.threads; bo.lowPriority = !a.normalPriority;
        return GenerateBench(bo, d);
    }
    const std::string ext = a.in.size() >= 4 ? a.in.substr(a.in.size() - 4) : "";
    if (ext == ".obj" || ext == ".OBJ") return LoadObjFile(a.in, d, a.flipWinding, a.scale);
    FileSource src;
    if (!src.Open(a.in)) return detail::MakeErr(Errc::IoError, "cannot open " + a.in);
    return ReadVgsrc(src, d, false);
}

// 1 回 cook して要約を出す。out が空なら書かない。
int CookOnce(const Args& a, const CookOptions& co, const std::string& outPath, bool quiet, CookStats* statsOut)
{
    VgsrcData d;
    const auto t0 = std::chrono::steady_clock::now();
    VgeoError e = LoadInput(a, d);
    if (!e.ok()) { std::fprintf(stderr, "input failed: %s\n", e.ToString().c_str()); return 2; }
    const double msLoad = MsSince(t0);
    if (!quiet) std::printf("input ready: %zu tris, %zu verts (%.0f ms)\n", d.indices.size() / 3, d.positions.size() / 3, msLoad);
    VgeoContent content;
    CookStats st;
    e = Cook(std::move(d), co, content, &st);
    if (!e.ok()) { std::fprintf(stderr, "cook failed: %s\n", e.ToString().c_str()); return 2; }
    u64 fileBytes = 0;
    double msWrite = 0, msValid = 0;
    if (!outPath.empty())
    {
        const auto tw = std::chrono::steady_clock::now();
        e = WriteVgeoToFile(content, outPath);
        msWrite = MsSince(tw);
        if (!e.ok()) { std::fprintf(stderr, "write failed: %s\n", e.ToString().c_str()); return 2; }
        FileSource fs;
        if (fs.Open(outPath)) fileBytes = fs.Size();
        if (!a.noValidate)
        {
            const auto tv = std::chrono::steady_clock::now();
            ReadOptions ro;
            ro.strictReserved = true;
            const ValidationReport rep = ValidateVgeo(fs, ro);
            msValid = MsSince(tv);
            if (!rep.ok())
            {
                std::fprintf(stderr, "VALIDATION FAILED (%zu issues)\n", rep.issues.size());
                for (const auto& i : rep.issues) std::fprintf(stderr, "  %s\n", i.ToString().c_str());
                return 2;
            }
            if (!quiet) std::printf("validate: OK (strict, deep) %.0f ms\n", msValid);
        }
    }
    else
    {
        // ファイルサイズの推定（書かないとき）: ヘッダ + 全セクション（4096 整列）
        std::vector<u8> bytes;
        if (content.pages.size() < (64u << 20) && WriteVgeoToMemory(content, bytes).ok()) fileBytes = bytes.size();
        else fileBytes = content.pages.size();
    }
    if (!quiet)
    {
        if (!outPath.empty()) std::printf("wrote %s (%.0f ms)\n", outPath.c_str(), msWrite);
        PrintSummary(st, fileBytes);
        if (a.levels) PrintLevels(st);
        if (a.curve) PrintCurve(st);
        std::printf("  memory: peak working set %.2f GB (sampled %.2f GB)\n", Gb(ProcessPeakWorkingSetBytes()), Gb(g_peakSeen.load()));
    }
    if (!a.jsonPath.empty() && statsOut == nullptr) WriteJson(a.jsonPath, st, fileBytes, msWrite, msValid);
    if (statsOut) { *statsOut = st; statsOut->fileBytesEstimate = fileBytes; }
    return 0;
}

int Sweep(const Args& a)
{
    struct Variant { std::string name; CookOptions co; };
    std::vector<Variant> vs;
    if (a.sweep == "group")
        for (GroupMethod m : {GroupMethod::Morton, GroupMethod::Bisect, GroupMethod::Greedy, GroupMethod::Meshopt})
        {
            Variant v{GroupMethodName(m), a.co};
            v.co.groupMethod = m;
            vs.push_back(v);
        }
    else if (a.sweep == "lod0")
        for (Lod0Builder b : {Lod0Builder::Classic, Lod0Builder::Flex, Lod0Builder::Spatial})
            for (int pre : {0, 1, 2})
            {
                Variant v{std::string(b == Lod0Builder::Classic ? "classic" : b == Lod0Builder::Flex ? "flex" : "spatial") + (pre == 1 ? "+vcache" : pre == 2 ? "+vc+overdraw" : ""), a.co};
                v.co.lod0Builder = b;
                v.co.vertexCacheOpt = pre >= 1;
                v.co.overdrawOpt = pre == 2;
                vs.push_back(v);
            }
    else if (a.sweep == "weights")
        for (f32 nw : {0.0f, 0.25f, 0.5f, 1.0f})
            for (f32 uw : {0.0f, 0.25f, 1.0f})
            {
                char nm[64];
                std::snprintf(nm, sizeof nm, "n%.2f/uv%.2f", static_cast<double>(nw), static_cast<double>(uw));
                Variant v{nm, a.co};
                v.co.normalWeight = nw;
                v.co.uvWeight = uw;
                vs.push_back(v);
            }
    else return Usage();
    std::printf("%-16s %9s %8s %7s %9s %9s %8s %8s %8s %9s %9s %8s %7s %7s\n", "variant", "clusters", "levels", "roots", "LOD0fill%", "allfill%", "B/tri", "cutEdge%", "lock%", "err(lvl3)", "err(root)", "cook ms", "coneOK%", "coneCut");
    for (const Variant& v : vs)
    {
        CookStats st;
        const int rc = CookOnce(a, v.co, std::string(), true, &st);
        if (rc != 0) return rc;
        u64 cut = 0, shared = 0, lockedV = 0, gv = 0;
        for (const LevelStat& l : st.perLevel) { cut += l.cutEdges; shared += l.sharedEdges; lockedV += l.lockedVerts; gv += l.groupVerts; }
        f64 e3 = st.perLevel.size() > 3 ? st.perLevel[3].maxLodErr : 0.0, er = st.perLevel.empty() ? 0.0 : st.perLevel.back().maxLodErr;
        std::printf("%-16s %9llu %8u %7llu %9.1f %9.1f %8.2f %8.2f %8.1f %9.3g %9.3g %8.0f %7.1f %7.3f\n", v.name.c_str(), static_cast<unsigned long long>(st.clusters), st.levels, static_cast<unsigned long long>(st.rootClusters),
                    100.0 * st.avgFillLod0, 100.0 * st.avgFillAll, st.vgTris ? static_cast<double>(st.pageBytes) / static_cast<double>(st.vgTris) : 0.0,
                    shared ? 100.0 * static_cast<double>(cut) / static_cast<double>(shared) : 0.0, gv ? 100.0 * static_cast<double>(lockedV) / static_cast<double>(gv) : 0.0, e3, er, st.msTotal, 100.0 * st.coneValidFrac, st.coneMeanCutoff);
        std::fflush(stdout);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Args a;
    std::string validate, info;
    ReadOptions ro;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i)
    {
        const std::string s = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (s == "--gen-bench" && (v = next())) a.genKind = v;
        else if (s == "--tris" && (v = next())) a.genTris = std::strtoull(v, nullptr, 10);
        else if (s == "--seed" && (v = next())) a.genSeed = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (s == "--materials" && (v = next())) a.genMaterials = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (s == "--out" && (v = next())) a.genOut = v;
        else if (s == "--cook" && (v = next())) a.genCook = v;
        else if (s == "--validate" && (v = next())) validate = v;
        else if (s == "--info" && (v = next())) info = v;
        else if (s == "--sweep" && (v = next())) a.sweep = v;
        else if (s == "--json" && (v = next())) a.jsonPath = v;
        else if (s == "--threads" && (v = next())) a.co.threads = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (s == "--normal-priority") { a.normalPriority = true; a.co.lowPriority = false; }
        else if (s == "--mem-limit-gb" && (v = next())) a.memLimitGb = std::atof(v);
        else if (s == "--group-method" && (v = next())) { if (!ParseGroupMethod(v, a.co.groupMethod)) return Usage(); }
        else if (s == "--group-size" && (v = next())) a.co.groupSize = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (s == "--reduction" && (v = next())) a.co.reduction = static_cast<f32>(std::atof(v));
        else if (s == "--target-fill" && (v = next())) a.co.targetFill = static_cast<f32>(std::atof(v));
        else if (s == "--normal-weight" && (v = next())) a.co.normalWeight = static_cast<f32>(std::atof(v));
        else if (s == "--uv-weight" && (v = next())) a.co.uvWeight = static_cast<f32>(std::atof(v));
        else if (s == "--lock-borders") a.co.lockOpenBorders = true;
        else if (s == "--regularize") a.co.regularize = true;
        else if (s == "--permissive" && (v = next())) a.co.permissive = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (s == "--no-dedupe") a.co.dedupe = false;
        else if (s == "--lod0" && (v = next()))
        {
            const std::string b = v;
            if (b == "classic") a.co.lod0Builder = Lod0Builder::Classic; else if (b == "flex") a.co.lod0Builder = Lod0Builder::Flex; else if (b == "spatial") a.co.lod0Builder = Lod0Builder::Spatial; else return Usage();
        }
        else if (s == "--vcache") a.co.vertexCacheOpt = true;
        else if (s == "--overdraw") { a.co.vertexCacheOpt = true; a.co.overdrawOpt = true; }
        else if (s == "--max-verts" && (v = next())) a.co.maxVerts = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (s == "--max-tris" && (v = next())) a.co.maxTris = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (s == "--cone-weight" && (v = next())) a.co.coneWeight = static_cast<f32>(std::atof(v));
        else if (s == "--proxy-tris" && (v = next())) a.co.proxyMaxTris = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (s == "--nonvg-tris" && (v = next())) a.co.nonVgMaxTris = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (s == "--no-proxy") a.co.proxy = false;
        else if (s == "--pin-pages" && (v = next())) a.co.pinPages = static_cast<u32>(std::strtoul(v, nullptr, 10));
        else if (s == "--label" && (v = next())) a.co.label = v;
        else if (s == "--no-validate") a.noValidate = true;
        else if (s == "--levels") a.levels = true;
        else if (s == "--curve") a.curve = true;
        else if (s == "--verbose") a.co.verbose = true;
        else if (s == "--flip-winding") a.flipWinding = true;
        else if (s == "--scale" && (v = next())) a.scale = static_cast<f32>(std::atof(v));
        else if (s == "--no-deep") ro.deep = false;
        else if (s == "--no-crc") ro.verifyCrc = false;
        else if (s == "--strict") ro.strictReserved = true;
        else if (!s.empty() && s[0] != '-') positional.push_back(s);
        else return Usage();
    }
    if (!validate.empty()) return Validate(validate, ro);
    if (!info.empty()) return Info(info);

    // 論理コアを超えないように丸める。PC を占有しない（既定 = 論理コア / 4、BelowNormal）。
    if (a.co.threads > HardwareThreads()) a.co.threads = HardwareThreads();
    if (!a.normalPriority) SetProcessBelowNormal();
    std::thread watcher;
    if (a.memLimitGb > 0 || true) watcher = std::thread(WatchMemory, a.memLimitGb);
    struct Guard { std::thread& t; ~Guard() { g_watchStop.store(true); if (t.joinable()) t.join(); } } guard{watcher};

    if (!a.genKind.empty() && !a.genOut.empty())
    {
        if (a.genTris == 0) return Usage();
        BenchOptions bo;
        bo.kind = a.genKind; bo.tris = a.genTris; bo.seed = a.genSeed; bo.materials = a.genMaterials; bo.threads = a.co.threads; bo.lowPriority = !a.normalPriority;
        VgsrcData d;
        const auto t0 = std::chrono::steady_clock::now();
        VgeoError e = GenerateBench(bo, d);
        if (!e.ok()) { std::fprintf(stderr, "bench generation failed: %s\n", e.ToString().c_str()); return 2; }
        const double msGen = MsSince(t0);
        FileSink fs;
        if (!fs.Open(a.genOut)) { std::fprintf(stderr, "cannot open %s for write\n", a.genOut.c_str()); return 2; }
        e = WriteVgsrc(d, fs);
        if (!fs.Close() || !e.ok()) { std::fprintf(stderr, "vgsrc write failed: %s\n", e.ToString().c_str()); return 2; }
        std::printf("wrote %s (VGSRC, %s, %zu tris, %zu verts, %.0f ms gen, %.2f GB peak)\n", a.genOut.c_str(), a.genKind.c_str(), d.indices.size() / 3, d.positions.size() / 3, msGen,
                    Gb(ProcessPeakWorkingSetBytes()));
        return 0;
    }
    if (!a.sweep.empty())
    {
        if (a.genKind.empty()) { if (positional.empty()) return Usage(); a.in = positional[0]; }
        else if (a.genTris == 0) return Usage();
        return Sweep(a);
    }
    if (!a.genKind.empty())
    {
        if (a.genTris == 0 || a.genCook.empty()) return Usage();
        return CookOnce(a, a.co, a.genCook, false, nullptr);
    }
    if (positional.size() != 2) return Usage();
    a.in = positional[0];
    return CookOnce(a, a.co, positional[1], false, nullptr);
}
