// マテリアル AO（ORM の R / glTF occlusionTexture）の回帰テスト。GPU 不要。
//
// 守っているもの:
//   1. ModelLoader の occlusionTexture の読み取り規則
//        (a) metallicRoughness と同じ画像 → sharedWithMR（MR の R をそのまま AO にする）
//        (b) 別画像 / MR 無し             → !sharedWithMR（読み込み時に ORM を作る）
//        occlusion の無いモデルは、MR の R に暗い値が入っていても AO 扱いしない（罠）
//   2. TextureLoader::ComposeOrmPng が R=AO / G,B=元の MR に詰め、解像度違いは大きい方へ揃えること
//   3. Material.h の b2 詰め方（ResolveAoStrength / PackAoFlags）。AO なしは pbrFlags が従来と 1 ビットも違わない
//
// 素材（tests/data/、Blender 5.2 で書き出し。テクスチャは 64px / 32px）:
//   ao_orm_shared.glb  ORM 1 枚を occlusion と metallicRoughness で共有
//   ao_separate.glb    MR 64px と AO 32px が別画像
//   ao_only.glb        MR 無し（係数のみ）、occlusion だけ
//   mr_noocc.glb       MR の R に暗い帯があるが occlusionTexture は無い
#include "renderer/Material.h"
#include "resource/ModelLoader.h"
#include "resource/TextureLoader.h"

#include <DirectXTex.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <objbase.h>

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

std::string Data(const char* f) { return std::string(DX12E_TEST_DATA_DIR) + "/" + f; }

// 先頭（唯一）の AO 付きマテリアルを返す。無ければ hasOcclusion=false の空エントリ。
OcclusionProbe FirstOcc(const char* file)
{
    const auto v = ModelLoader::ProbeOcclusion(Data(file));
    if (v.empty()) { std::printf("[FAIL] %s: probe failed\n", file); ++g_failures; return {}; }
    return v[0];
}

void Test_Probe()
{
    const auto shared = FirstOcc("ao_orm_shared.glb");
    CHECK(shared.hasOcclusion && shared.hasMetalRoughness && shared.sharedWithMR);
    CHECK(std::fabs(shared.strength - 1.0f) < 1e-4f);

    const auto sep = FirstOcc("ao_separate.glb");
    CHECK(sep.hasOcclusion && sep.hasMetalRoughness && !sep.sharedWithMR);

    const auto only = FirstOcc("ao_only.glb");
    CHECK(only.hasOcclusion && !only.hasMetalRoughness && !only.sharedWithMR);

    const auto noocc = FirstOcc("mr_noocc.glb");
    CHECK(!noocc.hasOcclusion);   // MR の R が暗くても occlusionTexture が無ければ AO 扱いしない

    CHECK(!FirstOcc("cube1m.glb").hasOcclusion);             // テクスチャ無しの既存モデル
    CHECK(!FirstOcc("cube1m.obj").hasOcclusion);             // glTF 以外は対象外
}

// 単色 RGBA8 画像を PNG にして返す（fn(x,y) → RGB）
template <class F>
std::vector<uint8_t> MakePng(size_t w, size_t h, F fn)
{
    DirectX::ScratchImage img;
    img.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1);
    const DirectX::Image* im = img.GetImage(0, 0, 0);
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x)
        {
            uint8_t* p = im->pixels + y * im->rowPitch + x * 4;
            uint8_t rgb[3]; fn(x, y, rgb);
            p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2]; p[3] = 255;
        }
    DirectX::Blob blob;
    DirectX::SaveToWICMemory(*im, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), blob);
    return std::vector<uint8_t>(static_cast<const uint8_t*>(blob.GetBufferPointer()),
                                static_cast<const uint8_t*>(blob.GetBufferPointer()) + blob.GetBufferSize());
}

bool Decode(const std::vector<uint8_t>& png, DirectX::ScratchImage& out)
{
    DirectX::ScratchImage raw;   // WIC の PNG は B8G8R8A8 で返るので、チャンネル順を固定するため RGBA8 へ揃える
    if (FAILED(DirectX::LoadFromWICMemory(png.data(), png.size(), DirectX::WIC_FLAGS_NONE, nullptr, raw))) return false;
    return SUCCEEDED(DirectX::Convert(raw.GetImages(), raw.GetImageCount(), raw.GetMetadata(),
                                      DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT,
                                      DirectX::TEX_THRESHOLD_DEFAULT, out));
}

