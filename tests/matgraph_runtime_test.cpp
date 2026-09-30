// マテリアルグラフ G2b の純ロジック（GPU / DXC 不要）:
//   ・.dxmat の graph キー: 旧ファイルのバイト一致往復（旧経路は 1 バイトも変わらない）/ グラフ材質の往復
//   ・パラメータプールのオフセット規則（ParamPoolAllocator）
//   ・DXIL ディスクキャッシュ（ヒット / 壊れ検出 / キー）と include ハッシュ
//   ・グラフ化（従来 PBR 材質 → 標準テンプレートグラフ + インスタンス）: 12 構造すべてがコンパイルでき、旧経路の式と
//     CPU 評価で一致し、構造が同じ材質は同じ HLSL（= PSO 共有）になる / ファイル書き出しと .bak
//
// 実行: ctest -R MatGraphRuntimeTests
#include "renderer/matgraph/Compiler.h"
#include "renderer/matgraph/CpuEval.h"
#include "renderer/matgraph/GraphIO.h"
#include "renderer/matgraph/ParamPool.h"
#include "renderer/matgraph/PbrTemplate.h"
#include "renderer/matgraph/ShaderCache.h"
#include "resource/MaterialAssetIO.h"
#include "resource/MaterialGraphize.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace dx12e;
namespace mg = dx12e::matgraph;
namespace fs = std::filesystem;

namespace
{
int g_failures = 0;
int g_checks = 0;
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static bool Near(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol * (1.0f + std::fabs(a) + std::fabs(b)); }
#define CHECK_NEAR(a, b) CHECK(Near((a), (b)))

static std::vector<uint8_t> Bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

static std::string ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void WriteAll(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// ---------------------------------------------------------------------------------------------
// .dxmat（旧経路はバイト一致 / graph キー）
// ---------------------------------------------------------------------------------------------
// ★以下 3 本は「graph キーを足す前の SerializeMaterialAsset が実際に出していた出力」（G2b 着手前に実コードから採取）。
//   これを Parse → Serialize しても 1 バイトも変わらないことが、既存 .dxmat が無傷である証拠。
static const char* kLegacy1 = R"({
  "albedo": "textures/red_brick_03/red_brick_03_diff.jpg",
  "license": "CC0",
  "metalRoughness": "textures/red_brick_03/red_brick_03_arm.png",
  "metallic": 0.25,
  "name": "red_brick_03",
  "normal": "textures/red_brick_03/red_brick_03_nor_gl.png",
  "roughness": 0.75,
  "source": "Poly Haven",
  "uvTiling": [
    2.0,
    3.5
  ],
  "version": 1
})";
static const char* kLegacy2 = R"({
  "albedo": "textures/g.png",
  "emissive": "textures/g_e.png",
  "emissiveColor": [
    1.0,
    0.5,
    0.25
  ],
  "emissiveIntensity": 4.0,
  "metallic": 1.0,
  "name": "glow",
  "roughness": 1.0,
  "uvTiling": [
    1.0,
    1.0
  ],
  "version": 1
})";
static const char* kLegacy3 = R"({
  "albedo": "textures/p.png",
  "metallic": 1.0,
  "name": "plain",
  "roughness": 1.0,
  "uvTiling": [
    1.0,
    1.0
  ],
  "version": 1
})";

static void Test_LegacyBytesUnchanged()
{
    for (const char* text : {kLegacy1, kLegacy2, kLegacy3})
    {
        MaterialAssetData d;
        CHECK(ParseMaterialAsset(Bytes(text), d));
        CHECK(!d.IsGraph());
        CHECK(d.graphParams.empty());
        CHECK(SerializeMaterialAsset(d) == text);            // 旧ファイルはバイト一致で往復する
        MaterialAssetData d2;
        CHECK(ParseMaterialAsset(Bytes(SerializeMaterialAsset(d)), d2));
        CHECK(SerializeMaterialAsset(d2) == text);           // 2 周目も同じ
    }
    // 旧経路の検証は不変: テクスチャ 1 枚も無い .dxmat は無効
    MaterialAssetData out;
    CHECK(!ParseMaterialAsset(Bytes(R"({"name":"empty"})"), out));
    // "params" だけあっても graph が無ければ旧経路（params は読まれず、検証も従来どおり失敗）
    CHECK(!ParseMaterialAsset(Bytes(R"({"name":"x","params":{"A":1.0}})"), out));
    // graph が文字列でない値なら旧経路の扱い（= 無効）
    CHECK(!ParseMaterialAsset(Bytes(R"({"name":"x","graph":5})"), out));
}

