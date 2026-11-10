// 仮想ジオメトリの自動 cook（resource/VgAutoCook）と材質つき .vgeo（v1.1: AO・拡張シェーディング 'MSHD'）のテスト。GPU 不要。
//
// 守っているもの:
//   ・判定（DecideIneligible）: auto / on / off・しきい値・スキン / アニメ・アルファ材質・頂点カラー・材質数
//   ・glTF の JSON だけで判定する前置き（PrecheckGltfJson）: ノードの参照回数・MASK・スキン・COLOR_0・範囲
//   ・取り出し（ModelLoader::ExtractStaticForVg）の座標が ModelLoader::Probe（= LoadFromFile と同じ変換）と一致すること
//     （FBX の cm → m・ノードのスケール・glTF / OBJ の等倍）。AO（occlusionTexture）の取り込み。拡張シェーディング
//   ・鍵（ComputeKey）が中身 / import.json / cook パラメータで変わること
//   ・.vgeo v1.1: AO（MaterialRecord.aoStrength）と 'MSHD' が書いて読めて、使わないファイルは v1.0 とバイト一致（minor 0・reserved1 ゼロ）
#include "resource/ModelLoader.h"
#include "resource/VgAutoCook.h"
#include "renderer/vg/VgShadingRecord.h"
#include "renderer/vg/VgeoFormat.h"
#include "VgeoCook.h"

#include <cmath>
#include <cstdio>
#include <string>

#include <Windows.h>
#include <objbase.h>

using namespace dx12e;

namespace
{
int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("[FAIL] %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

std::string Data(const char* f) { return std::string(DX12E_TEST_DATA_DIR) + "/" + f; }

void TestDecide()
{
    using M = ModelImportOptions::VgMode;
    vgauto::Config cfg;
    cfg.minTriangles = 1000;
    cfg.maxMaterials = 4;
    vgauto::Facts f;
    f.triangles = 5000; f.materials = 2;
    CHECK(vgauto::DecideIneligible(M::Auto, cfg, f).empty(), "big opaque static should cook");
    f.triangles = 999;
    CHECK(!vgauto::DecideIneligible(M::Auto, cfg, f).empty(), "below threshold must not cook in auto");
    CHECK(vgauto::DecideIneligible(M::On, cfg, f).empty(), "on ignores the threshold");
    f.triangles = 5000;
    CHECK(!vgauto::DecideIneligible(M::Off, cfg, f).empty(), "off never cooks");
    f.nonOpaque = true;
    CHECK(!vgauto::DecideIneligible(M::Auto, cfg, f).empty(), "alpha material not in auto");
    CHECK(vgauto::DecideIneligible(M::On, cfg, f).empty(), "on cooks alpha into NONVG");
    f.nonOpaque = false; f.vertexColor = true;
    CHECK(!vgauto::DecideIneligible(M::Auto, cfg, f).empty(), "vertex color not in auto");
    f.vertexColor = false; f.materials = 5;
    CHECK(!vgauto::DecideIneligible(M::Auto, cfg, f).empty(), "too many materials not in auto");
    CHECK(vgauto::DecideIneligible(M::On, cfg, f).empty(), "on ignores material count");
    f.materials = 2; f.skin = true;
    CHECK(!vgauto::DecideIneligible(M::On, cfg, f).empty(), "skinned never cooks, even with on");
    f.skin = false; f.animation = true;
    CHECK(!vgauto::DecideIneligible(M::On, cfg, f).empty(), "animated never cooks, even with on");
    std::printf("[ ok ] DecideIneligible\n");
}

void TestPrecheck()
{
    // mesh 0（100 tri の索引付き）を 3 ノードが参照、mesh 1（非索引 30 頂点 = 10 tri、MASK）を参照なし
    const std::string json = R"({
      "accessors":[{"count":300},{"count":200,"min":[-1,0,-2],"max":[1,3,2]},{"count":30,"min":[-5,-5,-5],"max":[0,0,0]}],
      "meshes":[{"primitives":[{"indices":0,"attributes":{"POSITION":1},"material":0}]},
                {"primitives":[{"attributes":{"POSITION":2,"COLOR_0":2},"material":1}]}],
      "nodes":[{"mesh":0},{"mesh":0},{"mesh":0},{"name":"empty"}],
      "materials":[{"name":"a"},{"name":"b","alphaMode":"MASK"}]})";
    vgauto::Facts f;
    float lo[3], hi[3];
    bool hb = false;
    CHECK(vgauto::PrecheckGltfJson(json, f, lo, hi, hb, nullptr), "precheck parse");
    CHECK(f.triangles == 3 * 100 + 10, "triangles %llu (expect 310: node instancing + unreferenced mesh)", static_cast<unsigned long long>(f.triangles));
    CHECK(f.nonOpaque, "MASK material detected");
    CHECK(f.vertexColor, "COLOR_0 detected");
    CHECK(f.materials == 2, "materials %u", f.materials);
    CHECK(!f.skin && !f.animation, "no skin / animation");
    CHECK(hb && lo[0] == -5.0f && hi[1] == 3.0f, "bounds union");
    const std::string skinned = R"({"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"accessors":[{"count":3}],"nodes":[{"mesh":0,"skin":0}],"skins":[{}],"animations":[{}]})";
    CHECK(vgauto::PrecheckGltfJson(skinned, f, lo, hi, hb, nullptr) && f.skin && f.animation, "skin / animation detected");
    CHECK(!vgauto::PrecheckGltfJson("not json", f, lo, hi, hb, nullptr), "broken json fails");
    // 実ファイル（.glb の JSON チャンクだけ読む）
    CHECK(vgauto::PrecheckGltfFile(Data("cube1m.glb"), f, lo, hi, hb, nullptr) && f.triangles == 12, "cube1m.glb triangles %llu", static_cast<unsigned long long>(f.triangles));
    std::printf("[ ok ] PrecheckGltf\n");
}

