// GpuScope / UNO_GPU_SCOPE の検査（W05-27）。GPU もコマンドリストも使わない（cmd=null でも対の計数は動く）。
#include "graphics/GpuMarker.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace dx12e;

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int Early(bool early)
{
    UNO_GPU_SCOPE(nullptr, nullptr, GpuTimer::Shadows);
    if (early) return 1;
    return 0;
}

int main()
{
    GpuMarker::ResetStats();

    // 入れ子
    {
        GpuScope a(nullptr, nullptr, GpuTimer::Total);
        CHECK(GpuMarker::Stats().depth == 1);
        {
            UNO_GPU_SCOPE(nullptr, nullptr, GpuTimer::PostFX);
            GpuEventScope b(nullptr, "Named", 5);
            CHECK(GpuMarker::Stats().depth == 3);
        }
        CHECK(GpuMarker::Stats().depth == 1);
    }
    CHECK(GpuMarker::Stats().depth == 0);
    CHECK(GpuMarker::Stats().begins == 3 && GpuMarker::Stats().ends == 3);
    CHECK(GpuMarker::Stats().maxDepth == 3);

    // 早期 return
    // 文字列リテラル版（長さ N-1 を自動で渡す）。
    GpuMarker::ResetStats();
    {
        GpuScope lit(nullptr, nullptr, GpuTimer::Total, "TestPass");
        GpuEventScope ev(nullptr, "Ev");
        CHECK(GpuMarker::Stats().depth == 2);
    }
    CHECK(GpuMarker::Stats().begins == 2 && GpuMarker::Stats().ends == 2 && GpuMarker::Stats().depth == 0);
    GpuMarker::ResetStats();
    CHECK(Early(true) == 1);
    CHECK(Early(false) == 0);
    CHECK(GpuMarker::Stats().begins == 2 && GpuMarker::Stats().ends == 2 && GpuMarker::Stats().depth == 0);

    // 例外時も対になる
    GpuMarker::ResetStats();
    try
    {
        GpuScope a(nullptr, nullptr, GpuTimer::Total);
        UNO_GPU_SCOPE(nullptr, nullptr, GpuTimer::UI);
        throw std::runtime_error("x");
    }
    catch (const std::exception&) {}
    CHECK(GpuMarker::Stats().begins == 2 && GpuMarker::Stats().ends == 2 && GpuMarker::Stats().depth == 0);

    // 手動の対
    GpuMarker::ResetStats();
    GpuMarker::Begin(nullptr, nullptr, GpuTimer::Bloom);
    CHECK(GpuMarker::Stats().depth == 1);
    GpuMarker::End(nullptr, nullptr, GpuTimer::Bloom);
    CHECK(GpuMarker::Stats().depth == 0);

    // 既定名の長さのキャッシュが strlen と一致
    for (u32 i = 0; i < GpuTimer::Count; ++i)
        CHECK(GpuMarker::DefaultNameLen(static_cast<GpuTimer::Scope>(i)) == std::strlen(GpuTimer::Info(static_cast<GpuTimer::Scope>(i)).name));

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
