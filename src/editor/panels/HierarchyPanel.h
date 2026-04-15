#pragma once

#include <entt/entt.hpp>
#include <string>

namespace dx12e
{

class EditorContext;
class Scene;

class HierarchyPanel
{
public:
    void Render(entt::registry& reg, EditorContext& ctx);
    void SetAssetsDir(const std::string& assetsDir) { m_assetsDir = assetsDir; }

private:
    void DrawEntityNode(entt::registry& reg, EditorContext& ctx, entt::entity e);

    // リネーム
    entt::entity m_renamingEntity = entt::null;
    char m_renameBuf[128] = {};

    std::string m_assetsDir;
};

} // namespace dx12e
