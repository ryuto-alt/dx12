#include "editor/panels/HierarchyPanel.h"
#include "editor/EditorContext.h"
#include "editor/UndoSystem.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <filesystem>
#include <vector>
#include <cstdio>
#include <Windows.h>

namespace dx12e
{

// 子エンティティ列挙ヘルパー
static std::vector<entt::entity> GetChildren(entt::registry& reg, entt::entity parent)
{
    std::vector<entt::entity> children;
    auto view = reg.view<const Transform>();
    for (auto [e, t] : view.each())
    {
        if (t.parent == parent)
            children.push_back(e);
    }
    return children;
}

void HierarchyPanel::DrawEntityNode(entt::registry& reg, EditorContext& ctx, entt::entity e)
{
    if (!reg.all_of<NameTag>(e)) return;
    auto& tag = reg.get<NameTag>(e);

    auto children = GetChildren(reg, e);
    bool hasChildren = !children.empty();
    bool selected = ctx.IsSelected(e);

    // リネーム中はインライン入力を表示
    if (m_renamingEntity == e)
    {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##Rename", m_renameBuf, sizeof(m_renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
        {
            if (std::strlen(m_renameBuf) > 0)
                tag.name = m_renameBuf;
            m_renamingEntity = entt::null;
        }
        // フォーカスが外れたら確定
        if (!ImGui::IsItemActive() && m_renamingEntity == e)
        {
            if (std::strlen(m_renameBuf) > 0)
                tag.name = m_renameBuf;
            m_renamingEntity = entt::null;
        }
        return;  // リネーム中はツリーノード描画しない
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;

    bool open = ImGui::TreeNodeEx(
        reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<u32>(e))),
        flags, "%s", tag.name.c_str());

