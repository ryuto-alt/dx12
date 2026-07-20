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

int main()
{
    Test_Roundtrip();
    Test_MissingFieldsUseDefaults();
    Test_InvalidInputFails();

    std::printf("dxmat_roundtrip: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