// 取り出しの範囲が Probe（LoadFromFile と同じ単位正規化・ノード変換）と一致すること
void TestExtractMatchesProbe(const char* file, uint64_t expectTris)
{
    StaticModelCpu cpu;
    std::string err;
    const bool ok = ModelLoader::ExtractStaticForVg(Data(file), cpu, &err);
    CHECK(ok, "%s: extract failed (%s)", file, err.c_str());
    if (!ok) return;
    const ModelProbeInfo info = ModelLoader::Probe(Data(file));
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i + 2 < cpu.positions.size(); i += 3)
        for (int k = 0; k < 3; ++k) { lo[k] = std::fmin(lo[k], cpu.positions[i + k]); hi[k] = std::fmax(hi[k], cpu.positions[i + k]); }
    for (int k = 0; k < 3; ++k)
        CHECK(std::fabs(lo[k] - info.aabbMin[k]) < 1e-3f && std::fabs(hi[k] - info.aabbMax[k]) < 1e-3f,
              "%s: axis %d extract [%.4f, %.4f] vs probe [%.4f, %.4f]", file, k, lo[k], hi[k], info.aabbMin[k], info.aabbMax[k]);
    CHECK(cpu.triangles == expectTris, "%s: triangles %llu", file, static_cast<unsigned long long>(cpu.triangles));
    CHECK(cpu.indices.size() == cpu.triangles * 3, "%s: index count", file);
    uint64_t covered = 0;
    for (const auto& s : cpu.sections) { CHECK(s.firstIndex == covered, "%s: sections tile", file); covered += s.indexCount; }
    CHECK(covered == cpu.indices.size(), "%s: sections cover all indices", file);
    CHECK(cpu.normals.size() == cpu.positions.size() && cpu.uvs.size() / 2 == cpu.positions.size() / 3, "%s: attribute sizes", file);
    std::printf("[ ok ] ExtractStaticForVg %s (%llu tris, %zu materials)\n", file, static_cast<unsigned long long>(cpu.triangles), cpu.materials.size());
}