void Test_ComposeOrm()
{
    // MR 4x4: R=7（捨てられるはず）/ G=100 / B=200。AO 2x2: R=33
    const auto mr = MakePng(4, 4, [](size_t, size_t, uint8_t* c) { c[0] = 7; c[1] = 100; c[2] = 200; });
    const auto ao = MakePng(2, 2, [](size_t, size_t, uint8_t* c) { c[0] = 33; c[1] = 99; c[2] = 99; });

    std::vector<uint8_t> out; std::string err;
    CHECK(TextureLoader::ComposeOrmPng(mr.data(), mr.size(), ao.data(), ao.size(), out, err));
    DirectX::ScratchImage img;
    CHECK(Decode(out, img));
    if (img.GetImageCount() > 0)
    {
        const auto& md = img.GetMetadata();
        CHECK(md.width == 4 && md.height == 4);                        // 大きい方へ揃う
        const uint8_t* p = img.GetImage(0, 0, 0)->pixels;
        CHECK(p[0] == 33 && p[1] == 100 && p[2] == 200 && p[3] == 255); // R=AO / G,B=MR
    }

    // MR 無し（AO だけ）: G=B=255（係数がそのまま効く＝「MR 無し」の既定と矛盾しない）
    std::vector<uint8_t> out2;
    CHECK(TextureLoader::ComposeOrmPng(nullptr, 0, ao.data(), ao.size(), out2, err));
    DirectX::ScratchImage img2;
    CHECK(Decode(out2, img2));
    if (img2.GetImageCount() > 0)
    {
        const uint8_t* p = img2.GetImage(0, 0, 0)->pixels;
        CHECK(img2.GetMetadata().width == 2);
        CHECK(p[0] == 33 && p[1] == 255 && p[2] == 255);
    }

    // 壊れた入力は false（呼び出し側が警告して AO なしで続ける）
    const uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> out3;
    CHECK(!TextureLoader::ComposeOrmPng(nullptr, 0, junk, sizeof(junk), out3, err));
}

void Test_FlagPacking()
{
    Material m;
    // AO なし（既定）: どんな上書きが来ても pbrFlags は 1 ビットも変わらない
    CHECK(ResolveAoStrength(&m, -1.0f) == 0.0f);
    CHECK(ResolveAoStrength(&m, 1.0f)  == 0.0f);
    CHECK(ResolveAoStrength(nullptr, 1.0f) == 0.0f);
    CHECK(PackAoFlags(2u, ResolveAoStrength(&m, 0.7f)) == 2u);

    m.occlusionInMR = true; m.aoStrength = 0.5f;
    CHECK(std::fabs(ResolveAoStrength(&m, -1.0f) - 0.5f) < 1e-6f);   // 継承
    CHECK(ResolveAoStrength(&m, 1.0f) == 1.0f);                       // 上書き
    CHECK(ResolveAoStrength(&m, 0.0f) == 0.0f);                       // 0 = オフ
    CHECK(PackAoFlags(0u, 1.0f) == 0u);                               // MR 無し（bit1 なし）なら立てない
    const u32 f = PackAoFlags(2u | kPbrFlagEmissiveTex, 1.0f);
    CHECK((f & kPbrFlagAoInMR) != 0 && ((f >> 16) & 0xFF) == 255);
    CHECK((f & 0xFFu) == (2u | kPbrFlagEmissiveTex | kPbrFlagAoInMR));
    // アルファテストの cutoff（bit8..15）と衝突しない
    AlphaParams a; a.mode = AlphaMode::Mask; a.cutoff = 0.5f;
    const u32 g = PackAlphaTestFlags(PackAoFlags(2u, 0.5f), a);
    CHECK(((g >> 8) & 0xFF) == 128 && ((g >> 16) & 0xFF) == 128 && (g & kPbrFlagAoInMR));
}
} // namespace

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Test_Probe();
    Test_ComposeOrm();
    Test_FlagPacking();
    std::printf("ao_material_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
