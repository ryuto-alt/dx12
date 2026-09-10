#pragma once
#include "Types.h"
#include <chrono>

namespace dx12e
{

class GameClock
{
public:
    GameClock() = default;

    // dt の上限。これを超えたフレーム（ロード・ブレークポイント・アプリの復帰など）は
    // ここで頭打ちにする。クランプが無いと、数百ms〜数秒の dt が Lua の OnUpdate・
    // Animator・パーティクル・トレイル・UI アニメへそのまま流れ、1 フレームで
    // ワープ / アニメ飛び / 寿命の一括消滅が起きる。
    // （物理は PhysicsSystem 側で独自にクランプ済みだが、他は全部無防備だった）
    // 0.1s = 10fps 相当。正当に重いだけのフレームを不必要にスローモーにしない範囲で、
    // 「事故のような巨大 dt」だけを切る値。
    static constexpr f32 kMaxDeltaTime = 0.1f;

    void Reset();
    void Tick();

    // ── 固定 dt（決定論ステップ）──
    // MCP の dx12_step_frames(deterministic:true) が「N フレームぶん**必ず** N*dt 秒進める」
    // ために使う。実時間 dt のままだと、同じ入力を同じフレーム数だけ与えても
    // 進む距離が毎回変わる＝テストプレイの結果が再現しない（ジャンプが届いたり届かなかったりする）。
    //
    // ★物理はもともと 60Hz 固定ステップ + accumulator なので、dt=1/60 を与えると
    //   端数が出ず「1 フレーム = 1 物理ステップ」に揃う。
    // ★実時間から切り離すのは**シミュレーションの dt だけ**。GetRawDeltaTime()（FPS 表示・
    //   ヒッチ計測）と GetTotalTime() は実時間のまま＝計測系に嘘をつかない。
    void SetFixedDelta(f32 dt) { m_fixedDelta = (dt > 0.0f) ? dt : 0.0f; }
    void ClearFixedDelta()     { m_fixedDelta = 0.0f; }
    bool HasFixedDelta() const { return m_fixedDelta > 0.0f; }
    f32  FixedDelta()    const { return m_fixedDelta; }

    // ゲームロジック用の dt（クランプ済み）。
    f32 GetDeltaTime() const { return m_deltaTime; }
    // 実時間の dt（クランプなし）。FPS 表示やヒッチ計測など「本当の経過」が要る側だけ使う。
    f32 GetRawDeltaTime() const { return m_rawDeltaTime; }
    // 起動からの実時間（クランプの影響を受けない）。記録の時間軸はこちらを使う。
    f32 GetTotalTime() const { return m_totalTime; }
    f32 GetFPS() const { return m_displayFps; }
    u64 GetFrameCount() const { return m_frameCount; }

private:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    TimePoint m_startTime{};
    TimePoint m_prevTime{};
    f32       m_rawDeltaTime = 0.0f;   // クランプ前（FPS 表示用）
    f32       m_deltaTime = 0.0f;      // クランプ後（ゲームロジック用）
    f32       m_fixedDelta = 0.0f;     // >0 なら m_deltaTime をこの値に固定する（決定論ステップ）
    f32       m_totalTime = 0.0f;
    u64       m_frameCount = 0;

    // FPS計測（0.3秒間隔）
    f32       m_fpsAccum = 0.0f;
    u32       m_fpsFrames = 0;
    f32       m_displayFps = 0.0f;
};

} // namespace dx12e
