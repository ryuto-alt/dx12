#pragma once

#include <string>
#include "core/Types.h"

namespace dx12e
{

class EditorContext;
class ScriptEngine;
class GameClock;
class Scene;
class Window;
class AudioSystem;

class ToolbarPanel
{
public:
    // mode: 0=Editor, 1=Playing (matches EngineMode enum values)
    void Render(bool isPlaying,
                EditorContext& ctx,
                bool& outModeChangeRequested,
                bool& outPendingPlayMode,
                ScriptEngine* scriptEngine,
                GameClock* clock,
                Scene* scene,
                Window* window,
                AudioSystem* audioSystem,
                const std::string& assetsDir,
                f32 toolbarHeight);
};

} // namespace dx12e
