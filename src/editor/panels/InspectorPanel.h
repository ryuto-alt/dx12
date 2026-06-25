#pragma once

#include <entt/entt.hpp>
#include <string>
#include "core/Types.h"
#include "ecs/Components.h"

namespace dx12e
{

class EditorContext;
class Camera;
class AudioSystem;
class PhysicsDebugRenderer;
class GameClock;
class Scene;
class ScriptEngine;

class InspectorPanel
{
public:
    // 選択エンティティのコンポーネントを描く（グローバル設定は RenderEngineSettings へ分離）。
    void Render(entt::registry& reg,
                EditorContext& ctx,
                Scene* scene);

    // グローバルなエンジン設定（カメラ速度/シャドウ/オーディオ/VSync/ビルド）を
    // 独立した「エンジン設定」ウィンドウに描く。Inspector からは分離し、下ドックに置く。
    void RenderEngineSettings(EditorContext& ctx,
                              Camera* camera,
                              AudioSystem* audioSystem,
                              PhysicsDebugRenderer* physicsDebugRenderer,
                              bool& physicsDebugDraw,
                              bool& useVsync,
                              i32& shadowQualityIndex,
                              u32& shadowMapSize,
                              bool& shadowMapDirty,
                              GameClock* clock);

    void SetScriptEngine(ScriptEngine* e) { m_scriptEngine = e; }
    void SetAssetsDir(const std::string& d) { m_assetsDir = d; }

    // Undo 用: コンポーネント編集の追跡状態
    template<typename T>
    struct EditState
    {
        bool editing = false;
        T    snapshot{};
    };

private:
    // Undo 用: ウィジェット操作開始時のスナップショット
    bool      m_transformEditing = false;
    Transform m_transformSnapshot{};

    bool  m_pbrEditing = false;
    float m_pbrMetallicSnapshot  = -1.0f;
    float m_pbrRoughnessSnapshot = -1.0f;

    EditState<PointLight>       m_plEdit;
    EditState<DirectionalLight> m_dlEdit;
    EditState<SpotLight>        m_slEdit;
    EditState<AudioSource>      m_audioEdit;
    EditState<CameraComponent>  m_camEdit;
    EditState<Gimmick>          m_gimmickEdit;
    EditState<ParticleEmitter>  m_emitterEdit;
    EditState<RigidBody>        m_rbEdit;
    EditState<BoxCollider>      m_boxColEdit;
    EditState<SphereCollider>   m_sphereColEdit;
    EditState<CapsuleCollider>  m_capsuleColEdit;
    EditState<CharacterController> m_ccEdit;

    ScriptEngine* m_scriptEngine = nullptr;
    std::string   m_assetsDir;
};

} // namespace dx12e
