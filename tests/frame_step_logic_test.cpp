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

    // ---- 凍結の状態機械（PlanFreeze）----
    {
        FreezeModel m; FreezeView v;
        // 何も張っていない: 何もしない
        { auto p = PlanFreeze(m, v, false, false); CHECK(!p.setCapture && !p.setPaused && !p.releaseFreeze && !p.invalidateHistory); }
        // 凍結 + pinRender: 固定を張り、履歴を捨てる
        m.frozen = true; m.pinRender = true;
        { auto p = PlanFreeze(m, v, true, false);
          CHECK(p.setCapture && p.captureValue && p.invalidateHistory); }
        // 張り済みの再宣言（frame_step）: 張り直しで履歴だけ捨てる
        m.ownsCapture = true; v.captureOn = true;
        { auto p = PlanFreeze(m, v, true, false); CHECK(!p.setCapture && p.invalidateHistory); }
        // 毎フレームの調整では履歴を捨てない
        { auto p = PlanFreeze(m, v, false, false); CHECK(!p.setCapture && !p.invalidateHistory); }
        // 撮影の中断が固定を落とした: 次の調整が戻す（履歴は捨てない）
        v.captureOn = false;
        { auto p = PlanFreeze(m, v, false, false); CHECK(p.setCapture && p.captureValue && !p.invalidateHistory); }
        // 撮影の最中は触らない
        v.shotInFlight = true;
        { auto p = PlanFreeze(m, v, false, false); CHECK(!p.setCapture); }
        v.shotInFlight = false; v.captureOn = true;
        // 【ブロッカーの再現】pinRender:false へ切り替えたら固定を外す（外し忘れて居座らない）
        m.pinRender = false;
        { auto p = PlanFreeze(m, v, true, false); CHECK(p.setCapture && !p.captureValue && !p.invalidateHistory); }
        m.ownsCapture = false; v.captureOn = false;
        // 解除: 固定を外し、paused を戻す
        m.pinRender = true; m.ownsCapture = true; v.captureOn = true; v.paused = true; v.playing = true;
        m.frozen = false;
        { auto p = PlanFreeze(m, v, false, true);
          CHECK(p.setCapture && !p.captureValue && p.setPaused && !p.pausedValue); }
        // 解除後は pinRender が true のままでも WantPin は偽（FinishFinalScreenshot の戻り先）
        CHECK(!m.WantPin());
        // Editor モードで frame_step が残した paused も解除で戻る
        { FreezeModel e; FreezeView ev; ev.playing = false; ev.paused = true;
          auto p = PlanFreeze(e, ev, false, true); CHECK(p.setPaused && !p.pausedValue); }
    }
    {
        // Play 中: paused が戻されても止め直す。frame_step の最中は触らない
        FreezeModel m; m.frozen = true; m.pinRender = false;
        FreezeView v; v.playing = true; v.paused = false;
        { auto p = PlanFreeze(m, v, false, false); CHECK(p.setPaused && p.pausedValue); }
        v.stepping = true;
        { auto p = PlanFreeze(m, v, false, false); CHECK(!p.setPaused); }
        // Editor では paused に触らない
        v.stepping = false; v.playing = false;
        { auto p = PlanFreeze(m, v, false, false); CHECK(!p.setPaused); }
        // 他人が立てた決定論キャプチャ（ownsCapture でない）は、pin なしの凍結でも落とさない
        v.captureOn = true;
        { auto p = PlanFreeze(m, v, false, false); CHECK(!p.setCapture); }
    }
    {
        // リース: MCP が切れていたら凍結を解く（固定・paused も戻す）
        FreezeModel m; m.frozen = true; m.pinRender = true; m.ownsCapture = true;
        FreezeView v; v.captureOn = true; v.playing = true; v.paused = true;
        v.disconnectedSec = kFreezeLeaseSec - 0.1;
        { auto p = PlanFreeze(m, v, false, false); CHECK(!p.releaseFreeze && !p.setCapture); }
        v.disconnectedSec = kFreezeLeaseSec;
        { auto p = PlanFreeze(m, v, false, false);
          CHECK(p.releaseFreeze && p.setCapture && !p.captureValue && p.setPaused && !p.pausedValue); }
    }

    // FU-L-2: 決定論キャプチャの自動露出は実時間に依存しない（即適応）。通常は実 dt のまま
    CHECK(AutoExposureDt(true, 0.016f) == AutoExposureDt(true, 0.5f));
    CHECK(AutoExposureDt(true, 0.016f) > 1.0e8f);
    CHECK(AutoExposureDt(false, 0.016f) == 0.016f);

    std::printf("FrameStepLogicTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
