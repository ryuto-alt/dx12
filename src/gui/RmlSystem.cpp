#include "gui/RmlSystem.h"
#include "core/GameClock.h"
#include "core/Logger.h"

namespace dx12e
{

RmlSystem::RmlSystem(const GameClock& clock)
    : m_clock(clock)
{
}

double RmlSystem::GetElapsedTime()
{
    return static_cast<double>(m_clock.GetTotalTime());
}

bool RmlSystem::LogMessage(Rml::Log::Type type, const Rml::String& message)
{
    switch (type)
    {
    case Rml::Log::LT_ERROR:
    case Rml::Log::LT_ASSERT:
        Logger::Error("[RmlUi] {}", message);
        break;
    case Rml::Log::LT_WARNING:
        Logger::Warn("[RmlUi] {}", message);
        break;
    default:
        Logger::Info("[RmlUi] {}", message);
        break;
    }
    return true;
}

} // namespace dx12e
