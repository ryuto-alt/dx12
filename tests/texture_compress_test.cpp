// BC7/BC5 テクスチャ圧縮パイプラインのテスト。GPU/D3D12デバイス不要
// (形式選択は純関数、圧縮/伸長は DirectXTex の CPU コーデック)。
//
// 見ているもの:
//   1. TextureLoader::SelectCompressedFormat の用途 → BC 形式の対応表
//   2. TextureLoader::IsCompressibleSize の 4x4 ブロック制約
//   3. BC7_UNORM の往復（圧縮 → 伸長）で色が壊れないこと
//   4. BC5_UNORM の往復 + シェーダ側と同じ z 再構成（z=sqrt(1-x^2-y^2)）で
//      法線が元に戻ること = PBR.hlsli の PerturbNormal の前提が成り立つこと
//
// 実行: ctest --output-on-failure （失敗があれば終了コード 1）

#include "resource/TextureLoader.h"

#include <DirectXTex.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;
} // namespace

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// 用途 + 元フォーマット + sRGB から BC 形式が決まること。
// 「用途不明は圧縮しない（安全側）」がこのパイプラインの一番大事な契約。
static void Test_FormatSelection()
{
    CHECK(TextureLoader::SelectCompressedFormat(
              TextureUsage::BaseColor, DXGI_FORMAT_R8G8B8A8_UNORM, true) == DXGI_FORMAT_BC7_UNORM_SRGB);
    CHECK(TextureLoader::SelectCompressedFormat(
              TextureUsage::BaseColor, DXGI_FORMAT_R8G8B8A8_UNORM, false) == DXGI_FORMAT_BC7_UNORM);
    CHECK(TextureLoader::SelectCompressedFormat(
              TextureUsage::Normal, DXGI_FORMAT_R8G8B8A8_UNORM, false) == DXGI_FORMAT_BC5_UNORM);
    CHECK(TextureLoader::SelectCompressedFormat(
              TextureUsage::NonColor, DXGI_FORMAT_R8G8B8A8_UNORM, false) == DXGI_FORMAT_BC7_UNORM);

    // 用途不明は必ず無圧縮（UI画像・アイコン・3D LUT がここを通る）
    CHECK(TextureLoader::SelectCompressedFormat(
              TextureUsage::Unknown, DXGI_FORMAT_R8G8B8A8_UNORM, true) == DXGI_FORMAT_UNKNOWN);
    CHECK(TextureLoader::SelectCompressedFormat(
              TextureUsage::Unknown, DXGI_FORMAT_R16G16B16A16_FLOAT, false) == DXGI_FORMAT_UNKNOWN);

    // HDR(float) は用途より元データの型が優先 → BC6H
    CHECK(TextureLoader::SelectCompressedFormat(
              TextureUsage::BaseColor, DXGI_FORMAT_R16G16B16A16_FLOAT, true) == DXGI_FORMAT_BC6H_UF16);
    CHECK(TextureLoader::SelectCompressedFormat(
              TextureUsage::NonColor, DXGI_FORMAT_R32G32B32A32_FLOAT, false) == DXGI_FORMAT_BC6H_UF16);
}

// D3D12 はブロック圧縮の最上位 mip に 4 の倍数を要求する。
static void Test_CompressibleSize()
{
    CHECK(TextureLoader::IsCompressibleSize(64, 64, 1, 1));
    CHECK(TextureLoader::IsCompressibleSize(4, 4, 1, 1));
    CHECK(!TextureLoader::IsCompressibleSize(2, 2, 1, 1));      // 4 未満
    CHECK(!TextureLoader::IsCompressibleSize(63, 64, 1, 1));    // 4 の倍数でない
    CHECK(!TextureLoader::IsCompressibleSize(64, 66, 1, 1));
    CHECK(!TextureLoader::IsCompressibleSize(64, 64, 6, 1));    // キューブ/配列は対象外
    CHECK(!TextureLoader::IsCompressibleSize(64, 64, 1, 4));    // 3D も対象外
}

// 64x64 の RGBA8 画像を作る（fill が 1 ピクセル分の RGBA を書く）。
template <typename Fn>
static bool MakeImage(DirectX::ScratchImage& out, size_t w, size_t h, Fn fill)
{
    if (FAILED(out.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1)))
        return false;
    const DirectX::Image* img = out.GetImage(0, 0, 0);
    if (!img) return false;
    for (size_t y = 0; y < h; ++y)
    {
        uint8_t* row = img->pixels + y * img->rowPitch;
        for (size_t x = 0; x < w; ++x)
            fill(x, y, row + x * 4);
    }
    return true;
}

