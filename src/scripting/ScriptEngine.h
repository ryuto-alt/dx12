#pragma once

#include <string>
#include <memory>
#include "core/Types.h"

// sol2 forward declaration
struct lua_State;
namespace sol { class state; }

namespace dx12e
{

class Scene;
class InputSystem;
class Camera;

class ScriptEngine
{
public:
    ScriptEngine();
    ~ScriptEngine();

    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    void Initialize(Scene* scene, InputSystem* input, Camera* camera,
                    const std::string& assetsDir);

    void LoadScript(const std::string& filePath);

    // ゲームライフサイクル
    void CallOnStart();
    void CallOnUpdate(f32 dt);

    void Shutdown();

    const std::string& GetLastError() const { return m_lastError; }
    void ClearError() { m_lastError.clear(); }

private:
    void RegisterBindings();

    std::unique_ptr<sol::state> m_lua;
    Scene*       m_scene  = nullptr;
    InputSystem* m_input  = nullptr;
    Camera*      m_camera = nullptr;
    std::string  m_assetsDir;
    std::string  m_lastError;
};

} // namespace dx12e
