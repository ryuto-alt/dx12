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

private:
    // 未保存確認モーダルの OpenPopup を毎フレーム呼ばないためのラッチ
    // （AssetBrowserPanel の削除確認と同じ作法）。
    bool m_unsavedPopupOpen   = false;
    bool m_autosavePopupOpen  = false;
};

} // namespace dx12e