// BC7 往復: なめらかなグラデーションなら誤差はごく小さい（BC7 は 4x4 ブロックあたり 128bit）。
static void Test_BC7Roundtrip()
{
    using namespace DirectX;
    ScratchImage src;
    const bool made = MakeImage(src, 64, 64, [](size_t x, size_t y, uint8_t* p) {
        p[0] = static_cast<uint8_t>(x * 4);
        p[1] = static_cast<uint8_t>(y * 4);
        p[2] = static_cast<uint8_t>((x + y) * 2);
        p[3] = 255;
    });
    CHECK(made);
    if (!made) return;

    ScratchImage compressed;
    const HRESULT hr = Compress(*src.GetImage(0, 0, 0), DXGI_FORMAT_BC7_UNORM,
                                TEX_COMPRESS_PARALLEL, TEX_THRESHOLD_DEFAULT, compressed);
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) return;

    CHECK(compressed.GetMetadata().format == DXGI_FORMAT_BC7_UNORM);
    // BC7 は 1 ピクセル 1 バイト = 非圧縮 RGBA8 の 1/4
    CHECK(compressed.GetPixelsSize() * 4 == src.GetPixelsSize());

    ScratchImage decoded;
    CHECK(SUCCEEDED(Decompress(*compressed.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM, decoded)));

    const Image* a = src.GetImage(0, 0, 0);
    const Image* b = decoded.GetImage(0, 0, 0);
    CHECK(a && b);
    if (!a || !b) return;

    int maxErr = 0;
    for (size_t y = 0; y < a->height; ++y)
    {
        const uint8_t* ra = a->pixels + y * a->rowPitch;
        const uint8_t* rb = b->pixels + y * b->rowPitch;
        for (size_t i = 0; i < a->width * 4; ++i)
        {
            const int d = std::abs(static_cast<int>(ra[i]) - static_cast<int>(rb[i]));
            if (d > maxErr) maxErr = d;
        }
    }
    if (maxErr > 8)
        std::printf("  BC7 max channel error = %d\n", maxErr);
    CHECK(maxErr <= 8);
}

// BC5 往復 + z 再構成。PBR.hlsli の PerturbNormal は b を読まず xy から z を復元するので、
// BC5(RG 2ch)へ落としても法線が元に戻ることをここで担保する。
static void Test_BC5NormalRoundtrip()
{
    using namespace DirectX;

    // 半球状に振った単位法線を (0..1) へエンコードした法線マップ
    auto encode = [](float v) { return static_cast<uint8_t>((v * 0.5f + 0.5f) * 255.0f + 0.5f); };

    ScratchImage src;
    const bool made = MakeImage(src, 64, 64, [&](size_t x, size_t y, uint8_t* p) {
        const float nx = (static_cast<float>(x) / 63.0f) * 1.4f - 0.7f;
        const float ny = (static_cast<float>(y) / 63.0f) * 1.4f - 0.7f;
        const float nz = std::sqrt((std::max)(0.0f, 1.0f - nx * nx - ny * ny));
        p[0] = encode(nx);
        p[1] = encode(ny);
        p[2] = encode(nz);
        p[3] = 255;
    });
    CHECK(made);
    if (!made) return;

    ScratchImage compressed;
    const HRESULT hr = Compress(*src.GetImage(0, 0, 0), DXGI_FORMAT_BC5_UNORM,
                                TEX_COMPRESS_PARALLEL, TEX_THRESHOLD_DEFAULT, compressed);
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) return;

    CHECK(compressed.GetMetadata().format == DXGI_FORMAT_BC5_UNORM);
    CHECK(compressed.GetPixelsSize() * 4 == src.GetPixelsSize());

    ScratchImage decoded;
    CHECK(SUCCEEDED(Decompress(*compressed.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM, decoded)));

    const Image* a = src.GetImage(0, 0, 0);
    const Image* b = decoded.GetImage(0, 0, 0);
    CHECK(a && b);
    if (!a || !b) return;

    // BC5 は B を持たない = 伸長すると b は 0 になる（＝シェーダが b を読んだら壊れる）
    CHECK(b->pixels[2] == 0);

    float maxAngleErrDeg = 0.0f;
    for (size_t y = 0; y < a->height; ++y)
    {
        const uint8_t* ra = a->pixels + y * a->rowPitch;
        const uint8_t* rb = b->pixels + y * b->rowPitch;
        for (size_t x = 0; x < a->width; ++x)
        {
            auto dec = [](uint8_t v) { return v / 255.0f * 2.0f - 1.0f; };
            const float ax = dec(ra[x * 4 + 0]), ay = dec(ra[x * 4 + 1]);
            const float az = std::sqrt((std::max)(0.0f, 1.0f - ax * ax - ay * ay));
            const float bx = dec(rb[x * 4 + 0]), by = dec(rb[x * 4 + 1]);
            const float bz = std::sqrt((std::max)(0.0f, 1.0f - bx * bx - by * by));  // シェーダと同じ再構成

            const float dot = (std::min)(1.0f, (std::max)(-1.0f, ax * bx + ay * by + az * bz));
            const float deg = std::acos(dot) * 57.2957795f;
            if (deg > maxAngleErrDeg) maxAngleErrDeg = deg;
        }
    }
    // 半球の縁(z≈0)は xy の僅かな誤差が角度に効くので、しきい値は 6 度。
    // z 再構成を忘れる(b をそのまま使う)と 90 度近くズレるので、この値でも十分に効く網。
    if (maxAngleErrDeg > 6.0f)
        std::printf("  BC5 max normal angle error = %.3f deg\n", static_cast<double>(maxAngleErrDeg));
    CHECK(maxAngleErrDeg <= 6.0f);
}

