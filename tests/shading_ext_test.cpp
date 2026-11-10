// 拡張シェーディング（クリアコート / 異方性 / シーン / スペキュラ・IOR）のデータの通り道の単体テスト（GPU 不要）。
//   1. PackShadingExt / UnpackShadingExt の往復（量子化誤差の範囲）・既定値なら 8 DWORD とも 0
//   2. ResolveShadingExt の優先順位 / ClampShadingExt / DielectricF0
//   3. JSON（.dxmat・シーン JSON の "material"）の読み書き
//   4. glTF の取り込み: Blender で作った glb（tools/blender/make_shading_test_glb.py）を
//      ParseGltfShading（JSON）と ReadAssimpShading（Assimp のキー）の両方で読み、値を突き合わせる
#include "renderer/ShadingExt.h"
#include "resource/GltfShadingExt.h"
#include "resource/ShadingExtJson.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) { std::printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
#define CHECK_NEAR(a, b, eps) CHECK(Near((a), (b), (eps)))

void TestPackDefault()
{
    const PackedShadingExt w = PackShadingExt(ShadingExt{});
    for (u32 v : w) CHECK(v == 0u);
    CHECK(ShadingExt{}.IsDefault());
    const DecodedShadingExt d = UnpackShadingExt(w);
    CHECK(d.flags == 0u);

    // NaN でも既定へ落ちる（Pack は Clamp を通す）
    ShadingExt n;
    n.clearCoat = std::nanf("");
    n.ior = std::nanf("");
    for (u32 v : PackShadingExt(n)) CHECK(v == 0u);
}

void TestPackRoundTrip()
{
    ShadingExt e;
    e.clearCoat = 0.75f;  e.clearCoatRoughness = 0.25f;
    e.anisotropy = -0.6f; e.anisotropyRotation = 1.5707964f;
    e.sheenColor[0] = 0.2f; e.sheenColor[1] = 0.4f; e.sheenColor[2] = 0.8f; e.sheenRoughness = 0.4f;
    e.specularFactor = 0.5f; e.specularColor[0] = 1.0f; e.specularColor[1] = 0.5f; e.specularColor[2] = 0.25f;
    e.ior = 1.33f;
    const PackedShadingExt w = PackShadingExt(e);
    CHECK((w[0] & kShadingExtMagicMask) == kShadingExtMagic);
    CHECK((w[0] & 0xFFu) == (kShadingExtCoat | kShadingExtAniso | kShadingExtSheen | kShadingExtSpecular));
    const DecodedShadingExt d = UnpackShadingExt(w);
    CHECK_NEAR(d.clearCoat, 0.75f, 1.0f / 65535.0f);
    CHECK_NEAR(d.clearCoatRoughness, 0.25f, 1.0f / 65535.0f);
    CHECK_NEAR(d.anisotropy, -0.6f, 1.0f / 32767.0f);
    CHECK_NEAR(d.anisotropyRotation, 1.5707964f, 6.2832f / 65535.0f);
    CHECK_NEAR(d.sheenColor[0], 0.2f, 1.0f / 255.0f);
    CHECK_NEAR(d.sheenColor[1], 0.4f, 1.0f / 255.0f);
    CHECK_NEAR(d.sheenColor[2], 0.8f, 1.0f / 255.0f);
    CHECK_NEAR(d.sheenRoughness, 0.4f, 1.0f / 255.0f);
    f32 f0[3], f90;
    DielectricF0(e, f0, f90);
    CHECK_NEAR(d.f0[0], f0[0], 1e-7f);
    CHECK_NEAR(d.f0[2], f0[2], 1e-7f);
    CHECK_NEAR(d.f90, 0.5f, 1e-7f);

    // 機能ごとに独立（コートだけ）
    ShadingExt c;
    c.clearCoat = 1.0f;
    const PackedShadingExt wc = PackShadingExt(c);
    CHECK((wc[0] & 0xFFu) == kShadingExtCoat);
    CHECK(wc[2] == 0u && wc[3] == 0u && wc[4] == 0u && wc[7] == 0u);
    // 異方性 -1 / +1 の端
    ShadingExt a;
    a.anisotropy = -1.0f;
    CHECK_NEAR(UnpackShadingExt(PackShadingExt(a)).anisotropy, -1.0f, 1e-6f);
    a.anisotropy = 1.0f;
    CHECK_NEAR(UnpackShadingExt(PackShadingExt(a)).anisotropy, 1.0f, 1e-6f);
}

