// 起動音 Calibrate のイベント表（src/core/SplashCalibrateEvents.h。tools/gen_boot_sound_calibrate.py の生成物）の検査（W1-BD1）。
// ヘッダと events.json の一致は tools/gen_boot_sound_calibrate.py --check-assets（ctest の CalibrateSoundAssets）。
// ここは C++ から見た形: 件数、時刻の昇順、§2.6 のキュー表（EXEC_W05_W1）との一致、サンプル位置と ms の整合、
// 指針の角度（15 度 x (i + 1)）、ロックの順序。GPU 不要。
#include "core/SplashCalibrateEvents.h"

#include <cmath>
#include <cstdio>

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace cal = dx12e::splash::calibrate;

// §2.6 の表（検証専用。生成には使わない）
constexpr double kRefClicks[24] = {268, 359, 449, 536, 620, 698, 770, 834, 892, 942, 985, 1022, 1054, 1083, 1110, 1136,
                                   1166, 1200, 1241, 1289, 1346, 1408, 1475, 1543};
constexpr double kTol = 0.5;

const cal::Event* Find(cal::EventKind kind, int index)
{
    for (const auto& e : cal::kEvents)
        if (e.kind == kind && e.index == index) return &e;
    return nullptr;
}
}

int main()
{
    static_assert(cal::kClickCount == 24);
    static_assert(cal::kSampleRate == 44100);
    static_assert(cal::kEventCount >= 24 + 2 + 1 + 2 + 5 + 1 + 1 + 1);

    // クリック
    for (int i = 0; i < cal::kClickCount; ++i)
    {
        CHECK(std::fabs(cal::kClickMs[i] - kRefClicks[i]) <= kTol);
        CHECK(std::fabs(cal::kClickAngleDeg[i] - 15.0 * (i + 1)) < 1e-9);
        if (i > 0) CHECK(cal::kClickMs[i] > cal::kClickMs[i - 1]);
        const cal::Event* e = Find(cal::EventKind::Click, i);
        CHECK(e != nullptr);
        if (e)
        {
            CHECK(e->tMs == cal::kClickMs[i]);
            // パン = 0.6 sin(2 pi (i + 1) / 24)（4 桁に丸めた値）
            const double pan = 0.6 * std::sin(2.0 * 3.14159265358979323846 * (i + 1) / 24.0);
            CHECK(std::fabs(e->pan - pan) < 1e-4);
        }
    }
    CHECK(cal::kClickAngleDeg[23] == 360.0);

    // 時刻の昇順、サンプル位置と ms の整合（0.05ms 以下）
    for (std::size_t i = 0; i < cal::kEventCount; ++i)
    {
        const auto& e = cal::kEvents[i];
        if (i > 0) CHECK(e.tMs >= cal::kEvents[i - 1].tMs);
        CHECK(std::fabs(e.sample * 1000.0 / cal::kSampleRate - e.tMs) < 0.05);
    }

    // ロックと、その後ろ
    const cal::Event* lock = Find(cal::EventKind::Latch, 0);
    const cal::Event* latch2 = Find(cal::EventKind::Latch, 1);
    const cal::Event* tok = Find(cal::EventKind::Tok, 0);
    const cal::Event* ping = Find(cal::EventKind::Ping, 0);
    const cal::Event* end = Find(cal::EventKind::End, 0);
    CHECK(lock && latch2 && tok && ping && end);
    if (lock && latch2 && tok && ping && end)
    {
        CHECK(std::fabs(lock->tMs - 1628.0) <= kTol);
        CHECK(lock->tMs == cal::kLockMs && lock->sample == cal::kLockSample);
        CHECK(std::fabs(tok->tMs - 1630.0) <= kTol);
        CHECK(std::fabs(latch2->tMs - 1646.0) <= kTol);
        CHECK(std::fabs(ping->tMs - 2148.0) <= kTol);
        CHECK(end->tMs == cal::kLengthMs && end->sample == cal::kLengthSamples);
        CHECK(cal::kClickMs[23] < cal::kLockMs);
    }
    const double bells[5] = {1648, 1673, 1698, 1723, 1748};
    for (int k = 0; k < 5; ++k)
    {
        const cal::Event* b = Find(cal::EventKind::Bell, k);
        CHECK(b != nullptr && std::fabs(b->tMs - bells[k]) <= kTol);
    }

    // ベッドのループ: ロックより前で、55Hz の整数周期（400ms = 22 周期）
    CHECK(cal::kBedLoopEndMs < cal::kLockMs);
    const double cycles = (cal::kBedLoopEndMs - cal::kBedLoopStartMs) * 1e-3 * 55.0;
    CHECK(std::fabs(cycles - std::round(cycles)) < 1e-9);

    if (g_fail == 0) std::printf("splash_calibrate_events_test: ok\n");
    return g_fail == 0 ? 0 : 1;
}
