// freeze_time / frame_step（FU-E-24）の引数検証。ヘッダのみ（GPU 不要）。
#include "core/FrameStepLogic.h"

#include <cmath>
#include <cstdio>

using namespace dx12e;

namespace { int g_failures = 0, g_checks = 0; }
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

int main()
{
    // 既定: 1 フレーム・dt=1/60
    { auto r = ParseFrameStepRequest(1, false, 0.0);
      CHECK(r.ok && r.frames == 1 && std::fabs(r.dt - 1.0f / 60.0f) < 1e-9f); }
    // frames は丸める
    CHECK(ParseFrameStepRequest(0, false, 0.0).frames == 1);
    CHECK(ParseFrameStepRequest(-5, false, 0.0).frames == 1);
    CHECK(ParseFrameStepRequest(8, false, 0.0).frames == 8);
    CHECK(ParseFrameStepRequest(100000, false, 0.0).frames == kFrameStepMaxFrames);
    // dt の範囲
    CHECK(ParseFrameStepRequest(1, true, 0.01).ok);
    CHECK(std::fabs(ParseFrameStepRequest(1, true, 0.01).dt - 0.01f) < 1e-9f);
    CHECK(ParseFrameStepRequest(1, true, static_cast<double>(GameClock::kMaxDeltaTime)).ok);
    CHECK(!ParseFrameStepRequest(1, true, 0.0).ok);
    CHECK(!ParseFrameStepRequest(1, true, -0.01).ok);
    CHECK(!ParseFrameStepRequest(1, true, 0.5).ok);
    CHECK(!ParseFrameStepRequest(1, true, std::nan("")).ok);
    CHECK(!ParseFrameStepRequest(1, true, 0.0).error.empty());
    // 進む時間
    { auto r = ParseFrameStepRequest(8, true, 0.02);
      CHECK(std::fabs(FrameStepSimulatedSec(r) - 0.16f) < 1e-6f); }

    std::printf("FrameStepLogicTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