static const char* kGraphMat = R"({
  "graph": "materials/rock_wet.dxmg",
  "name": "rock_wet_mossy",
  "params": {
    "Albedo": "textures/moss/moss_diff.jpg",
    "Roughness": 0.8,
    "Tint": [
      0.6,
      0.7,
      0.5,
      1.0
    ],
    "Uv2": [
      1.0,
      2.0
    ]
  },
  "source": "unit test",
  "uvTiling": [
    2.0,
    3.0
  ],
  "version": 2
})";

static void Test_GraphDxmatRoundtrip()
{
    MaterialAssetData d;
    CHECK(ParseMaterialAsset(Bytes(kGraphMat), d));
    CHECK(d.IsGraph());
    CHECK(d.graphPath == "materials/rock_wet.dxmg");
    CHECK(d.graphParams.size() == 4);
    CHECK(d.graphParams["Roughness"].kind == MaterialAssetData::GraphParam::Kind::Scalar);
    CHECK_NEAR(d.graphParams["Roughness"].v[0], 0.8f);
    CHECK(d.graphParams["Albedo"].kind == MaterialAssetData::GraphParam::Kind::Texture);
    CHECK(d.graphParams["Albedo"].texture == "textures/moss/moss_diff.jpg");
    CHECK(d.graphParams["Tint"].n == 4);
    CHECK(d.graphParams["Uv2"].n == 2);
    CHECK_NEAR(d.uvTilingU, 2.0f);
    // 代理値（影・深度・パストレ用）の既定は 0 / 0.5（従来の 1 / 1 ではない）
    CHECK_NEAR(d.metallic, 0.0f);
    CHECK_NEAR(d.roughness, 0.5f);
    CHECK(d.albedoPath.empty());
    // バイト一致の往復（0.8 は "0.8" のまま。0.800000011920929 にならない）
    CHECK(SerializeMaterialAsset(d) == kGraphMat);

    // graph だけの最小ファイル（params 無し / uvTiling 無し）は有効で、往復してもキーが増えない
    const std::string minimal = "{\n  \"graph\": \"g.dxmg\",\n  \"name\": \"m\",\n  \"version\": 2\n}";
    MaterialAssetData m;
    CHECK(ParseMaterialAsset(Bytes(minimal), m));
    CHECK(m.IsGraph());
    CHECK(SerializeMaterialAsset(m) == minimal);

    // 型が合わない params は読み捨てる（ロードは止めない）
    MaterialAssetData bad;
    CHECK(ParseMaterialAsset(Bytes(R"({"graph":"g.dxmg","params":{"a":true,"b":[1],"c":[1,"x"],"d":{"z":1},"ok":2}})"), bad));
    CHECK(bad.graphParams.size() == 1 && bad.graphParams.count("ok") == 1);

    // ParamOverrides への変換（3 成分の Vector は w = 1）
    MaterialAssetData v;
    v.graphPath = "g.dxmg";
    MaterialAssetData::GraphParam gp; gp.kind = MaterialAssetData::GraphParam::Kind::Vector; gp.n = 3; gp.v[0] = 1; gp.v[1] = 2; gp.v[2] = 3;
    v.graphParams["c"] = gp;
    const mg::ParamOverrides ov = ToParamOverrides(v);
    CHECK(ov.count("c") == 1 && Near(ov.at("c").v[3], 1.0f) && !ov.at("c").isTexture);
}

