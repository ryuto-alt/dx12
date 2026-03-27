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

    // FPS計測（0.3秒間隔で更新）
    m_fpsAccum += m_deltaTime;
    m_fpsFrames++;
    if (m_fpsAccum >= 0.3f)
    {
        m_displayFps = static_cast<f32>(m_fpsFrames) / m_fpsAccum;
        m_fpsAccum = 0.0f;
        m_fpsFrames = 0;
    }
}

} // namespace dx12e
