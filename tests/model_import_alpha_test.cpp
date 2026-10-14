// モデル取り込みの 2 点の回帰テスト（GPU 不要）。
//   1. ModelImportOptions: `<モデル名>.import.json` のパース（あり / なし / 壊れた JSON / 型違い）
//   2. TextureLoader::HasTransparentAlpha: albedo のアルファから Mask を自動判定する土台
//        全不透明 → false / 切り抜き → true / アルファ無し形式 → false（無圧縮・BC7 圧縮後の両方）
#include "resource/ModelLoader.h"
#include "resource/TextureLoader.h"

#include <DirectXTex.h>

#include <cstdio>
#include <cstring>
#include <string>

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

std::filesystem::path Data(const char* f) { return std::filesystem::path(DX12E_TEST_DATA_DIR) / f; }

// 64x64 の RGBA8。alphaMode: 0=全 255、1=左半分だけ 0（切り抜き）
DirectX::ScratchImage MakeRgba(int alphaMode, DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM)
{
    DirectX::ScratchImage img;
    img.Initialize2D(fmt, 64, 64, 1, 1);
    const DirectX::Image* im = img.GetImage(0, 0, 0);
    for (size_t y = 0; y < im->height; ++y)
    {
        uint8_t* row = im->pixels + y * im->rowPitch;
        for (size_t x = 0; x < im->width; ++x)
        {
            row[x * 4 + 0] = static_cast<uint8_t>(x * 4);
            row[x * 4 + 1] = static_cast<uint8_t>(y * 4);
            row[x * 4 + 2] = 128;
            row[x * 4 + 3] = (alphaMode == 1 && x < 32) ? 0 : 255;
        }
    }
    return img;
}

// PNG にエンコードして WIC で読み直す（実際の読み込み経路と同じ形にする）
DirectX::ScratchImage ThroughPng(const DirectX::ScratchImage& src)
{
    DirectX::Blob blob;
    DirectX::ScratchImage out;
    if (SUCCEEDED(DirectX::SaveToWICMemory(*src.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
                                           DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), blob)))
        DirectX::LoadFromWICMemory(blob.GetBufferPointer(), blob.GetBufferSize(), DirectX::WIC_FLAGS_NONE, nullptr, out);
    return out;
}

DirectX::ScratchImage Bc7(const DirectX::ScratchImage& src)
{
    DirectX::ScratchImage out;
    const HRESULT hr = DirectX::Compress(src.GetImages(), src.GetImageCount(), src.GetMetadata(),
                                         DXGI_FORMAT_BC7_UNORM, DirectX::TEX_COMPRESS_BC7_QUICK, DirectX::TEX_THRESHOLD_DEFAULT, out);
    CHECK(SUCCEEDED(hr));
    return out;
}

void Test_ImportOptions()
{
    CHECK(ModelImportOptions::LoadFor(Data("import_orm_on.fbx")).specularIsORM);
    CHECK(!ModelImportOptions::LoadFor(Data("import_orm_off.fbx")).specularIsORM);          // 明示 false
    CHECK(!ModelImportOptions::LoadFor(Data("import_orm_missing.fbx")).specularIsORM);      // ファイル無し
    CHECK(!ModelImportOptions::LoadFor(Data("import_orm_broken.fbx")).specularIsORM);       // 壊れた JSON
    CHECK(!ModelImportOptions::LoadFor(Data("import_orm_wrongtype.fbx")).specularIsORM);    // 型違い

    CHECK(ModelImportOptions::Parse("{\"specularIsORM\":true}").specularIsORM);
    CHECK(!ModelImportOptions::Parse("{}").specularIsORM);
    CHECK(!ModelImportOptions::Parse("").specularIsORM);
    CHECK(!ModelImportOptions::Parse("[true]").specularIsORM);
    CHECK(!ModelImportOptions::Parse("{\"specularIsORM\":").specularIsORM);
}

void Test_Alpha()
{
    const auto opaque = ThroughPng(MakeRgba(0));
    const auto cutout = ThroughPng(MakeRgba(1));
    CHECK(opaque.GetImageCount() == 1 && cutout.GetImageCount() == 1);
    CHECK(!TextureLoader::HasTransparentAlpha(opaque));
    CHECK(TextureLoader::HasTransparentAlpha(cutout));

    // BC7 圧縮後（.texcache から読んだ BC 画像と同じ形）でも判定できる
    CHECK(!TextureLoader::HasTransparentAlpha(Bc7(opaque)));
    CHECK(TextureLoader::HasTransparentAlpha(Bc7(cutout)));

    // アルファ無し形式（B8G8R8X8）はピクセル値に関わらず false
    CHECK(!TextureLoader::HasTransparentAlpha(MakeRgba(1, DXGI_FORMAT_B8G8R8X8_UNORM)));
    // BC1（アルファ無し）の全不透明
    {
        DirectX::ScratchImage bc1;
        const HRESULT hr = DirectX::Compress(opaque.GetImages(), opaque.GetImageCount(), opaque.GetMetadata(),
                                             DXGI_FORMAT_BC1_UNORM, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, bc1);
        CHECK(SUCCEEDED(hr));
        CHECK(!TextureLoader::HasTransparentAlpha(bc1));
    }
    // 空の ScratchImage
    CHECK(!TextureLoader::HasTransparentAlpha(DirectX::ScratchImage{}));
}
} // namespace

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Test_ImportOptions();
    Test_Alpha();
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