// ---------------------------------------------------------------------------------------------
// パラメータプールのオフセット規則
// ---------------------------------------------------------------------------------------------
static void Test_PoolAllocator()
{
    mg::ParamPoolAllocator p(100);
    uint32_t a = 99, b = 99, c = 99, d = 99;
    CHECK(!p.Alloc(0, a));                                  // 長さ 0 は割り当てない
    CHECK(p.Alloc(10, a) && a == 0);                        // 最初は 0 から詰める
    CHECK(p.Alloc(20, b) && b == 10);
    CHECK(p.Alloc(30, c) && c == 30);
    CHECK(p.Used() == 60 && p.HighWater() == 60);
    CHECK(p.Free(10, 20));                                  // 真ん中を解放
    CHECK(!p.Free(10, 20));                                 // 二重解放は拒否
    CHECK(p.Alloc(15, d) && d == 10);                       // 穴の先頭から再利用
    CHECK(p.Alloc(5, d) && d == 25);                        // 穴の残り（25..30）
    CHECK(p.Free(0, 10) && p.Free(10, 15) && p.Free(25, 5));   // 隣接は全部マージされる
    CHECK(p.FreeRanges() == 2);                             // [0,30) と [60,100)
    CHECK(p.Free(30, 30));
    CHECK(p.FreeRanges() == 1 && p.Used() == 0);            // 全部戻せば 1 個の空き
    CHECK(p.Alloc(100, a) && a == 0);                       // 容量ちょうど
    CHECK(!p.Alloc(1, b));                                  // 超えたら失敗
    CHECK(!p.Free(90, 20));                                 // 範囲外の解放は拒否
    // 決定論: 同じ操作列は同じオフセット
    mg::ParamPoolAllocator q1(64), q2(64);
    uint32_t x1[4], x2[4];
    for (int i = 0; i < 4; ++i) { CHECK(q1.Alloc(7 + i, x1[i])); CHECK(q2.Alloc(7 + i, x2[i])); }
    CHECK(x1[0] == x2[0] && x1[1] == x2[1] && x1[2] == x2[2] && x1[3] == x2[3]);
}

// ---------------------------------------------------------------------------------------------
// DXIL ディスクキャッシュ / キー / include ハッシュ
// ---------------------------------------------------------------------------------------------
static void Test_ShaderCache(const fs::path& tmp)
{
    const fs::path dir = tmp / "cache";
    mg::ShaderDiskCache cache(dir.generic_string());
    mg::ShaderKeyInput in;
    in.hlsl = "void UnoMatEval(){}";
    in.includeHash = 123;
    in.dxcVersion = "1.8.2502";
    in.engineVersion = "9.9.9";
    in.kind = "ps";
    in.extra = "ps_6_6|-HV 2021";
    const uint64_t key = mg::MakeShaderKey(in);

    std::vector<uint8_t> out;
    CHECK(!cache.Load(key, "ps", out));                     // 最初は無い
    CHECK(cache.misses == 1 && cache.hits == 0);
    std::vector<uint8_t> blob(4096);
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(i * 7 + 3);
    CHECK(cache.Store(key, "ps", blob));
    CHECK(cache.Load(key, "ps", out) && out == blob);       // 保存したものが戻る
    CHECK(cache.hits == 1 && cache.stores == 1);
    CHECK(!cache.Load(key, "vs", out));                     // 種別が違えば別ファイル

    // 別のインスタンス（別プロセス相当）から読める
    mg::ShaderDiskCache cache2(dir.generic_string());
    CHECK(cache2.Load(key, "ps", out) && out == blob);

    // 壊れたファイルは捨てる（ミス扱い + corrupt カウント）。描画は止めない
    {
        std::string raw = ReadAll(cache.PathFor(key, "ps"));
        raw[raw.size() / 2] = static_cast<char>(raw[raw.size() / 2] ^ 0x5a);
        WriteAll(cache.PathFor(key, "ps"), raw);
    }
    CHECK(!cache2.Load(key, "ps", out));
    CHECK(cache2.corrupt == 1);
    WriteAll(cache.PathFor(key, "ps"), "xx");               // 短すぎる
    CHECK(!cache2.Load(key, "ps", out));
    CHECK(cache.Store(key, "ps", blob) && cache2.Load(key, "ps", out));   // 上書きで復旧

    // キー: どの要素が変わっても別キー
    auto k = [&](auto edit) { mg::ShaderKeyInput t = in; edit(t); return mg::MakeShaderKey(t); };
    CHECK(k([](mg::ShaderKeyInput& t) { t.hlsl += " "; }) != key);
    CHECK(k([](mg::ShaderKeyInput& t) { t.includeHash = 124; }) != key);
    CHECK(k([](mg::ShaderKeyInput& t) { t.dxcVersion = "1.8.2503"; }) != key);
    CHECK(k([](mg::ShaderKeyInput& t) { t.engineVersion = "9.9.10"; }) != key);
    CHECK(k([](mg::ShaderKeyInput& t) { t.kind = "vs"; }) != key);
    CHECK(k([](mg::ShaderKeyInput& t) { t.extra = "ps_6_6"; }) != key);
    CHECK(k([](mg::ShaderKeyInput&) {}) == key);            // 同じ入力は同じキー

    // include ハッシュ: 辿った先のファイルが変わればハッシュが変わる。コメントアウトは辿らない
    const fs::path inc = tmp / "inc";
    WriteAll(inc / "a.hlsli", "#include \"sub/b.hlsli\"\nfloat a;\n");
    WriteAll(inc / "sub" / "b.hlsli", "float b;\n");
    WriteAll(inc / "unused.hlsli", "float never;\n");
    const std::string src = "#include \"a.hlsli\"\n// #include \"unused.hlsli\"\n#include \"missing.hlsli\"\n";
    const mg::IncludeScan s1 = mg::ScanIncludes(src, {inc.generic_string()});
    CHECK(s1.files.size() == 2);
    CHECK(s1.missing.size() == 1 && s1.missing[0] == "missing.hlsli");
    const mg::IncludeScan s1b = mg::ScanIncludes(src, {inc.generic_string()});
    CHECK(s1.hash == s1b.hash);
    WriteAll(inc / "sub" / "b.hlsli", "float b2;\n");
    const mg::IncludeScan s2 = mg::ScanIncludes(src, {inc.generic_string()});
    CHECK(s2.hash != s1.hash);
    WriteAll(inc / "unused.hlsli", "float never2;\n");     // 辿らないファイルの変更はハッシュに効かない
    CHECK(mg::ScanIncludes(src, {inc.generic_string()}).hash == s2.hash);
}

