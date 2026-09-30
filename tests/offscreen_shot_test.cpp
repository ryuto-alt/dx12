// core/OffscreenShot.h（任意解像度のオフスクリーン出力の純ロジック）と、線形 HDR 出力の書式（PFM / EXR）の単体テスト。
// 依存は標準ライブラリだけ（GPU も Windows ヘッダも不要）。実行: ctest -R OffscreenShotTests
//
// 守りたいこと:
//   ・"1920x1080" 等の解釈と、不正な入力の拒否
//   ・上限（1 辺 / 総画素 / 線形 float 出力の総画素）の境界と、エラー文が原因を言うこと
//   ・GPU メモリの見積と利用可能量の照合（取得できなければ照合しない）
//   ・PFM / EXR の書式（パストレーサーと同じ writer。比較ツール tools/parity が読む規約）: 往復・行の向き・決定論
#include "core/OffscreenShot.h"
#include "renderer/pt/PtImageIO.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace dx12e::offshot;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

int main()
{
    // ---------------------------------------------------------------
    // 1) サイズの解釈
    // ---------------------------------------------------------------
    {
        uint32_t w = 0, h = 0;
        CHECK(ParseSize("1920x1080", w, h) && w == 1920 && h == 1080);
        CHECK(ParseSize("1920X1080", w, h) && w == 1920 && h == 1080);
        CHECK(ParseSize("  640 x 360  ", w, h) && w == 640 && h == 360);
        CHECK(ParseSize("3840,2160", w, h) && w == 3840 && h == 2160);
        CHECK(ParseSize("1280 720", w, h) && w == 1280 && h == 720);
        CHECK(!ParseSize("", w, h));
        CHECK(!ParseSize("1920", w, h));
        CHECK(!ParseSize("1920x", w, h));
        CHECK(!ParseSize("x1080", w, h));
        CHECK(!ParseSize("0x1080", w, h));
        CHECK(!ParseSize("1920x0", w, h));
        CHECK(!ParseSize("-1920x1080", w, h));
        CHECK(!ParseSize("1920x1080x3", w, h));
        CHECK(!ParseSize("1920x1080abc", w, h));
        CHECK(!ParseSize("abc", w, h));
        CHECK(!ParseSize("99999999999x1", w, h));   // 桁あふれ
    }

    // ---------------------------------------------------------------
    // 2) 上限
    // ---------------------------------------------------------------
    {
        CHECK(Validate(1920, 1080, false) == Verdict::Ok);
        CHECK(Validate(3840, 2160, true) == Verdict::Ok);         // 4K の線形 float も可
        CHECK(Validate(8192, 4096, false) == Verdict::Ok);        // 上限ちょうど
        CHECK(Validate(4096, 4096, true) == Verdict::Ok);         // 線形の上限ちょうど
        CHECK(Validate(15, 1080, false) == Verdict::TooSmall);
        CHECK(Validate(1920, 8, false) == Verdict::TooSmall);
        CHECK(Validate(8193, 100, false) == Verdict::TooWide);
        CHECK(Validate(100, 8193, false) == Verdict::TooWide);
        CHECK(Validate(8192, 8192, false) == Verdict::TooManyPixels);
        CHECK(Validate(8192, 4097, false) == Verdict::TooManyPixels);
        CHECK(Validate(8192, 4096, true) == Verdict::LinearTooBig);   // png ならよいが pfm/exr は不可
        CHECK(Validate(4097, 4096, true) == Verdict::LinearTooBig);
        // エラー文は原因と数値を言う
        CHECK(Describe(Verdict::TooWide, 9000, 100).find("9000x100") != std::string::npos);
        CHECK(Describe(Verdict::TooWide, 9000, 100).find("8192") != std::string::npos);
        CHECK(Describe(Verdict::LinearTooBig, 8192, 4096).find("png") != std::string::npos);
        CHECK(Describe(Verdict::NoVram, 8192, 4096).find("GPU memory") != std::string::npos);
        CHECK(Describe(Verdict::Ok, 1, 1) == "ok");
    }

    // ---------------------------------------------------------------
    // 3) GPU メモリの見積
    // ---------------------------------------------------------------
    {
        const uint64_t px1080 = 1920ull * 1080ull;
        CHECK(EstimateVramBytes(1920, 1080, false) == px1080 * kBytesPerPixel);
        CHECK(EstimateVramBytes(1920, 1080, true) == px1080 * (kBytesPerPixel + 32ull));
        CHECK(EstimateVramBytes(3840, 2160, false) == 4ull * EstimateVramBytes(1920, 1080, false));
        // 利用可能量が取得できなければ照合しない
        CHECK(CheckVram(8192, 4096, true, 0) == Verdict::Ok);
        // 8 GB の空きに 1080p は余裕、8K x 4K の PNG は 60% 枠（4.8 GB）を超える見積（5.4 GB）で拒否
        const uint64_t gb = 1024ull * 1024ull * 1024ull;
        CHECK(CheckVram(1920, 1080, false, 8 * gb) == Verdict::Ok);
        CHECK(CheckVram(8192, 4096, false, 8 * gb) == Verdict::NoVram);
        CHECK(CheckVram(8192, 4096, false, 16 * gb) == Verdict::Ok);
        // 枠の割合は kVramShare（見積 = 空き x 0.6 ちょうどの手前 / 超え）
        {
            const uint64_t need = EstimateVramBytes(4096, 2160, false);
            CHECK(CheckVram(4096, 2160, false, static_cast<uint64_t>(static_cast<double>(need) / kVramShare) + 1024) == Verdict::Ok);
            CHECK(CheckVram(4096, 2160, false, static_cast<uint64_t>(static_cast<double>(need) / kVramShare) - 1024) == Verdict::NoVram);
        }
        // 起動引数の既定（未指定 = 0）
        CHECK(Cli().w == 0 && Cli().h == 0);
    }

    // ---------------------------------------------------------------
    // 4) PFM / EXR の書式（パストレーサーと同じ writer）
    // ---------------------------------------------------------------
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "dx12e_offscreen_test";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const uint32_t w = 5, h = 3;
        std::vector<float> rgb(static_cast<size_t>(w) * h * 3);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const size_t i = (static_cast<size_t>(y) * w + x) * 3;
                rgb[i + 0] = 0.001f * static_cast<float>(x) + 100.0f * static_cast<float>(y);   // 行ごとに大きく違う値
                rgb[i + 1] = 30000.0f + static_cast<float>(x);                                   // nit 単位の大きな値
                rgb[i + 2] = 0.0f;
            }

        // PFM: 往復（値が完全に一致・row 0 が上端のまま戻る）
        const fs::path pfm = dir / "a.pfm";
        CHECK(dx12e::pt::io::WritePfm(pfm, w, h, rgb.data()));
        uint32_t rw = 0, rh = 0;
        std::vector<float> back;
        CHECK(dx12e::pt::io::ReadPfm(pfm, rw, rh, back));
        CHECK(rw == w && rh == h && back.size() == rgb.size());
        CHECK(back == rgb);
        // ヘッダ: "PF\n5 3\n-1.0\n"（リトルエンディアン）と、先頭データ行が最下行（PFM は下から上）
        {
            std::ifstream f(pfm, std::ios::binary);
            std::string magic, dims, scale;
            std::getline(f, magic); std::getline(f, dims); std::getline(f, scale);
            CHECK(magic == "PF");
            CHECK(dims == "5 3");
            CHECK(std::stof(scale) < 0.0f);
            float first[3] = {};
            f.read(reinterpret_cast<char*>(first), sizeof(first));
            CHECK(first[0] == rgb[static_cast<size_t>(h - 1) * w * 3 + 0]);   // 最下行の先頭画素
        }
        // 決定論: 同じ入力は同じバイト列
        const fs::path pfm2 = dir / "b.pfm";
        CHECK(dx12e::pt::io::WritePfm(pfm2, w, h, rgb.data()));
        {
            std::ifstream a(pfm, std::ios::binary), b(pfm2, std::ios::binary);
            std::vector<char> ba((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
            std::vector<char> bb((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());
            CHECK(!ba.empty() && ba == bb);
        }

        // EXR: マジック 0x01312F76、サイズが「ヘッダ + 行テーブル + 行データ」の整合（無圧縮 float32 = 4 B x 3ch）
        const fs::path exr = dir / "a.exr";
        CHECK(dx12e::pt::io::WriteExr(exr, w, h, rgb.data()));
        {
            std::ifstream f(exr, std::ios::binary);
            unsigned char m[4] = {};
            f.read(reinterpret_cast<char*>(m), 4);
            CHECK(m[0] == 0x76 && m[1] == 0x2f && m[2] == 0x31 && m[3] == 0x01);
            f.seekg(0, std::ios::end);
            const auto size = static_cast<uint64_t>(f.tellg());
            const uint64_t pixelBytes = static_cast<uint64_t>(w) * h * 3ull * 4ull;
            CHECK(size > pixelBytes);                                   // ヘッダとテーブルを含む
            CHECK(size < pixelBytes + 4096ull + static_cast<uint64_t>(h) * 16ull);
        }
        fs::remove_all(dir, ec);
    }

    std::printf("OffscreenShotTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
