#pragma once

#include <entt/entt.hpp>
#include "core/Types.h"
#include "ecs/Components.h"

namespace dx12e
{

class EditorContext;
class Camera;
class Scene;
class Window;

class SceneViewPanel
{
public:
    void RenderGizmo(entt::registry& reg,
                     EditorContext& ctx,
                     Camera* camera,
                     f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    void HandlePicking(entt::registry& reg,
                       EditorContext& ctx,
                       Camera* camera,
                       f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    void HandleDeleteKey(entt::registry& reg,
                         EditorContext& ctx,
                         Scene* scene,
                         f32 vpX, f32 vpY, f32 vpW, f32 vpH);

private:
    // ギズモ操作中の開始時 Transform（Undo用）
    bool      m_gizmoWasUsing = false;
    Transform m_gizmoStartTransform{};
};

} // namespace dx12e
