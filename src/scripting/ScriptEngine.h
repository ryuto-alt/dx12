#pragma once

#include <string>
#include <memory>
#include "core/Types.h"
#include <entt/entt.hpp>

// sol2 forward declaration
struct lua_State;
namespace sol { class state; }

namespace dx12e
{

class Scene;
class InputSystem;
class Camera;
class AudioSystem;
class PhysicsSystem;

class ScriptEngine
{
public:
    ScriptEngine();
    ~ScriptEngine();

    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    void Initialize(Scene* scene, InputSystem* input, Camera* camera,
                    AudioSystem* audio, PhysicsSystem* physics,
                    const std::string& assetsDir);

    void LoadScript(const std::string& filePath);

    // ゲームライフサイクル
    void CallOnStart();
    void CallOnUpdate(f32 dt);

    // エンティティアタッチ版 API
    void AttachScriptToEntity(entt::entity e, const std::string& scriptPath);
    void DetachScriptFromEntity(entt::entity e);
    void OnPlayStart();                   // 全 LuaScript 初期化 + OnStart
    void OnPlayStop();                    // 全 env 破棄、started リセット
    void UpdateAttachedScripts(f32 dt);   // 毎フレーム OnUpdate
    void ReloadScript(entt::entity e);    // Inspector Reload ボタン用

    void Shutdown();

    const std::string& GetLastError()       const { return m_lastError; }
    void               ClearError()                { m_lastError.clear(); }
    // Toolbar が表示する直近の Lua log() 出力 (動作確認用)
    const std::string& GetLastLuaMessage()  const { return m_lastLuaMessage; }

private:
    void RegisterBindings();
    void RegisterPhysicsBindings();

    std::unique_ptr<sol::state> m_lua;
    Scene*         m_scene   = nullptr;
    InputSystem*   m_input   = nullptr;
    Camera*        m_camera  = nullptr;
    AudioSystem*   m_audio   = nullptr;
    PhysicsSystem* m_physics = nullptr;
    std::string  m_assetsDir;
    std::string  m_lastError;
    std::string  m_lastLuaMessage;
};

} // namespace dx12e