void TestExtractMaterials()
{
    StaticModelCpu cpu;
    std::string err;
    CHECK(ModelLoader::ExtractStaticForVg(Data("ao_separate.glb"), cpu, &err), "ao_separate: %s", err.c_str());
    bool ao = false;
    for (const auto& m : cpu.materials) if (m.occlusionInMR && !m.metalRough.Empty()) ao = true;
    CHECK(ao, "ao_separate.glb: occlusionTexture → ORM (occlusionInMR)");
    cpu = StaticModelCpu{};
    CHECK(ModelLoader::ExtractStaticForVg(Data("shading_ext.glb"), cpu, &err), "shading_ext: %s", err.c_str());
    bool ext = false;
    for (const auto& m : cpu.materials) if (!m.shading.IsDefault()) ext = true;
    CHECK(ext, "shading_ext.glb: clearcoat / sheen / anisotropy reach the cook");
    std::printf("[ ok ] ExtractStaticForVg materials (AO / shading)\n");
}

void TestKey()
{
    const std::string p = "a=1";
    const uint64_t k0 = vgauto::ComputeKey(1, "", p);
    CHECK(k0 != vgauto::ComputeKey(2, "", p), "content changes the key");
    CHECK(k0 != vgauto::ComputeKey(1, "{\"leftHanded\":true}", p), "import.json changes the key");
    CHECK(k0 != vgauto::ComputeKey(1, "", "a=2"), "cook params change the key");
    CHECK(k0 == vgauto::ComputeKey(1, "", p), "key is deterministic");
    CHECK(vgauto::HashFileContents(Data("cube1m.glb")) == vgauto::HashFileContents(Data("cube1m.glb")) && vgauto::HashFileContents(Data("cube1m.glb")) != 0,
          "file hash deterministic");
    CHECK(vgauto::HashFileContents(Data("cube1m.glb")) != vgauto::HashFileContents(Data("cube1m.obj")), "different files hash differently");
    std::printf("[ ok ] ComputeKey / HashFileContents\n");
}

void TestShadingRecord()
{
    ShadingExt e;
    e.clearCoat = 0.6f; e.clearCoatRoughness = 0.1f; e.anisotropy = -0.4f; e.anisotropyRotation = 1.0f;
    e.sheenColor[0] = 0.2f; e.sheenRoughness = 0.5f; e.specularFactor = 0.8f; e.specularColor[2] = 0.5f; e.ior = 1.33f;
    CHECK(vg::UnpackShadingRecord(vg::PackShadingRecord(e)) == e, "MSHD record roundtrip");
    CHECK(vg::UnpackShadingRecord(vg::PackShadingRecord(ShadingExt{})).IsDefault(), "default stays default");
    std::printf("[ ok ] VgShadingRecord\n");
}

