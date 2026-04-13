#pragma once

#include <entt/entt.hpp>
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

class InspectorPanel
{
public:
    void Render(entt::registry& reg,
                EditorContext& ctx,
                Camera* camera,
                AudioSystem* audioSystem,
                PhysicsDebugRenderer* physicsDebugRenderer,
                bool& physicsDebugDraw,
                bool& useVsync,
                i32& shadowQualityIndex,
                u32& shadowMapSize,
                bool& shadowMapDirty,
                GameClock* clock,
                Scene* scene);

private:
    // Undo 用: ウィジェット操作開始時のスナップショット
    bool      m_transformEditing = false;
    Transform m_transformSnapshot{};

    bool  m_pbrEditing = false;
    float m_pbrMetallicSnapshot  = -1.0f;
    float m_pbrRoughnessSnapshot = -1.0f;
};

} // namespace dx12e