// ---------------------------------------------------------------------------------------------
// グラフ化
// ---------------------------------------------------------------------------------------------
static MaterialAssetData MakeLegacy(bool n, bool m, int e, float metallic, float roughness)
{
    MaterialAssetData d;
    d.name = "mat";
    d.albedoPath = "textures/a/a_diff.jpg";
    if (n) d.normalPath = "textures/a/a_nor.png";
    if (m) d.metalRoughnessPath = "textures/a/a_arm.png";
    if (e == 2) d.emissivePath = "textures/a/a_emi.png";
    if (e >= 1) { d.emissiveColor[0] = 1.0f; d.emissiveColor[1] = 0.5f; d.emissiveColor[2] = 0.25f; d.emissiveIntensity = 3.0f; }
    d.metallic = metallic;
    d.roughness = roughness;
    return d;
}

// 従来の Forward.hlsl の式（テクスチャ = モック）
struct Mock
{
    float albedo[4]   = {0.2f, 0.4f, 0.6f, 1.0f};
    float normal[4]   = {0.6f, 0.4f, 1.0f, 1.0f};
    float mr[4]       = {0.9f, 0.3f, 0.7f, 1.0f};
    float emissive[4] = {0.5f, 0.25f, 1.0f, 1.0f};
    float vcol[4]     = {0.9f, 0.8f, 0.7f, 1.0f};
};

