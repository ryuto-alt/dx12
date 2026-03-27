#include "GameClock.h"

namespace dx12e
{

void GameClock::Reset()
{
    m_startTime = Clock::now();
    m_prevTime = m_startTime;
    m_deltaTime = 0.0f;
    m_totalTime = 0.0f;
    m_frameCount = 0;
}

void GameClock::Tick()
{
    auto now = Clock::now();

    std::chrono::duration<f32> delta = now - m_prevTime;
    std::chrono::duration<f32> total = now - m_startTime;

    m_deltaTime = delta.count();
    m_totalTime = total.count();
    m_prevTime = now;
    m_frameCount++;
}

f32 GameClock::GetFPS() const
{
    if (m_totalTime > 0.0f)
    {
        return static_cast<f32>(m_frameCount) / m_totalTime;
    }
    return 0.0f;
}

} // namespace dx12e