// 小さい板（2 材質）を cook → 書いて読む。AO / MSHD の有無で minor と reserved1 が変わる
void TestFormatV11()
{
    auto make = [](bool withExt, vg::VgeoContent& out)
    {
        std::vector<float> pos, nrm, uv;
        std::vector<uint32_t> idx;
        const int N = 40;
        for (int y = 0; y <= N; ++y)
            for (int x = 0; x <= N; ++x)
            {
                pos.insert(pos.end(), {x / float(N), 0.0f, y / float(N)});
                nrm.insert(nrm.end(), {0.0f, 1.0f, 0.0f});
                uv.insert(uv.end(), {x / float(N), y / float(N)});
            }
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
            {
                const uint32_t a = y * (N + 1) + x, b = a + 1, c = a + N + 1, d = c + 1;
                idx.insert(idx.end(), {a, c, b, b, c, d});
            }
        vg::VgsrcData src = vg::cook::MakeVgsrcFromTriangles(pos, idx, &nrm, &uv, "m0");
        // 2 材質に分ける（前半 / 後半）
        const uint32_t half = static_cast<uint32_t>(idx.size() / 2 / 3 * 3);
        src.sections = {vg::VgsrcSection{0, 0, half, 0}, vg::VgsrcSection{1, half, static_cast<uint32_t>(idx.size()) - half, 0}};
        vg::StringPool pool;
        for (int m = 0; m < 2; ++m)
        {
            vg::MaterialRecord r = vg::MakeDefaultMaterial();
            r.nameOff = pool.Add(m ? "m1" : "m0");
            r.metalRoughPathOff = pool.Add("t/mr.png");
            r.flags = vg::kMatHasMetalRough;
            if (withExt && m == 1) { r.flags |= vg::kMatAoInMR; r.aoStrength = 0.75f; }
            src.materials.resize(2);
            src.materials[m] = r;
        }
        src.strings = pool.Data();
        if (withExt)
        {
            ShadingExt e; e.clearCoat = 0.5f;
            src.materialShading = {vg::PackShadingRecord(ShadingExt{}), vg::PackShadingRecord(e)};
        }
        vg::cook::CookOptions co;
        co.threads = 2; co.collectStats = false;
        return vg::cook::Cook(std::move(src), co, out, nullptr);
    };
    for (int withExt = 0; withExt < 2; ++withExt)
    {
        vg::VgeoContent c;
        const vg::VgeoError e = make(withExt != 0, c);
        CHECK(e.ok(), "cook: %s", e.ToString().c_str());
        if (!e.ok()) continue;
        const vg::ValidationReport rep = vg::ValidateContent(c);
        CHECK(rep.ok(), "strict validation (%s): %s", withExt ? "ext" : "plain", rep.ok() ? "" : rep.issues.front().ToString().c_str());
        std::vector<uint8_t> bytes;
        CHECK(vg::WriteVgeoToMemory(c, bytes).ok(), "write");
        vg::VgeoMeta meta;
        const vg::VgeoError le = vg::LoadMeta(vg::MemorySource(bytes), meta);
        CHECK(le.ok(), "LoadMeta: %s", le.ToString().c_str());
        if (!le.ok()) continue;
        if (withExt)
        {
            CHECK(meta.header.versionMinor == 1, "minor 1 when v1.1 fields are used");
            CHECK(meta.materialShading.size() == meta.materials.size(), "MSHD present (%zu)", meta.materialShading.size());
            bool coat = false, ao = false;
            for (size_t i = 0; i < meta.materials.size(); ++i)
            {
                if (i < meta.materialShading.size() && vg::UnpackShadingRecord(meta.materialShading[i]).clearCoat == 0.5f) coat = true;
                if ((meta.materials[i].flags & vg::kMatAoInMR) && meta.materials[i].aoStrength == 0.75f) ao = true;
            }
            CHECK(coat, "clearcoat survives the cook");
            CHECK(ao, "AO strength survives the cook");
        }
        else
        {
            CHECK(meta.header.versionMinor == 0, "v1.0 bytes when no v1.1 field is used");
            bool zero = true;
            for (uint8_t b : meta.header.reserved1) if (b) zero = false;
            CHECK(zero && meta.materialShading.empty(), "no extension table");
        }
        // 壊した MSHD は CRC で落ちる
        if (withExt && !bytes.empty())
        {
            uint64_t off = 0;
            std::memcpy(&off, meta.header.reserved1, 8);
            vg::ExtEntry ee{};
            std::memcpy(&ee, bytes.data() + off, sizeof ee);
            bytes[static_cast<size_t>(ee.offset)] ^= 0x40;
            vg::VgeoMeta bad;
            CHECK(!vg::LoadMeta(vg::MemorySource(bytes), bad).ok(), "corrupted MSHD is rejected");
        }
    }
    std::printf("[ ok ] .vgeo v1.1 (AO / MSHD)\n");
}
} // namespace

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // ORM の合成（WIC）。エンジンではメインスレッド / cook のスレッドが済ませている
    TestDecide();
    TestPrecheck();
    TestExtractMatchesProbe("cube1m.glb", 12);
    TestExtractMatchesProbe("cube1m.obj", 12);
    TestExtractMatchesProbe("cube1m_blender_default.fbx", 12);   // cm → m（UnitScaleFactor）
    TestExtractMatchesProbe("cube2m_nodescale.fbx", 12);         // ノードのスケール 2 を焼く
    TestExtractMaterials();
    TestKey();
    TestShadingRecord();
    TestFormatV11();
    if (g_fail) { std::printf("vg_auto_cook_test: %d failure(s)\n", g_fail); return 1; }
    std::printf("vg_auto_cook_test: all passed\n");
    return 0;
}
