// UnoFX の放出の算術と、カール場の CPU 参照（W1-BD4）。GPU 不要。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

#include <DirectXPackedVector.h>

#include "renderer/vfx/VfxCurl.h"
#include "renderer/vfx/VfxSpawnMath.h"

using namespace dx12e;
using namespace dx12e::vfx;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)
} // namespace

int main()
{
    // ---- 持ち越し式: 刻みが何でも、合計は rate * T に 1 個以内で一致する ----
    for (double dt : { 1.0 / 60, 1.0 / 144, 1.0 / 30, 0.0137, 0.1 })
    {
        SpawnDesc s; s.rate = 100.0f;
        SpawnClock clk;
        const int steps = static_cast<int>(std::round(5.0 / dt));
        for (int i = 0; i < steps; ++i) PlanSpawnStep(s, true, 0.0f, dt, 1.0f, 100000, clk);
        const double expect = 100.0 * clk.time;
        CHECK(std::fabs(double(clk.spawned) - expect) <= 1.0);
    }
    // 倍率
    {
        SpawnDesc s; s.rate = 100.0f; SpawnClock clk;
        for (int i = 0; i < 60; ++i) PlanSpawnStep(s, true, 0.0f, 1.0 / 60, 0.5f, 100000, clk);
        CHECK(std::llabs(static_cast<long long>(clk.spawned) - 50) <= 1);
    }
    // 1 ステップの上限（持ち越し）: 失わずに後のステップで出る
    {
        SpawnDesc s; s.rate = 0.0f; s.bursts.push_back({0.0f, 40000});
        SpawnClock clk; u32 total = 0, maxStep = 0;
        for (int i = 0; i < 10; ++i)
        {
            const SpawnStep st = PlanSpawnStep(s, false, 1.0f, 1.0 / 60, 1.0f, 100000, clk);
            total += st.count; maxStep = std::max(maxStep, st.count);
        }
        CHECK(maxStep <= kMaxEmitPerStep);
        CHECK(total == 40000);            // 取りこぼしなし
        CHECK(clk.spawned == 40000);
    }
    // リングの大きさ（cap）が 1 ステップの上限になる
    {
        SpawnDesc s; s.rate = 0.0f; s.bursts.push_back({0.0f, 1000});
        SpawnClock clk;
        const SpawnStep st = PlanSpawnStep(s, false, 1.0f, 1.0 / 60, 1.0f, 64, clk);
        CHECK(st.count == 64);
    }
    // バースト: ちょうど t を含むステップで 1 回だけ。loop は周回ごとに 1 回
    {
        SpawnDesc s; s.rate = 0.0f; s.bursts.push_back({0.5f, 7});
        SpawnClock clk; u32 hits = 0;
        for (int i = 0; i < 60 * 5; ++i) hits += PlanSpawnStep(s, true, 1.0f, 1.0 / 60, 1.0f, 1000, clk).count;
        CHECK(hits == 7 * 5);
        SpawnClock c2; u32 h2 = 0;
        for (int i = 0; i < 60 * 5; ++i) h2 += PlanSpawnStep(s, false, 1.0f, 1.0 / 60, 1.0f, 1000, c2).count;
        CHECK(h2 == 7);
    }
    // 非ループ: duration で連続放出が止まる。startDelay まで出ない
    {
        SpawnDesc s; s.rate = 60.0f; s.startDelay = 0.5f;
        SpawnClock clk;
        for (int i = 0; i < 60 * 5; ++i) PlanSpawnStep(s, false, 2.0f, 1.0 / 60, 1.0f, 100000, clk);
        CHECK(std::llabs(static_cast<long long>(clk.spawned) - 90) <= 1);   // 0.5〜2.0 秒の 1.5 秒ぶん
    }
    // サブフレーム補間
    {
        const u32 n = 4;
        float prev = 0.0f;
        for (u32 k = 0; k < n; ++k)
        {
            const float f = SubframeFraction(k, n);
            CHECK(f > prev && f < 1.0f);
            prev = f;
            CHECK(std::fabs(SubframeAdvance(k, n, 0.016f) - 0.016f * (1.0f - f)) < 1e-6f);
        }
        CHECK(std::fabs(SubframeFraction(0, 4) - 0.125f) < 1e-7f && std::fabs(SubframeFraction(3, 4) - 0.875f) < 1e-7f);
    }
    // 決定論の再現: 同じ入力は同じ列
    {
        SpawnDesc s; s.rate = 77.0f; s.bursts.push_back({0.3f, 11});
        SpawnClock a, b; std::vector<u32> la, lb;
        for (int i = 0; i < 300; ++i)
        {
            la.push_back(PlanSpawnStep(s, true, 1.0f, 1.0 / 60, 1.0f, 5000, a).count);
            lb.push_back(PlanSpawnStep(s, true, 1.0f, 1.0 / 60, 1.0f, 5000, b).count);
        }
        CHECK(la == lb);
    }

    // ---- 乱数 ----
    {
        CHECK(HashU(1, 2, 3) == HashU(1, 2, 3));
        CHECK(HashU(1, 2, 3) != HashU(1, 3, 3) && HashU(1, 2, 3) != HashU(2, 2, 3) && HashU(1, 2, 3) != HashU(1, 2, 4));
        double sum = 0.0; int bins[10] = {};
        const int N = 100000;
        bool inRange = true;
        for (int i = 0; i < N; ++i)
        {
            const float v = Hash01(7, static_cast<u32>(i), 1);
            if (!(v >= 0.0f && v < 1.0f)) inRange = false;
            sum += v; ++bins[static_cast<int>(v * 10)];
        }
        CHECK(inRange);
        CHECK(std::fabs(sum / N - 0.5) < 0.01);
        for (int b : bins) CHECK(std::abs(b - N / 10) < N / 10 * 0.05);
    }

    // ---- カール場 ----
    {
        const std::vector<float> f1 = GenerateCurlField(), f2 = GenerateCurlField();
        CHECK(f1.size() == size_t(kCurlRes) * kCurlRes * kCurlRes * 4);
        CHECK(f1 == f2);   // 決定論（バイト一致）
        float maxLen = 0.0f;
        for (size_t i = 0; i < f1.size(); i += 4)
            maxLen = std::max(maxLen, std::sqrt(f1[i] * f1[i] + f1[i + 1] * f1[i + 1] + f1[i + 2] * f1[i + 2]));
        CHECK(std::fabs(maxLen - 1.0f) < 1e-4f);
        CHECK(MaxDivergence(f1) < 1e-5f);    // 離散の発散（rot なので 0）

        // テクセル中心のサンプルは格納値そのもの
        const u32 i3[3] = { 5, 17, 40 };
        const size_t idx = (size_t(i3[2]) * kCurlRes * kCurlRes + size_t(i3[1]) * kCurlRes + i3[0]) * 4;
        float out[3];
        SampleCurl(f1, (i3[0] + 0.5f) / kCurlRes, (i3[1] + 0.5f) / kCurlRes, (i3[2] + 0.5f) / kCurlRes, out);
        CHECK(std::fabs(out[0] - f1[idx]) < 1e-5f && std::fabs(out[1] - f1[idx + 1]) < 1e-5f && std::fabs(out[2] - f1[idx + 2]) < 1e-5f);
        // 周期（ラップ）
        float a[3], b[3];
        SampleCurl(f1, 0.123f, 0.456f, 0.789f, a);
        SampleCurl(f1, 1.123f, -0.544f, 2.789f, b);
        CHECK(std::fabs(a[0] - b[0]) < 1e-4f && std::fabs(a[1] - b[1]) < 1e-4f && std::fabs(a[2] - b[2]) < 1e-4f);
        // GPU に上げる half の丸めは 1e-3 以下（CPU 参照との許容に収まる）
        const std::vector<u16> h = GenerateCurlFieldHalf();
        float worst = 0.0f;
        for (size_t i = 0; i < f1.size(); ++i)
            worst = std::max(worst, std::fabs(DirectX::PackedVector::XMConvertHalfToFloat(h[i]) - f1[i]));
        CHECK(worst < 1e-3f);
        // 場が「何か」を持っている（全部 0 ではない）
        std::set<int> distinct;
        for (size_t i = 0; i < f1.size(); i += 4 * 97) distinct.insert(static_cast<int>(f1[i] * 100));
        CHECK(distinct.size() > 20);
    }

    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
