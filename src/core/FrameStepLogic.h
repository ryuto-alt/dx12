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

// ---------------------------------------------------------------------------
// 凍結（freeze_time）の状態機械。状態の持ち主は 1 か所（FreezeModel）で、毎フレームと各ハンドラが
// 「今の観測（FreezeView）→ すべき操作（FreezePlan）」を PlanFreeze から得て適用する。
// 撮影の中断（bail / AbortOffscreenShot / perceive の後始末）が m_deterministicCapture を落としても、
// 次のフレームの調整が約束（frozen && pinRender）へ戻す。
// ---------------------------------------------------------------------------
constexpr double kFreezeLeaseSec = 5.0;     // MCP クライアントが 1 本も繋がっていない時間がこれを超えたら凍結を解く

struct FreezeModel
{
    bool frozen      = false;   // 時間を止めている（Play 中のシミュ + 描画の固定）
    bool pinRender   = true;    // 描画の時間依存（決定論キャプチャと同じ固定）も止めているか
    bool ownsCapture = false;   // 決定論キャプチャを立てたのは私たち（落とすのも私たち）
    bool WantPin() const { return frozen && pinRender; }
};

struct FreezeView
{
    bool   captureOn      = false;   // m_deterministicCapture の今の値
    bool   shotInFlight   = false;   // 撮影（screenshot_final / perceive / オフスクリーン / 収束待ち）が進行中
    bool   playing        = false;   // Play 中
    bool   paused         = false;   // EditorContext::paused
    bool   stepping       = false;   // frame_step / step_frames の最中（paused を触らない）
    double disconnectedSec = 0.0;    // MCP クライアントが 0 本の状態が続いた秒数
};

struct FreezePlan
{
    bool releaseFreeze     = false;   // リース切れで凍結を解く（model.frozen=false にする）
    bool setCapture        = false;
    bool captureValue      = false;
    bool invalidateHistory = false;   // 固定を張り直すので TAA 履歴などを捨てる
    bool setPaused         = false;
    bool pausedValue       = false;
};

// restartHistory: 凍結を（張り直して）宣言する呼び出し（freeze_time{frozen:true} / frame_step）だけ true。
// releasedNow: 解除の呼び出し（freeze_time{frozen:false}）だけ true。paused を戻す。
inline FreezePlan PlanFreeze(const FreezeModel& m, const FreezeView& v, bool restartHistory, bool releasedNow)
{
    FreezePlan p;
    bool frozen = m.frozen;
    if (frozen && v.disconnectedSec >= kFreezeLeaseSec)
    {
        p.releaseFreeze = true;
        frozen          = false;
        releasedNow     = true;
    }
    const bool wantPin = frozen && m.pinRender;
    if (!v.shotInFlight)
    {
        if (wantPin && (!v.captureOn || restartHistory))
        {
            p.setCapture        = !v.captureOn;
            p.captureValue      = true;
            p.invalidateHistory = restartHistory;   // 張り直すたびに同じ初期状態から。毎フレームの調整（中断の後始末）では捨てない
        }
        else if (!wantPin && v.captureOn && m.ownsCapture)
        {
            p.setCapture   = true;
            p.captureValue = false;
        }
    }
    if (frozen && v.playing && !v.stepping && !v.paused)
    {
        p.setPaused   = true;     // Play/Stop・シーン切替で paused が戻されても止め直す
        p.pausedValue = true;
    }
    else if (releasedNow && !v.stepping && v.paused)
    {
        p.setPaused   = true;
        p.pausedValue = false;
    }
    return p;
}
} // namespace dx12e
