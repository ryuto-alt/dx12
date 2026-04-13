#pragma once

#include <entt/entt.hpp>
#include "core/Types.h"

namespace dx12e
{

class EditorContext;
class Camera;
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
};

} // namespace dx12e
