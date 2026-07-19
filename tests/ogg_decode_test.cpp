// OGG Vorbis デコード（third_party/stb_vorbis.c、AudioClip の .ogg 対応の土台）の単体テスト。
// 既知の埋め込み OGG（0.25 秒 / 22050Hz / モノラル、ogg_test_data.h）をメモリからデコードし、
// チャンネル数・サンプルレート・PCM 長が期待どおりであることを確認する。
// stb_vorbis はシングルファイル・依存ゼロなのでスタンドアロン構成でも動く。
#pragma warning(push)
#pragma warning(disable: 4100 4127 4189 4244 4245 4267 4456 4457 4459 4569 4701 4702 4706)
#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis.c>
#pragma warning(pop)

#include <cstdio>
#include <cstdlib>

#include "ogg_test_data.h"

static int g_checks = 0;
static int g_failures = 0;

// stb_vorbis.c 内部のデバッグ用 CHECK マクロと衝突するので上書きする
#ifdef CHECK
#undef CHECK
#endif
#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

int main()
{
    int channels = 0, sampleRate = 0;
    short* samples = nullptr;
    const int totalFrames = stb_vorbis_decode_memory(
        kTestToneOgg, static_cast<int>(sizeof(kTestToneOgg)),
        &channels, &sampleRate, &samples);

    CHECK(totalFrames > 0);
    CHECK(samples != nullptr);
    CHECK(channels == 1);
    CHECK(sampleRate == 22050);

    if (totalFrames > 0 && samples)
    {
        // 0.25 秒 @ 22050Hz = 5512.5 → エンコーダのフレーム境界誤差を ±1024 まで許容
        const int expected = 22050 / 4;
        CHECK(totalFrames > expected - 1024 && totalFrames < expected + 1024);

        // 440Hz サイン波なので無音ではない（ffmpeg sine 既定音量 + vorbis q0 で実測ピーク ≈ 4200）
        int peak = 0;
        for (int i = 0; i < totalFrames * channels; ++i)
        {
            const int v = (samples[i] >= 0) ? samples[i] : -samples[i];
            if (v > peak) peak = v;
        }
        CHECK(peak > 2000);

        std::printf("decoded: %d frames, %dch, %dHz, peak=%d (%.3fs)\n",
                    totalFrames, channels, sampleRate, peak,
                    static_cast<double>(totalFrames) / sampleRate);
        free(samples);   // stb_vorbis_decode_* は malloc 確保
    }

    std::printf("OggDecodeTests: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
