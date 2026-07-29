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
    f32       m_totalTime = 0.0f;
    u64       m_frameCount = 0;

    // FPS計測（0.3秒間隔）
    f32       m_fpsAccum = 0.0f;
    u32       m_fpsFrames = 0;
    f32       m_displayFps = 0.0f;
};

} // namespace dx12e
