// .dxmat（マテリアルアセット）JSON往復テスト。GPU/D3D12デバイス不要
// (MaterialAssetIOは純ロジックでSRV/テクスチャに一切触れない)。
//
// 実行: ctest --output-on-failure （失敗があれば終了コード 1）

#include "resource/MaterialAssetIO.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

bool feq(float a, float b)
{
    return std::fabs(a - b) <= 1e-4f * (1.0f + std::fabs(a) + std::fabs(b));
}
} // namespace

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_F(a, b) CHECK(feq((a), (b)))

static std::vector<uint8_t> ToBytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Serialize→Parse の往復で全フィールドが一致することを確認する。
static void Test_Roundtrip()
{
    MaterialAssetData src;
    src.name = "red_brick_03";
    src.albedoPath = "textures/red_brick_03/red_brick_03_diff.jpg";
    src.normalPath = "textures/red_brick_03/red_brick_03_nor_gl.png";
    src.metalRoughnessPath = "textures/red_brick_03/red_brick_03_arm.png";
    src.metallic = 0.25f;
    src.roughness = 0.75f;
    src.uvTilingU = 2.0f;
    src.uvTilingV = 3.5f;
    src.aoStrength = 0.6f;
    src.source = "Poly Haven";
    src.license = "CC0";

    const std::string json = SerializeMaterialAsset(src);
    CHECK(!json.empty());

    MaterialAssetData out;
    const bool ok = ParseMaterialAsset(ToBytes(json), out);
    CHECK(ok);
    if (ok)
    {
        CHECK(out.name == "red_brick_03");
        CHECK(out.albedoPath == src.albedoPath);
        CHECK(out.normalPath == src.normalPath);
        CHECK(out.metalRoughnessPath == src.metalRoughnessPath);
        CHECK_F(out.metallic, 0.25f);
        CHECK_F(out.roughness, 0.75f);
        CHECK_F(out.uvTilingU, 2.0f);
        CHECK_F(out.uvTilingV, 3.5f);
        CHECK_F(out.aoStrength, 0.6f);
        CHECK(out.source == "Poly Haven");
        CHECK(out.license == "CC0");
    }
}

// normal/metalRoughness/source/license 等の任意フィールドを省略しても既定値で読める(後方互換)。
static void Test_MissingFieldsUseDefaults()
{
    const std::string json = R"({"name":"minimal","albedo":"textures/x/x_diff.jpg"})";
    MaterialAssetData out;
    const bool ok = ParseMaterialAsset(ToBytes(json), out);
    CHECK(ok);
    if (ok)
    {
        CHECK(out.name == "minimal");
        CHECK(out.albedoPath == "textures/x/x_diff.jpg");
        CHECK(out.normalPath.empty());
        CHECK(out.metalRoughnessPath.empty());
        CHECK_F(out.metallic, 1.0f);
        CHECK_F(out.roughness, 1.0f);
        CHECK_F(out.uvTilingU, 1.0f);
        CHECK_F(out.uvTilingV, 1.0f);
        CHECK_F(out.aoStrength, 0.0f);   // 既定 = AO を読まない（既存 .dxmat は不変）
        CHECK(out.source.empty());
        CHECK(out.license.empty());
    }
}

// 壊れたJSON/空バイト列/非オブジェクトはfalseを返す(呼び出し側がデフォルトマテリアルへ
// フォールバックできるようにするための契約)。
static void Test_InvalidInputFails()
{
    MaterialAssetData out;
    CHECK(!ParseMaterialAsset({}, out));                                  // 空バイト列
    CHECK(!ParseMaterialAsset(ToBytes("{ not json"), out));               // 壊れたJSON
    CHECK(!ParseMaterialAsset(ToBytes("[1,2,3]"), out));                  // オブジェクトでない
    CHECK(!ParseMaterialAsset(ToBytes(R"({"name":"empty"})"), out));      // テクスチャ参照なし = 無効
}

// POM の項目: 無いときは出力 JSON が従来と同一（キーが増えない）、あるときは往復で一致、範囲外はクランプ。
static void Test_Parallax()
{
    MaterialAssetData plain;
    plain.name = "plain";
    plain.albedoPath = "textures/a.png";
    const std::string jsonPlain = SerializeMaterialAsset(plain);
    CHECK(jsonPlain.find("height") == std::string::npos);
    CHECK(jsonPlain.find("parallax") == std::string::npos);
    // 既定値のまま読んで書き戻しても同一
    MaterialAssetData reread;
    CHECK(ParseMaterialAsset(ToBytes(jsonPlain), reread));
    CHECK(reread.heightPath.empty() && reread.parallaxScale == 0.0f && reread.parallaxSteps == 24);
    CHECK(SerializeMaterialAsset(reread) == jsonPlain);

    MaterialAssetData src = plain;
    src.heightPath = "textures/a/a_disp_4k.png";
    src.parallaxScale = 0.06f;
    src.parallaxSteps = 32;
    const std::string json = SerializeMaterialAsset(src);
    MaterialAssetData out;
    CHECK(ParseMaterialAsset(ToBytes(json), out));
    CHECK(out.heightPath == src.heightPath);
    CHECK_F(out.parallaxScale, 0.06f);
    CHECK(out.parallaxSteps == 32);
    CHECK(SerializeMaterialAsset(out) == json);          // 安定（書き戻しで変わらない）

    // 手書き JSON・範囲外はクランプ。steps 省略 = 既定
    const std::string hand = R"({"albedo":"x.png","height":"h.png","parallaxScale":9.0,"parallaxSteps":1000})";
    MaterialAssetData h;
    CHECK(ParseMaterialAsset(ToBytes(hand), h));
    CHECK_F(h.parallaxScale, 0.25f);
    CHECK(h.parallaxSteps == 64);
    const std::string hand2 = R"({"albedo":"x.png","height":"h.png","parallaxScale":0.04})";
    MaterialAssetData h2;
    CHECK(ParseMaterialAsset(ToBytes(hand2), h2));
    CHECK(h2.parallaxSteps == 24);
    // 型違いは読み捨て（ロードは通る）
    const std::string bad = R"({"albedo":"x.png","height":5,"parallaxScale":"deep"})";
    MaterialAssetData b;
    CHECK(ParseMaterialAsset(ToBytes(bad), b));
    CHECK(b.heightPath.empty() && b.parallaxScale == 0.0f);
    // グラフ材質は POM 非対応（書いても無視）
    const std::string graph = R"({"graph":"g.dxmg","height":"h.png","parallaxScale":0.1})";
    MaterialAssetData g;
    CHECK(ParseMaterialAsset(ToBytes(graph), g));
    CHECK(g.heightPath.empty() && g.parallaxScale == 0.0f);
}

int main()
{
    Test_Parallax();
    Test_Roundtrip();
    Test_MissingFieldsUseDefaults();
    Test_InvalidInputFails();

    std::printf("dxmat_roundtrip: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
