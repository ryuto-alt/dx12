#pragma once
// ===========================================================================
// 時間を止めて 1 フレームずつ進める口（MCP freeze_time / frame_step。FU-E-24）の純粋な部分。
// Application に触れない検証だけを置く（GPU 不要でテストできる）。
// ===========================================================================
#include "core/GameClock.h"
#include <string>

namespace dx12e
{
struct FrameStepRequest
{
    int         frames = 1;                 // 1..kMaxFrames
    f32         dt     = 1.0f / 60.0f;      // 秒。0 < dt <= GameClock::kMaxDeltaTime
    bool        ok     = true;
    std::string error;                      // ok=false のときの理由（日本語）
};

constexpr int kFrameStepMaxFrames = 600;    // step_frames と同じ上限（クライアントのタイムアウト対策）

// frames は範囲外なら黙って丸める（step_frames と同じ流儀）。dt は範囲外ならエラー。
// hasDt=false なら dt は既定（1/60）。
inline FrameStepRequest ParseFrameStepRequest(int frames, bool hasDt, double dt)
{
    FrameStepRequest r;
    r.frames = frames < 1 ? 1 : (frames > kFrameStepMaxFrames ? kFrameStepMaxFrames : frames);
    if (hasDt)
    {
        if (!(dt > 0.0) || dt > static_cast<double>(GameClock::kMaxDeltaTime))
        {
            r.ok    = false;
            r.error = "dt は 0 より大きく " + std::to_string(GameClock::kMaxDeltaTime) + " 以下（既定 1/60）";
            return r;
        }
        r.dt = static_cast<f32>(dt);
    }
    return r;
}

inline f32 FrameStepSimulatedSec(const FrameStepRequest& r) { return r.dt * static_cast<f32>(r.frames); }
} // namespace dx12e