void TestResolveAndClamp()
{
    ShadingExt base;
    base.clearCoat = 0.5f; base.clearCoatRoughness = 0.3f; base.anisotropy = 0.2f; base.ior = 1.4f;
    base.sheenColor[0] = 0.1f; base.sheenColor[1] = 0.1f; base.sheenColor[2] = 0.1f;

    ShadingExtOverride none;
    CHECK(!none.Any());
    CHECK(ResolveShadingExt(base, none) == base);

    ShadingExtOverride ov;
    ov.clearCoat = 0.9f;
    ov.anisotropySet = true; ov.anisotropy = -0.5f;
    ov.sheenColor[0] = 0.7f; ov.sheenColor[1] = 0.6f; ov.sheenColor[2] = 0.5f;
    ov.ior = 2.0f;
    CHECK(ov.Any());
    const ShadingExt r = ResolveShadingExt(base, ov);
    CHECK_NEAR(r.clearCoat, 0.9f, 1e-7f);
    CHECK_NEAR(r.clearCoatRoughness, 0.3f, 1e-7f);   // 上書きの無い欄は材質に従う
    CHECK_NEAR(r.anisotropy, -0.5f, 1e-7f);           // 負の異方性も上書きできる（anisotropySet）
    CHECK_NEAR(r.sheenColor[2], 0.5f, 1e-7f);
    CHECK_NEAR(r.ior, 2.0f, 1e-7f);
    // 異方性 0 への上書き（0 は「従う」ではなく値）
    ShadingExtOverride z;
    z.anisotropySet = true; z.anisotropy = 0.0f;
    CHECK_NEAR(ResolveShadingExt(base, z).anisotropy, 0.0f, 1e-7f);

    ShadingExt c;
    c.clearCoat = 3.0f; c.clearCoatRoughness = -2.0f; c.anisotropy = -9.0f; c.anisotropyRotation = -1.5707964f;
    c.sheenColor[0] = 5.0f; c.specularFactor = 2.0f; c.specularColor[1] = 99.0f; c.ior = 0.2f;
    ClampShadingExt(c);
    CHECK_NEAR(c.clearCoat, 1.0f, 0.0f);
    CHECK_NEAR(c.clearCoatRoughness, 0.0f, 0.0f);
    CHECK_NEAR(c.anisotropy, -1.0f, 0.0f);
    CHECK_NEAR(c.anisotropyRotation, 4.712389f, 1e-5f);   // -90 度 -> 270 度
    CHECK_NEAR(c.sheenColor[0], 1.0f, 0.0f);
    CHECK_NEAR(c.specularFactor, 1.0f, 0.0f);
    CHECK_NEAR(c.specularColor[1], 16.0f, 0.0f);
    CHECK_NEAR(c.ior, 1.0f, 0.0f);
}

void TestDielectricF0()
{
    f32 f0[3], f90 = -1.0f;
    DielectricF0(ShadingExt{}, f0, f90);
    CHECK_NEAR(f0[0], 0.04f, 1e-6f);
    CHECK_NEAR(f0[1], 0.04f, 1e-6f);
    CHECK_NEAR(f90, 1.0f, 0.0f);

    ShadingExt e;
    e.specularFactor = 0.0f;
    DielectricF0(e, f0, f90);
    CHECK_NEAR(f0[0], 0.0f, 0.0f);
    CHECK_NEAR(f90, 0.0f, 0.0f);

    ShadingExt w;
    w.ior = 1.33f;
    DielectricF0(w, f0, f90);
    CHECK_NEAR(f0[0], 0.02006f, 1e-4f);   // ((1.33-1)/(1.33+1))^2
    ShadingExt big;
    big.ior = 5.0f; big.specularColor[0] = 16.0f;
    DielectricF0(big, f0, f90);
    CHECK_NEAR(f0[0], 1.0f, 0.0f);          // F0 は 1 で頭打ち
    CHECK_NEAR(IorToF0(1.5f), 0.04f, 1e-6f);
}

