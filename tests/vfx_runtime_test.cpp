// UnoFX の実行時窓口の試験（FU-E-19）: GPU 初期化の失敗が黙殺されず、統計（vfx_stats の元）へ出る。GPU 不要。
#include "renderer/vfx/VfxRuntime.h"

#include <cstdio>

using namespace dx12e::vfx;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main()
{
    Runtime& rt = Runtime::Get();
    CHECK(!rt.GetStats().gpuFailed);
    CHECK(rt.DescribeGpuFailure().empty());

    Stats ok; ok.gpuReady = true;
    rt.PublishStats(std::move(ok));
    CHECK(rt.GetStats().gpuReady && !rt.GetStats().gpuFailed);

    rt.ReportGpuFailure("UnoFX: テスト用の失敗");
    CHECK(rt.GetStats().gpuFailed);
    CHECK(!rt.GetStats().gpuReady);
    CHECK(rt.GetStats().lastError == "UnoFX: テスト用の失敗");
    CHECK(rt.DescribeGpuFailure() == "UnoFX: テスト用の失敗");

    rt.ReportGpuFailure("");
    CHECK(rt.GetStats().gpuFailed);
    CHECK(!rt.DescribeGpuFailure().empty());   // 理由が空でも説明は空にならない

    if (g_failures == 0) std::printf("VfxRuntimeTests: OK\n");
    return g_failures == 0 ? 0 : 1;
}
