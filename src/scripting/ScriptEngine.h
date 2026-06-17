#pragma once

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
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
class ParticleSystem;

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

    // プロジェクト切替時に assets ベースを再設定（スクリプトの相対パス解決用）
    void SetAssetsDir(const std::string& assetsDir) { m_assetsDir = assetsDir; }

    // パーティクルシステムを Lua fx API へ公開（Application が一度だけ注入）。
    // ポインタはメンバに保持され、Initialize 再実行（シーン切替）後の fx バインドも参照する。
    void SetParticleSystem(ParticleSystem* p) { m_particleSystem = p; }

    void LoadScript(const std::string& filePath);

    // 画面サイズを Lua グローバル SCREEN_W / SCREEN_H に公開（UI レイアウト用）
    void SetScreenSize(int w, int h);

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

    const std::string& GetLastError() const { return m_lastError; }
    void ClearError() { m_lastError.clear(); }

    // ゲーム制御コールバック（Application が注入）。Lua の loadScene/nextScene/quit から呼ばれる。
    using LoadSceneCb = std::function<void(const std::string&)>;
    using VoidCb      = std::function<void()>;
    // ゲーム内 UI 描画コマンド（WP7）: type/x/y/w/h/size/text/r/g/b/a。戻り値はボタン押下判定に使う id。
    using UiTextCb   = std::function<void(float x, float y, const std::string& text, float size, float r, float g, float b, float a)>;
    using UiButtonCb = std::function<bool(float x, float y, float w, float h, const std::string& label)>;
    using UiImageCb  = std::function<void(float x, float y, float w, float h, const std::string& path)>;
    using UiRectCb   = std::function<void(float x, float y, float w, float h, float r, float g, float b, float a, float rounding)>;

    using TransitionCb = std::function<void(const std::string& rel, int type, float dur)>;

    void SetLoadSceneCallback(LoadSceneCb cb) { m_loadSceneCb = std::move(cb); }
    void SetNextSceneCallback(VoidCb cb)      { m_nextSceneCb = std::move(cb); }
    void SetQuitCallback(VoidCb cb)           { m_quitCb = std::move(cb); }
    void SetTransitionCallback(TransitionCb cb) { m_transitionCb = std::move(cb); }
    void SetUiCallbacks(UiTextCb t, UiButtonCb b, UiImageCb i)
    { m_uiTextCb = std::move(t); m_uiButtonCb = std::move(b); m_uiImageCb = std::move(i); }
    void SetUiRectCallback(UiRectCb r) { m_uiRectCb = std::move(r); }

private:
    void RegisterBindings();
    // 高レベルヘルパー(actor/keyDown/cameraFollow 等)をグローバルへ定義する
    // Lua prelude を実行する。全アタッチスクリプトから参照可能になる。
    void LoadPrelude();
    void RegisterPhysicsBindings();

    std::unique_ptr<sol::state> m_lua;
    Scene*         m_scene   = nullptr;
    InputSystem*   m_input   = nullptr;
    Camera*        m_camera  = nullptr;
    AudioSystem*   m_audio   = nullptr;
    PhysicsSystem* m_physics = nullptr;
    ParticleSystem* m_particleSystem = nullptr;
    std::string  m_assetsDir;
    std::string  m_lastError;

    // シーン切替（Shutdown→Initialize で lua state は作り直される）をまたいで
    // 残す数値ストレージ。Lua: saveNum(key,val) / loadNum(key,default)。
    // 例: ゲームシーンでスコアを保存 → リザルトシーンで読み出す。
    std::unordered_map<std::string, double> m_blackboard;

    LoadSceneCb  m_loadSceneCb;
    VoidCb       m_nextSceneCb;
    VoidCb       m_quitCb;
    TransitionCb m_transitionCb;
    UiTextCb    m_uiTextCb;
    UiButtonCb  m_uiButtonCb;
    UiImageCb   m_uiImageCb;
    UiRectCb    m_uiRectCb;
};

} // namespace dx12e