static void Test_Graphize()
{
    const Mock mock;
    int compiled = 0;
    std::vector<uint64_t> hashes;
    for (int n = 0; n < 2; ++n)
    for (int m = 0; m < 2; ++m)
    for (int e = 0; e < 3; ++e)
    {
        const MaterialAssetData legacy = MakeLegacy(n, m, e, 0.35f, 0.65f);
        const GraphizeResult gr = GraphizeLegacyMaterial(legacy);
        CHECK(gr.ok);
        if (!gr.ok) continue;
        char want[64];
        std::snprintf(want, sizeof(want), "materials/_graphs/pbr_std_n%dm%de%d.dxmg", n, m, e);
        CHECK(gr.templateRel == want);
        CHECK(gr.instance.IsGraph() && gr.instance.graphPath == want);

        // テンプレートを読み直してコンパイルできる（往復も正準形）
        mg::MaterialGraph g;
        std::string err;
        CHECK(mg::LoadDxmg(gr.templateText, g, &err));
        CHECK(mg::SaveDxmg(g, false) == gr.templateText);
        const mg::CompileResult cr = mg::CompileGraph(g);
        CHECK(cr.ok);
        if (!cr.ok) { for (const auto& dg : cr.diagnostics) std::printf("  diag %s %s: %s\n", dg.code.c_str(), dg.nodeId.c_str(), dg.message.c_str()); continue; }
        ++compiled;
        hashes.push_back(cr.hash);
        CHECK(!cr.HasErrors());
        // 警告も出ない（テンプレートは型をきれいに揃えてある）
        for (const auto& dg : cr.diagnostics) CHECK(dg.severity == mg::Severity::Info);

        // 全パラメータがスロットにあり、インスタンスの params と 1 対 1（名前の食い違いが無い = 黙って無効な上書きが無い）
        for (const auto& [name, p] : gr.instance.graphParams)
        {
            (void)p;
            CHECK(cr.FindSlotByName(name) != nullptr);
        }
        for (const auto& s : cr.slots)
            if (!s.name.empty()) CHECK(gr.instance.graphParams.count(s.name) == 1 || s.name == "Albedo");

        // ---- 旧経路の式と CPU 評価が一致する -------------------------------------------------
        mg::EvalEnv env;
        env.uv[0] = 0.3f; env.uv[1] = 0.7f;
        for (int i = 0; i < 4; ++i) env.vertexColor[i] = mock.vcol[i];
        auto slotName = [&](int slot) -> std::string { for (const auto& s : cr.slots) if (s.slot == slot) return s.name; return {}; };
        env.sampleTexture = [&](int slot, const float*, float, float* rgba) {
            const std::string nm = slotName(slot);
            const float* src = nm == "Albedo" ? mock.albedo : nm == "Normal" ? mock.normal : nm == "MetalRoughness" ? mock.mr : mock.emissive;
            for (int i = 0; i < 4; ++i) rgba[i] = src[i];
        };
        const mg::ParamOverrides ov = ToParamOverrides(gr.instance);
        const mg::CpuEvalResult ev = mg::EvaluateCpu(cr, env, &ov);
        CHECK(ev.ok);
        // BaseColor = albedo.rgb * vertexColor.rgb
        for (int i = 0; i < 3; ++i) CHECK_NEAR(ev.surface.baseColor[i], mock.albedo[i] * mock.vcol[i]);
        // Roughness / Metallic（MR あり: テクスチャ G / B × 係数。無し: 係数）
        CHECK_NEAR(ev.surface.roughness, m ? mock.mr[1] * legacy.roughness : legacy.roughness);
        CHECK_NEAR(ev.surface.metallic,  m ? mock.mr[2] * legacy.metallic  : legacy.metallic);
        // 法線: xy = rgb.xy * 2 - 1、z = sqrt(saturate(1 - dot(xy, xy)))
        CHECK(ev.surface.hasNormal == (n == 1));
        if (n)
        {
            const float x = mock.normal[0] * 2 - 1, y = mock.normal[1] * 2 - 1;
            CHECK_NEAR(ev.surface.normalTS[0], x);
            CHECK_NEAR(ev.surface.normalTS[1], y);
            CHECK_NEAR(ev.surface.normalTS[2], std::sqrt(std::fmax(0.0f, 1.0f - (x * x + y * y))));
        }
        // 発光: テクスチャ（あれば）× 色 × 強度
        for (int i = 0; i < 3; ++i)
        {
            const float col[3] = {1.0f, 0.5f, 0.25f};
            const float want2 = e == 0 ? 0.0f : col[i] * 3.0f * (e == 2 ? mock.emissive[i] : 1.0f);
            CHECK_NEAR(ev.surface.emissive[i], want2);
        }
    }
    CHECK(compiled == 12);

    // 構造が同じなら値が違っても同じ HLSL（= PSO 共有）。構造が違えば別 HLSL
    {
        mg::MaterialGraph g1, g2, g3;
        auto build = [](const MaterialAssetData& d, mg::MaterialGraph& g) {
            const GraphizeResult gr = GraphizeLegacyMaterial(d);
            std::string err;
            return mg::LoadDxmg(gr.templateText, g, &err) && mg::CompileGraph(g).ok;
        };
        CHECK(build(MakeLegacy(true, true, 0, 0.1f, 0.2f), g1));
        CHECK(build(MakeLegacy(true, true, 0, 0.9f, 0.8f), g2));
        CHECK(build(MakeLegacy(false, true, 0, 0.9f, 0.8f), g3));
        CHECK(mg::CompileGraph(g1).hash == mg::CompileGraph(g2).hash);
        CHECK(mg::CompileGraph(g1).hash != mg::CompileGraph(g3).hash);
        CHECK(mg::CompileGraph(g1).hlsl == mg::CompileGraph(g2).hlsl);
    }
    // 12 通りのハッシュは全部別
    for (size_t i = 0; i < hashes.size(); ++i)
        for (size_t j = i + 1; j < hashes.size(); ++j) CHECK(hashes[i] != hashes[j]);

    // 既にグラフ材質なら変換しない
    {
        MaterialAssetData g;
        g.graphPath = "x.dxmg";
        CHECK(!GraphizeLegacyMaterial(g).ok);
    }
}

