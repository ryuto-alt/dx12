#pragma once

#include <entt/entt.hpp>

namespace dx12e
{

class EditorContext;
class Scene;

class HierarchyPanel
{
public:
    void Render(entt::registry& reg, EditorContext& ctx);

private:
    void DrawEntityNode(entt::registry& reg, EditorContext& ctx, entt::entity e);

    // リネーム
    entt::entity m_renamingEntity = entt::null;
    char m_renameBuf[128] = {};
};

} // namespace dx12e
