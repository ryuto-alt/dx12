// FU-E-22: perf_stats の gpuFrameMs（フレームごとの GPU Total の p50/p95/p99/max）の純ロジックと、
// ApplicationMcpEditor.cpp の配線（gpuFrameMs / throttleTier / alwaysRealtime / frameMs.p99）の検査。
#include "core/mcp/McpPerfGpuStats.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace dx12e::mcpperf;

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
bool Near(double a, double b) { return std::fabs(a - b) < 1e-4; }
}

int main()
{
    {   // 空 / 全部未計測
        CHECK(ComputeGpuFrameStats({}).samples == 0);
        const auto s = ComputeGpuFrameStats({0.0f, 0.0f, -1.0f});
        CHECK(s.samples == 0 && s.p95 == 0.0 && s.max == 0.0);
    }
    {   // 1 サンプル
        const auto s = ComputeGpuFrameStats({7.5f});
        CHECK(s.samples == 1 && Near(s.p50, 7.5) && Near(s.p95, 7.5) && Near(s.p99, 7.5) && Near(s.max, 7.5));
    }
    {   // 1..100 ms（順不同で渡す）。最近傍: idx = round(p*99)
        std::vector<float> v;
        for (int i = 100; i >= 1; --i) v.push_back(static_cast<float>(i));
        const auto s = ComputeGpuFrameStats(v);
        CHECK(s.samples == 100);
        CHECK(Near(s.p50, 51.0));    // idx 50 (49.5+0.5)
        CHECK(Near(s.p95, 95.0));    // idx 94 (94.05+0.5 -> 94) -> 95
        CHECK(Near(s.p99, 99.0));    // idx 98
        CHECK(Near(s.max, 100.0));
    }
    {   // 尾: 平均は 10 ms 前後でも p95/p99 がスパイクを拾う
        std::vector<float> v(95, 8.0f);
        for (int i = 0; i < 5; ++i) v.push_back(30.0f);
        const auto s = ComputeGpuFrameStats(v);
        CHECK(Near(s.p50, 8.0));
        CHECK(Near(s.p99, 30.0));
        CHECK(Near(s.max, 30.0));
    }
    {   // 未計測の 0 と NaN は捨てる
        const auto s = ComputeGpuFrameStats({0.0f, 5.0f, std::nanf(""), 6.0f});
        CHECK(s.samples == 2 && Near(s.max, 6.0));
    }
    {   // 配線の回帰
        std::ifstream f(std::string(UI_LINT_ROOT) + "/src/core/mcp/ApplicationMcpEditor.cpp", std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        const std::string src = ss.str();
        CHECK(!src.empty());
        for (const char* k : {"\"gpuFrameMs\"", "\"throttleTier\"", "\"alwaysRealtime\"", "ComputeGpuFrameStats", "[\"frameMs\"][\"p99\"]"})
            if (src.find(k) == std::string::npos) { std::printf("FAIL wiring missing %s\n", k); ++g_fail; }
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("PerfStatsGpuTests OK\n");
    return 0;
}