void TestJson()
{
    ShadingExt e;
    nlohmann::json j = nlohmann::json::object();
    shadingjson::Write(j, e);
    CHECK(j.empty());   // 既定ならキーを書かない

    e.clearCoat = 0.6f;
    e.sheenColor[1] = 0.3f;
    shadingjson::Write(j, e);
    CHECK(j.contains("clearCoat") && j.contains("sheenColor"));
    CHECK(!j.contains("ior") && !j.contains("specularColor") && !j.contains("anisotropy"));
    ShadingExt back;
    CHECK(shadingjson::Read(j, back));
    CHECK(back == e);
    CHECK(!shadingjson::Read(nlohmann::json::parse(R"({"metallic":1})"), back));

    // エンティティの上書き: 設定した欄だけ書く / 読む
    ShadingExtOverride o;
    nlohmann::json oj = nlohmann::json::object();
    shadingjson::WriteOverride(oj, o);
    CHECK(oj.empty());
    o.clearCoat = 0.8f;
    o.anisotropySet = true; o.anisotropy = -0.4f;
    o.anisotropyRotation = 0.0f;
    o.specularColor[0] = 1.0f; o.specularColor[1] = 0.5f; o.specularColor[2] = 0.25f;
    shadingjson::WriteOverride(oj, o);
    CHECK(oj.size() == 4);
    ShadingExtOverride ob;
    shadingjson::ReadOverride(oj, ob);
    CHECK_NEAR(ob.clearCoat, 0.8f, 1e-6f);
    CHECK(ob.anisotropySet);
    CHECK_NEAR(ob.anisotropy, -0.4f, 1e-6f);
    CHECK(ob.anisotropyRotation == 0.0f);
    CHECK_NEAR(ob.specularColor[1], 0.5f, 1e-6f);
    CHECK(ob.clearCoatRoughness < 0.0f && ob.ior < 0.0f && !(ob.sheenColor[0] >= 0.0f));   // 無い欄は従う
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void TestGltfImport()
{
    const std::filesystem::path glb = std::filesystem::path(DX12E_SHADING_TEST_GLB);
    if (!std::filesystem::exists(glb)) { std::printf("[SKIP] glb が無い: %s\n", glb.string().c_str()); return; }

    const std::vector<uint8_t> bytes = ReadFileBytes(glb);
    const std::vector<GltfShadingEntry> js = ParseGltfShading(bytes.data(), bytes.size());
    CHECK(js.size() == 7);

    Assimp::Importer imp;
    const aiScene* sc = imp.ReadFile(glb.string(), 0);
    CHECK(sc != nullptr);
    if (!sc) return;
    CHECK(sc->mNumMaterials >= js.size());   // Assimp は既定材質を末尾に足すことがある（添字は先頭から一致）

    auto find = [&](const char* name) -> int {
        for (unsigned i = 0; i < sc->mNumMaterials && i < js.size(); ++i)
            if (std::string(sc->mMaterials[i]->GetName().C_Str()) == name) return static_cast<int>(i);
        return -1;
    };
    const int iPlain = find("Plain"), iCoat = find("Coat"), iSheen = find("Sheen"), iAniso = find("Aniso"),
              iSpec = find("SpecLevel"), iIor = find("Ior"), iZero = find("SpecZero");
    CHECK(iPlain >= 0 && iCoat >= 0 && iSheen >= 0 && iAniso >= 0 && iSpec >= 0 && iIor >= 0 && iZero >= 0);
    if (iPlain < 0 || iCoat < 0 || iSheen < 0 || iAniso < 0 || iSpec < 0 || iIor < 0 || iZero < 0) return;

    // ---- JSON（正）----
    CHECK(!js[iPlain].any);
    CHECK(js[iCoat].any);
    CHECK_NEAR(js[iCoat].shading.clearCoat, 0.75f, 1e-6f);
    CHECK_NEAR(js[iCoat].shading.clearCoatRoughness, 0.25f, 1e-6f);
    CHECK_NEAR(js[iSheen].shading.sheenColor[0], 0.2f, 1e-6f);
    CHECK_NEAR(js[iSheen].shading.sheenColor[2], 0.8f, 1e-6f);
    CHECK_NEAR(js[iSheen].shading.sheenRoughness, 0.4f, 1e-6f);
    CHECK_NEAR(js[iAniso].shading.anisotropy, 0.6f, 1e-6f);
    CHECK_NEAR(js[iAniso].shading.anisotropyRotation, 1.5707964f, 1e-6f);
    CHECK_NEAR(js[iSpec].shading.specularFactor, 0.5f, 1e-6f);
    CHECK_NEAR(js[iSpec].shading.specularColor[1], 0.5f, 1e-6f);
    CHECK_NEAR(js[iSpec].shading.specularColor[2], 0.25f, 1e-6f);
    CHECK_NEAR(js[iIor].shading.ior, 1.33f, 1e-6f);
    CHECK_NEAR(js[iIor].shading.specularFactor, 0.8f, 1e-6f);
    CHECK(js[iZero].any);
    CHECK_NEAR(js[iZero].shading.specularFactor, 0.0f, 0.0f);   // 鏡面を消す指定

    // ---- Assimp のキー（突き合わせ。Assimp が読む拡張は JSON と一致し、読めないものは false）----
    ShadingExt a;
    CHECK(!ReadAssimpShading(sc->mMaterials[iPlain], a));
    CHECK(ReadAssimpShading(sc->mMaterials[iCoat], a));
    CHECK_NEAR(a.clearCoat, 0.75f, 1e-6f);
    CHECK_NEAR(a.clearCoatRoughness, 0.25f, 1e-6f);
    a = ShadingExt{};
    CHECK(ReadAssimpShading(sc->mMaterials[iSheen], a));
    CHECK_NEAR(a.sheenColor[1], 0.4f, 1e-6f);
    CHECK_NEAR(a.sheenRoughness, 0.4f, 1e-6f);
    a = ShadingExt{};
    CHECK(ReadAssimpShading(sc->mMaterials[iSpec], a));
    CHECK_NEAR(a.specularFactor, 0.5f, 1e-6f);
    CHECK_NEAR(a.specularColor[2], 0.25f, 1e-6f);
    a = ShadingExt{};
    CHECK(ReadAssimpShading(sc->mMaterials[iIor], a));
    CHECK_NEAR(a.ior, 1.33f, 1e-6f);
    // Assimp が落とすもの: anisotropy（importer が書かない）/ specularFactor=0 だけの材質 -> JSON が補う
    CHECK(!ReadAssimpShading(sc->mMaterials[iAniso], a));
    CHECK(!ReadAssimpShading(sc->mMaterials[iZero], a));

    // 壊れた入力
    CHECK(ParseGltfShading(nullptr, 0).empty());
    const uint8_t junk[] = {'g', 'l', 'T', 'F', 1, 2, 3};
    CHECK(ParseGltfShading(junk, sizeof(junk)).empty());
    const std::string txt = R"({"materials":[{"extensions":{"KHR_materials_ior":{"ior":2.4}}},{}]})";
    const auto t = ParseGltfShading(reinterpret_cast<const uint8_t*>(txt.data()), txt.size());   // .gltf（JSON 本文）
    CHECK(t.size() == 2 && t[0].any && !t[1].any);
    CHECK_NEAR(t[0].shading.ior, 2.4f, 1e-6f);
}
} // namespace

int main()
{
    TestPackDefault();
    TestPackRoundTrip();
    TestResolveAndClamp();
    TestDielectricF0();
    TestJson();
    TestGltfImport();
    std::printf("shading_ext: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
