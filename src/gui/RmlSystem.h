#pragma once

#include <RmlUi/Core/SystemInterface.h>

namespace dx12e
{

class GameClock;

class RmlSystem : public Rml::SystemInterface
{
public:
    explicit RmlSystem(const GameClock& clock);

    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

private:
    const GameClock& m_clock;
};

} // namespace dx12e
