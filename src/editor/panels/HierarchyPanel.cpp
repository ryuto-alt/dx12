#include "editor/panels/HierarchyPanel.h"
#include "editor/EditorContext.h"
#include "ecs/Components.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

namespace dx12e
{

void HierarchyPanel::Render(entt::registry& reg, EditorContext& ctx)
{
    ImGui::Begin("\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc");  // Hierarchy

    // Entity count
    auto nameView = reg.view<const NameTag>();
    ImGui::TextDisabled("\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3  (%zu)",
        static_cast<size_t>(nameView.size()));  // Scene
    ImGui::Separator();

    // Entity list
    for (auto [e, tag] : nameView.each())
    {
        bool selected = (e == ctx.selectedEntity);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;

        bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<u32>(e))),
            flags, "%s", tag.name.c_str());

        if (ImGui::IsItemClicked())
            ctx.selectedEntity = selected ? entt::null : e;

        // Right-click context menu
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("\xe5\x89\x8a\xe9\x99\xa4"))  // Delete
            {
                ctx.pendingDeletions.push_back(e);
                if (ctx.selectedEntity == e)
                    ctx.ClearSelection();
            }
            ImGui::EndPopup();
        }

        if (open) ImGui::TreePop();
    }

    ImGui::Separator();

    // Add entity menu
    if (ImGui::Button("\xe2\x9c\x9a \xe3\x82\xa8\xe3\x83\xb3\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x86\xe3\x82\xa3\xe8\xbf\xbd\xe5\x8a\xa0"))  // Add Entity
        ImGui::OpenPopup("AddEntityPopup");

    if (ImGui::BeginPopup("AddEntityPopup"))
    {
        if (ImGui::MenuItem("Box"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__primitive_box__";
            req.position = {0.0f, 0.5f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Sphere"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__primitive_sphere__";
            req.position = {0.0f, 0.5f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Plane"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__primitive_plane__";
            req.position = {0.0f, 0.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Empty"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__empty__";
            req.position = {0.0f, 0.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace dx12e
