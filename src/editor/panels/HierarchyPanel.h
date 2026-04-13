#pragma once

#include <entt/entt.hpp>

namespace dx12e
{

class EditorContext;

class HierarchyPanel
{
public:
    void Render(entt::registry& reg, EditorContext& ctx);
};

} // namespace dx12e