static void Test_GraphizeFile(const fs::path& tmp)
{
    const fs::path assets = tmp / "assets";
    const std::string rel = "materials/brick.dxmat";
    WriteAll(assets / rel, kLegacy1);
    const std::string assetsDir = assets.generic_string() + "/";

    // dryRun は何も書かない
    GraphizeFileResult dry = GraphizeMaterialFile(assetsDir, rel, true);
    CHECK(dry.conv.ok && !dry.wroteInstance && !dry.wroteBackup && !dry.wroteTemplate);
    CHECK(ReadAll(assets / rel) == kLegacy1);
    CHECK(!fs::exists(assets / "materials/brick.dxmat.bak"));

    GraphizeFileResult r = GraphizeMaterialFile(assetsDir, rel, false);
    CHECK(r.conv.ok && r.wroteInstance && r.wroteBackup && r.wroteTemplate);
    CHECK(ReadAll(assets / "materials/brick.dxmat.bak") == kLegacy1);          // 原本は 1 バイトも変わらず退避される
    CHECK(fs::exists(assets / r.conv.templateRel));
    MaterialAssetData inst;
    CHECK(ParseMaterialAsset(Bytes(ReadAll(assets / rel)), inst));
    CHECK(inst.IsGraph() && inst.graphPath == r.conv.templateRel);
    CHECK(inst.graphParams.count("Albedo") && inst.graphParams["Albedo"].texture == "textures/red_brick_03/red_brick_03_diff.jpg");
    CHECK(inst.graphParams.count("Normal") && inst.graphParams.count("MetalRoughness"));
    CHECK_NEAR(inst.graphParams["Roughness"].v[0], 0.75f);
    CHECK_NEAR(inst.graphParams["Metallic"].v[0], 0.25f);
    CHECK_NEAR(inst.uvTilingU, 2.0f);                                          // uvTiling は初期値として引き継ぐ
    CHECK(inst.source == "Poly Haven" && inst.license == "CC0");

    // 2 回目（既にグラフ材質）は変換しない。.bak は最初の原本のまま
    GraphizeFileResult again = GraphizeMaterialFile(assetsDir, rel, false);
    CHECK(!again.conv.ok && !again.wroteInstance);
    CHECK(ReadAll(assets / "materials/brick.dxmat.bak") == kLegacy1);

    // 同じ構造の 2 枚目は同じテンプレートを共有する（テンプレートは書き直さない）
    WriteAll(assets / "materials/brick2.dxmat", kLegacy1);
    GraphizeFileResult r2 = GraphizeMaterialFile(assetsDir, "materials/brick2.dxmat", false);
    CHECK(r2.conv.ok && r2.conv.templateRel == r.conv.templateRel && !r2.wroteTemplate);

    // 読めない / 解釈できないファイル
    CHECK(!GraphizeMaterialFile(assetsDir, "materials/none.dxmat", false).conv.ok);
    WriteAll(assets / "materials/bad.dxmat", "{ nope");
    CHECK(!GraphizeMaterialFile(assetsDir, "materials/bad.dxmat", false).conv.ok);
}

int main(int argc, char** argv)
{
    // CLI: --dump-template <n0|n1><m0|m1><e0|e1|e2> <out.dxmg>   標準テンプレートグラフを書き出す（手動確認 / DXC の試験用）
    if (argc >= 4 && std::string(argv[1]) == "--dump-template")
    {
        const std::string k = argv[2];
        mg::LegacyPbr m;
        m.albedo = "textures/a.png";
        if (k.size() >= 6 && k[1] == '1') m.normal = "textures/n.png";
        if (k.size() >= 6 && k[3] == '1') m.metalRoughness = "textures/m.png";
        if (k.size() >= 6 && k[5] >= '1') { m.emissiveIntensity = 1.0f; }
        if (k.size() >= 6 && k[5] == '2') m.emissive = "textures/e.png";
        mg::MaterialGraph g;
        mg::BuildPbrTemplateGraph(m, g);
        WriteAll(argv[3], mg::SaveDxmg(g, false));
        std::printf("wrote %s (%s)\n", argv[3], mg::PbrTemplateName(m).c_str());
        return 0;
    }
    const fs::path tmp = fs::temp_directory_path() / ("matgraph_runtime_test_" + std::to_string(std::rand() ^ 0x5bd1e995));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    Test_LegacyBytesUnchanged();
    Test_GraphDxmatRoundtrip();
    Test_PoolAllocator();
    Test_ShaderCache(tmp);
    Test_Graphize();
    Test_GraphizeFile(tmp);

    fs::remove_all(tmp, ec);
    std::printf("matgraph_runtime: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
