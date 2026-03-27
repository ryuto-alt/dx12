#pragma once
#include "Types.h"
#include <chrono>

namespace dx12e
{

class GameClock
{
public:
    GameClock() = default;

    void Reset();
    void Tick();

    f32 GetDeltaTime() const { return m_deltaTime; }
    f32 GetTotalTime() const { return m_totalTime; }
    f32 GetFPS() const;
    u64 GetFrameCount() const { return m_frameCount; }

private:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    TimePoint m_startTime{};
    TimePoint m_prevTime{};
    f32       m_deltaTime = 0.0f;
    f32       m_totalTime = 0.0f;
    u64       m_frameCount = 0;
};

} // namespace dx12e
