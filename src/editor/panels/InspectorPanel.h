#pragma once

#include <entt/entt.hpp>
#include "core/Types.h"

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
};

} // namespace dx12e