    // ダブルクリックでリネーム開始
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        m_renamingEntity = e;
        std::memset(m_renameBuf, 0, sizeof(m_renameBuf));
        strncpy_s(m_renameBuf, tag.name.c_str(), _TRUNCATE);
    }
    // シングルクリック選択（Ctrl でマルチ選択）
    else if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
    {
        bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctrl)
            ctx.ToggleSelection(e);
        else
            ctx.Select(selected ? entt::null : e);
    }

    // D&D ソース（親子設定用）
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
    {
        ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &e, sizeof(entt::entity));
        ImGui::Text("%s", tag.name.c_str());
        ImGui::EndDragDropSource();
    }

    // D&D ターゲット（ドロップされたら子にする）
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
        {
            entt::entity droppedEntity = *static_cast<const entt::entity*>(payload->Data);
            if (droppedEntity != e && reg.valid(droppedEntity) && reg.all_of<Transform>(droppedEntity))
            {
                // 循環防止: e が droppedEntity の子孫でないかチェック
                bool isCyclic = false;
                entt::entity check = e;
                while (check != entt::null && reg.valid(check) && reg.all_of<Transform>(check))
                {
                    if (check == droppedEntity) { isCyclic = true; break; }
                    check = reg.get<Transform>(check).parent;
                }
                if (!isCyclic)
                    reg.get<Transform>(droppedEntity).parent = e;
            }
        }
        if (const ImGuiPayload* scriptPayload = ImGui::AcceptDragDropPayload("DND_SCRIPT"))
        {
            const char* pathCStr = static_cast<const char*>(scriptPayload->Data);
            std::string absPath(pathCStr);

            // assets 相対パスに変換
            namespace fs = std::filesystem;
            auto abs = fs::path(absPath).lexically_normal().string();
            auto base = fs::path(m_assetsDir).lexically_normal().string();
            std::replace(abs.begin(), abs.end(), '\\', '/');
            std::replace(base.begin(), base.end(), '\\', '/');
            std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;

            char dbgBuf[512];
            snprintf(dbgBuf, sizeof(dbgBuf),
                "[Hierarchy DND_SCRIPT] entity=%u abs=%s base=%s rel=%s\n",
                static_cast<u32>(e), abs.c_str(), base.c_str(), rel.c_str());
            OutputDebugStringA(dbgBuf);

            ctx.pendingScriptAttachments.push_back({e, rel});
        }
        ImGui::EndDragDropTarget();
    }

    // 右クリックコンテキストメニュー
    // ImGui 1.92+ は BeginPopupContextItem(NULL) で last-item ID が D&D 後に
    // 不安定になることがあるため、エンティティハンドルで明示的な ID を渡す
    char popupId[32];
    snprintf(popupId, sizeof(popupId), "##EntityCtx_%u", static_cast<u32>(e));
    if (ImGui::BeginPopupContextItem(popupId))
    {
        if (ImGui::MenuItem("\xe5\x90\x8d\xe5\x89\x8d\xe5\xa4\x89\xe6\x9b\xb4"))  // 名前変更
        {
            m_renamingEntity = e;
            std::memset(m_renameBuf, 0, sizeof(m_renameBuf));
            strncpy_s(m_renameBuf, tag.name.c_str(), _TRUNCATE);
        }
        if (ImGui::MenuItem("\xe5\x89\x8a\xe9\x99\xa4"))  // 削除
        {
            // Undo 用にデータを保存
            DeletedEntityData data;
            if (reg.all_of<NameTag>(e))
                data.name = reg.get<NameTag>(e).name;
            if (reg.all_of<Transform>(e))
                data.transform = reg.get<Transform>(e);
            if (reg.all_of<MeshRenderer>(e))
            {
                auto& mr = reg.get<MeshRenderer>(e);
                data.modelPath = mr.modelPath;
                data.overrideMetallic = mr.overrideMetallic;
                data.overrideRoughness = mr.overrideRoughness;
            }

            ctx.undoSystem.PushCommand(std::make_unique<DeleteEntityCommand>(
                nullptr, &reg, e, data));

            ctx.pendingDeletions.push_back(e);
            if (ctx.IsSelected(e))
            {
                auto& sel = ctx.selectedEntities;
                sel.erase(std::remove(sel.begin(), sel.end(), e), sel.end());
                ctx.selectedEntity = sel.empty() ? entt::null : sel.back();
            }
        }

        // 親子解除
        if (reg.all_of<Transform>(e) && reg.get<Transform>(e).parent != entt::null)
        {
            if (ImGui::MenuItem("\xe8\xa6\xaa\xe3\x81\x8b\xe3\x82\x89\xe5\xa4\x96\xe3\x81\x99"))  // 親から外す
                reg.get<Transform>(e).parent = entt::null;
        }

        ImGui::EndPopup();
    }

    // 子ノード描画
    if (open)
    {
        for (auto child : children)
            DrawEntityNode(reg, ctx, child);
        ImGui::TreePop();
    }
}

void HierarchyPanel::Render(entt::registry& reg, EditorContext& ctx)
{
    ImGui::Begin("\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc");  // Hierarchy

    auto nameView = reg.view<const NameTag>();
    ImGui::TextDisabled("\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3  (%zu)",
        static_cast<size_t>(nameView.size()));
    ImGui::Separator();

    // ルートノードのみ列挙（parent が null、GridPlane は非表示）
    for (auto [e, tag] : nameView.each())
    {
        if (reg.all_of<GridPlane>(e)) continue;

        bool isRoot = true;
        if (reg.all_of<Transform>(e))
        {
            auto& t = reg.get<Transform>(e);
            if (t.parent != entt::null && reg.valid(t.parent))
                isRoot = false;
        }
        if (isRoot)
            DrawEntityNode(reg, ctx, e);
    }

    // ヒエラルキーの空白部分への D&D（親子解除）
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
        {
            entt::entity droppedEntity = *static_cast<const entt::entity*>(payload->Data);
            if (reg.valid(droppedEntity) && reg.all_of<Transform>(droppedEntity))
                reg.get<Transform>(droppedEntity).parent = entt::null;
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Separator();

    // Add entity menu
    if (ImGui::Button("\xe2\x9c\x9a \xe3\x82\xa8\xe3\x83\xb3\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x86\xe3\x82\xa3\xe8\xbf\xbd\xe5\x8a\xa0"))
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