// 読み込み時の色空間指定が、実際のリソース形式へ正しく落ちること。
//
// 直したバグ: 以前は `srgb ? MakeSRGB(fmt) : fmt` で、srgb=false が
// 「sRGB を付けない」だけになっていた。ソース画像のメタデータが最初から _SRGB だと
// （最近の書き出しツールは法線や ORM にも sRGB チャンクを埋めてくる）、
// リニアで読んだつもりのテクスチャが GPU 側で sRGB デコードされる。
// 法線なら 0.5 が 0.21 になり、ORM なら粗さと金属度がまるごとずれる。
// エラーは何も出ないので気付けない。実プロジェクトで 29 枚が該当していた。
static void Test_ViewFormatStripsSrgb()
{
    using dx12e::TextureLoader;

    // ★本題: リニア要求は sRGB を「外す」こと（付けないだけでは不足）
    CHECK(TextureLoader::SelectViewFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false)
          == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(TextureLoader::SelectViewFormat(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, false)
          == DXGI_FORMAT_B8G8R8A8_UNORM);
    // BC 入力（sRGB タグ付き DDS）でも同じ
    CHECK(TextureLoader::SelectViewFormat(DXGI_FORMAT_BC7_UNORM_SRGB, false)
          == DXGI_FORMAT_BC7_UNORM);

    // sRGB 要求は付ける（従来どおり）
    CHECK(TextureLoader::SelectViewFormat(DXGI_FORMAT_R8G8B8A8_UNORM, true)
          == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    CHECK(TextureLoader::SelectViewFormat(DXGI_FORMAT_BC7_UNORM, true)
          == DXGI_FORMAT_BC7_UNORM_SRGB);

    // 既に要求どおりなら素通し（往復で形式が変わらない）
    CHECK(TextureLoader::SelectViewFormat(DXGI_FORMAT_R8G8B8A8_UNORM, false)
          == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(TextureLoader::SelectViewFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, true)
          == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

    // sRGB 版を持たない形式は、どちらの要求でも壊さない
    CHECK(TextureLoader::SelectViewFormat(DXGI_FORMAT_BC5_UNORM, false) == DXGI_FORMAT_BC5_UNORM);
    CHECK(TextureLoader::SelectViewFormat(DXGI_FORMAT_BC5_UNORM, true)  == DXGI_FORMAT_BC5_UNORM);
    CHECK(TextureLoader::SelectViewFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, true)
          == DXGI_FORMAT_R16G16B16A16_FLOAT);
}

// ---------------------------------------------------------------
// 6. サムネイル縮小の寸法計算
//    アセットブラウザは 4K テクスチャを 80px のセルに載せる。等倍で読むと
//    1 枚 85MB(mip 込み)が ResourceManager に残り続ける（エビクション無し）。
//    ここが壊れると「無言で VRAM を食う」に戻るので寸法だけは固定する。
// ---------------------------------------------------------------
void Test_ComputeDownscale()
{
    size_t w = 0, h = 0;

    // 縮小不要のケースは false（out は触らない）
    CHECK(!TextureLoader::ComputeDownscale(4096, 4096, 0, w, h));    // maxDim=0 = 無効
    CHECK(!TextureLoader::ComputeDownscale(256, 256, 256, w, h));    // ちょうど = 縮小しない
    CHECK(!TextureLoader::ComputeDownscale(128, 64, 256, w, h));     // 既に小さい
    CHECK(!TextureLoader::ComputeDownscale(0, 512, 256, w, h));      // サイズ 0
    CHECK(!TextureLoader::ComputeDownscale(512, 0, 256, w, h));

    // 正方形
    CHECK(TextureLoader::ComputeDownscale(4096, 4096, 256, w, h));
    CHECK(w == 256 && h == 256);

    // 横長：長辺が上限、縦横比は保つ
    CHECK(TextureLoader::ComputeDownscale(2048, 1024, 256, w, h));
    CHECK(w == 256 && h == 128);

    // 縦長：長辺で決まる（短辺が上限になってはいけない）
    CHECK(TextureLoader::ComputeDownscale(1024, 2048, 256, w, h));
    CHECK(w == 128 && h == 256);

    // ★極端なアスペクト比でも短辺が 0 にならない（0 だと Resize が失敗して等倍のまま通る）
    CHECK(TextureLoader::ComputeDownscale(8192, 1, 256, w, h));
    CHECK(w == 256 && h == 1);
    CHECK(TextureLoader::ComputeDownscale(1, 8192, 256, w, h));
    CHECK(w == 1 && h == 256);

    // 2 の冪でないサイズでも長辺は必ず上限以下に収まる
    CHECK(TextureLoader::ComputeDownscale(1920, 1080, 256, w, h));
    CHECK(w == 256 && h <= 256 && h == 144);
}

// ---------------------------------------------------------------
// 7. BC 圧縮ディスクキャッシュのキーが「中身だけ」で決まること
//
//    直したバグ: キーが "パス|サイズ|更新時刻" のハッシュだった。
//    git は checkout したファイルの mtime を【その時刻】に書き換えるので、
//    ブランチを行き来してテクスチャが書き戻されるだけで、中身が 1 バイトも
//    変わっていなくてもキーが変わり、assets/.texcache/ が全ミスした。
//    1 枚あたり数秒の BC7 圧縮がやり直しになるため、実プロジェクト(テクスチャ 42 枚)では
//    「ブランチを切り替えるとシーンが開くまで数十秒〜数分」になっていた。
//
//    ここが赤くなったら、キーに mtime やパスが混ざっていないか疑うこと。
// ---------------------------------------------------------------
void Test_CacheKeyIgnoresMtime()
{
    namespace fs = std::filesystem;
    using dx12e::TextureLoader;

    const fs::path dir  = fs::temp_directory_path() / "dx12_texcache_key_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / "sample.bin";

    auto write = [&](const std::vector<uint8_t>& bytes) {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    };

    // 64KB のチャンク境界をまたぐ大きさにする（チャンク送りの実装ミスを拾うため）。
    std::vector<uint8_t> data(200u * 1024u);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);

    write(data);
    const uint64_t h1 = TextureLoader::ContentHashForCacheKey(file.wstring());
    CHECK(h1 != 0);

    // ★git checkout 相当: 中身は同じまま更新時刻だけ未来へ動かす。
    //   メモ化が (サイズ, 更新時刻) で効いているので、mtime を変えると
    //   実際にファイルを読み直す経路が走る = 本番と同じ条件になる。
    fs::last_write_time(file, fs::last_write_time(file, ec) + std::chrono::hours(50), ec);
    CHECK(!ec);
    const uint64_t h2 = TextureLoader::ContentHashForCacheKey(file.wstring());
    CHECK(h2 == h1);   // ← 中身が同じならキーは動いてはいけない

    // 中身が変わったら当然キーも変わる（キャッシュが古い絵を返さないこと）
    data[data.size() / 2] ^= 0xFFu;
    write(data);
    const uint64_t h3 = TextureLoader::ContentHashForCacheKey(file.wstring());
    CHECK(h3 != h1);

    // 末尾 1 バイトの違いも拾う（先頭だけ読んで済ませる実装への退行を防ぐ）
    data.back() ^= 0x01u;
    write(data);
    CHECK(TextureLoader::ContentHashForCacheKey(file.wstring()) != h3);

    fs::remove_all(dir, ec);
}

int main()
{
    Test_FormatSelection();
    Test_ViewFormatStripsSrgb();
    Test_CompressibleSize();
    Test_BC7Roundtrip();
    Test_BC5NormalRoundtrip();
    Test_ComputeDownscale();
    Test_CacheKeyIgnoresMtime();

    std::printf("texture_compress: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
