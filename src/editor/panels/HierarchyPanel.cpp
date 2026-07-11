#include "editor/panels/HierarchyPanel.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
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
#include <cctype>
#include <Windows.h>

namespace dx12e
{

// エンティティの構成コンポーネントから代表アイコンを選ぶ（Hierarchy のノード頭に表示）
static u64 PickEntityIcon(entt::registry& reg, entt::entity e, const EditorUiIcons& ic)
{
    if (reg.all_of<CameraComponent>(e))                            return ic.entCamera;
    if (reg.any_of<PointLight, DirectionalLight, SpotLight>(e))   return ic.entLight;
    if (reg.any_of<UICanvas, UIRect, UIImage, UIText, UIButton>(e)) return ic.entUi;
    if (reg.all_of<MeshRenderer>(e))                              return ic.entMesh;
    if (reg.all_of<AudioSource>(e))                              return ic.entAudio;
    if (reg.any_of<RigidBody, BoxCollider, SphereCollider,
                   CapsuleCollider, ConvexHullCollider, CharacterController>(e))      return ic.entPhysics;
    if (reg.all_of<LuaScript>(e))                                 return ic.entScript;
    return ic.entEmpty;
}

// 種別ごとの tint（Nebula のカラフルなアイコンを単色 PNG に着色して再現）
static ImVec4 PickEntityTint(entt::registry& reg, entt::entity e)
{
    using namespace dx12e::theme;
    if (reg.all_of<CameraComponent>(e))                            return TypeCamera;
    if (reg.any_of<PointLight, DirectionalLight, SpotLight>(e))   return TypeLight;
    if (reg.any_of<UICanvas, UIRect, UIImage, UIText, UIButton>(e)) return TypeUi;
    if (reg.all_of<MeshRenderer>(e))                              return TypeMesh;
    if (reg.all_of<AudioSource>(e))                              return TypeAudio;
    if (reg.any_of<RigidBody, BoxCollider, SphereCollider,
                   CapsuleCollider, ConvexHullCollider, CharacterController>(e))      return TypePhysics;
    if (reg.all_of<LuaScript>(e))                                 return TypeScript;
    return TypeEmpty;
}

// 大文字小文字を無視した部分一致（Hierarchy フィルタ用）
static bool ContainsCI(const std::string& haystack, const char* needle)
{
    if (!needle || !*needle) return true;
    std::string h = haystack, n = needle;
    auto lower = [](std::string& s){ for (char& ch : s) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch))); };
    lower(h); lower(n);
    return h.find(n) != std::string::npos;
}

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

    // ID スコープを明示分離 (ImGui 1.92 の TreeNode + D&D + popup 衝突対策)
    ImGui::PushID(static_cast<int>(static_cast<u32>(e)));

    auto children = GetChildren(reg, e);
    bool hasChildren = !children.empty();
    bool selected = ctx.IsSelected(e);

    // リネーム中はインライン入力を表示
    if (m_renamingEntity == e)
    {
        // ウォームアップ中は SetKeyboardFocusHere を毎フレーム呼ぶ
        // (初回フレームでフォーカスが安定しない問題の回避)
        if (m_renameWarmup > 0)
        {
            ImGui::SetKeyboardFocusHere();
            --m_renameWarmup;
        }

        ImGui::SetNextItemWidth(-1);
        bool entered = ImGui::InputText("##Rename", m_renameBuf, sizeof(m_renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

        // Escape でキャンセル（元の名前に戻す）
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_renamingEntity = entt::null;
            ImGui::PopID();
            return;
        }

        // Enter で確定
        if (entered)
        {
            if (std::strlen(m_renameBuf) > 0 && tag.name != m_renameBuf)
            {
                NameTag before = tag;
                tag.name = m_renameBuf;
                ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<NameTag>>(
                    &reg, e, before, tag, "Rename"));
            }
            m_renamingEntity = entt::null;
            ImGui::PopID();
            return;
        }

        // ウォームアップ完了後のみフォーカスロスで確定判定
        if (m_renameWarmup == 0)
        {
            bool active  = ImGui::IsItemActive();
            bool focused = ImGui::IsItemFocused();
            if (!active && !focused)
            {
                if (std::strlen(m_renameBuf) > 0 && tag.name != m_renameBuf)
                {
                    NameTag before = tag;
                    tag.name = m_renameBuf;
                    ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<NameTag>>(
                        &reg, e, before, tag, "Rename"));
                }
                m_renamingEntity = entt::null;
            }
        }

        ImGui::PopID();
        return;  // リネーム中はツリーノード描画しない
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;

    // 種別アイコンをノード頭に表示（クリック判定は直後の TreeNodeEx が担う）
    // 単色 PNG を種別カラーで tint して Nebula のカラフルなアイコンを再現。
    if (ctx.icons)
    {
        u64 typeIcon = PickEntityIcon(reg, e, *ctx.icons);
        if (typeIcon)
        {
            float h = ImGui::GetTextLineHeight();
            ImGui::ImageWithBg(static_cast<ImTextureID>(typeIcon), ImVec2(h, h),
                ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                PickEntityTint(reg, e));
            ImGui::SameLine(0.0f, 6.0f);
        }
    }

    // ID は PushID で一意化済みなので TreeNodeEx は固定文字列でOK
    bool open = ImGui::TreeNodeEx("##node", flags, "%s", tag.name.c_str());

    bool itemHov = ImGui::IsItemHovered();

    // ダブルクリックでリネーム開始
    if (itemHov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        m_renamingEntity = e;
        m_renameWarmup = 3;  // 3フレーム分フォーカス安定を待つ
        std::memset(m_renameBuf, 0, sizeof(m_renameBuf));
        strncpy_s(m_renameBuf, tag.name.c_str(), _TRUNCATE);
    }
    // シングルクリック選択（Ctrl でマルチ選択）— ダブルクリック時は選択処理をスキップ
    else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctrl)
            ctx.ToggleSelection(e);
        else
            ctx.Select(e);
    }

    // D&D ソース（親子設定用）
    // SourceNoHoldToOpenOthers: ドラッグ中に他ツリーの自動展開を抑制
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers))
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
                {
                    auto& dt = reg.get<Transform>(droppedEntity);
                    Transform before = dt;
                    dt.parent = e;
                    ctx.undoSystem.PushCommand(std::make_unique<TransformCommand>(
                        &reg, droppedEntity, before, dt));
                }
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

    // 右クリックコンテキストメニュー (明示 ID 必須 ・ D&D の後に置く)
    // PushID スコープ内なので ID は "EntityCtx" だけで十分 unique
    if (ImGui::BeginPopupContextItem("EntityCtx", ImGuiPopupFlags_MouseButtonRight))
    {
        if (ImGui::MenuItem("\xe5\x90\x8d\xe5\x89\x8d\xe5\xa4\x89\xe6\x9b\xb4"))  // 名前変更
        {
            m_renamingEntity = e;
            m_renameWarmup = 3;  // 3フレーム分フォーカス安定を待つ
            std::memset(m_renameBuf, 0, sizeof(m_renameBuf));
            strncpy_s(m_renameBuf, tag.name.c_str(), _TRUNCATE);
        }
        if (ImGui::MenuItem("\xe8\xa4\x87\xe8\xa3\xbd"))  // 複製
        {
            ctx.pendingDuplications.push_back(e);
        }
        if (ImGui::MenuItem("プレハブにする"))  // Prefab 化（assets/prefabs へ保存）
        {
            ctx.pendingCreatePrefab = e;
        }
        if (ImGui::MenuItem("\xe5\x89\x8a\xe9\x99\xa4"))  // 削除
        {
            // Undo コマンドは Application の遅延削除処理で積まれる
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
            {
                auto& t = reg.get<Transform>(e);
                Transform before = t;
                t.parent = entt::null;
                ctx.undoSystem.PushCommand(std::make_unique<TransformCommand>(
                    &reg, e, before, t));
            }
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

    ImGui::PopID();
}

void HierarchyPanel::Render(entt::registry& reg, EditorContext& ctx)
{
    ImGui::Begin("\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc");  // Hierarchy

    auto nameView = reg.view<const NameTag>();

    // オブジェクト数（GridPlane は内部用なので除外）
    size_t objCount = 0;
    for (auto [e, tag] : nameView.each())
        if (!reg.all_of<GridPlane>(e)) ++objCount;

    // ---- ヘッダ（件数 + フィルタ。Nebula のヒエラルキー上部に倣う）----
    static char s_filterBuf[64] = {};
    {
        ImGui::PushStyleColor(ImGuiCol_Text, dx12e::theme::TextFaint);
        ImGui::Text("%zu objects", objCount);
        ImGui::PopStyleColor();

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##HierFilter", "Filter", s_filterBuf, sizeof(s_filterBuf));
    }
    ImGui::Spacing();

    if (s_filterBuf[0] != '\0')
    {
        // フィルタ中はツリーを畳んで、名前一致のフラットリストを表示
        for (auto [e, tag] : nameView.each())
        {
            if (reg.all_of<GridPlane>(e)) continue;
            if (!ContainsCI(tag.name, s_filterBuf)) continue;

            ImGui::PushID(static_cast<int>(static_cast<u32>(e)));
            if (ctx.icons)
            {
                u64 ico = PickEntityIcon(reg, e, *ctx.icons);
                if (ico)
                {
                    float h = ImGui::GetTextLineHeight();
                    ImGui::ImageWithBg(static_cast<ImTextureID>(ico), ImVec2(h, h),
                        ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                        PickEntityTint(reg, e));
                    ImGui::SameLine(0.0f, 6.0f);
                }
            }
            if (ImGui::Selectable(tag.name.c_str(), ctx.IsSelected(e)))
            {
                if (ImGui::GetIO().KeyCtrl) ctx.ToggleSelection(e);
                else                        ctx.Select(e);
            }
            ImGui::PopID();
        }
    }
    else
    {
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
    }

    // ヒエラルキーの空白部分への D&D（親子解除）
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
        {
            entt::entity droppedEntity = *static_cast<const entt::entity*>(payload->Data);
            if (reg.valid(droppedEntity) && reg.all_of<Transform>(droppedEntity)
                && reg.get<Transform>(droppedEntity).parent != entt::null)
            {
                auto& t = reg.get<Transform>(droppedEntity);
                Transform before = t;
                t.parent = entt::null;
                ctx.undoSystem.PushCommand(std::make_unique<TransformCommand>(
                    &reg, droppedEntity, before, t));
            }
        }
        ImGui::EndDragDropTarget();
    }

    // ヒエラルキーにフォーカスがある状態で Del キー → 選択エンティティを削除。
    // 実際の削除は pendingDeletions 経由でフレーム境界に行われ、Undo も積まれる。
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && m_renamingEntity == entt::null
        && ctx.HasSelection()
        && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        for (auto e : ctx.selectedEntities)
            if (reg.valid(e)) ctx.pendingDeletions.push_back(e);
        ctx.ClearSelection();
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
        ImGui::Separator();
        if (ImGui::MenuItem("Camera"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__camera__";
            req.position = {0.0f, 2.0f, -5.0f};
            ctx.pendingSpawns.push_back(req);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Directional Light"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__directional_light__";
            req.position = {0.0f, 5.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Point Light"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__point_light__";
            req.position = {0.0f, 3.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Spot Light"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__spot_light__";
            req.position = {0.0f, 5.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Gimmick（ステージ部品）"))
        {
            auto spawnGimmick = [&](const char* marker, float y)
            {
                PendingSpawnRequest req;
                req.modelPath = marker;
                req.position = {0.0f, y, 0.0f};
                ctx.pendingSpawns.push_back(req);
            };
            if (ImGui::MenuItem("Spike Pulse（上下するトゲ）")) spawnGimmick("__gimmick_spike__", 0.7f);
            if (ImGui::MenuItem("Slide Wall（左右に動く壁）")) spawnGimmick("__gimmick_slide__", 0.75f);
            if (ImGui::MenuItem("Static Wall（動かない壁）"))   spawnGimmick("__gimmick_wall__", 0.7f);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Particle Emitter（配置エフェクト）"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__particle_emitter__";
            req.position = {0.0f, 1.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Trigger（イベント範囲）"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__trigger__";
            req.position = {0.0f, 1.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("UI（ゲーム内UI）"))
        {
            // Image/Text/Button は Application 側で「選択エンティティが UI ツリー内なら
            // その子 → 無ければ最初の UICanvas の子 → Canvas 不在なら自動生成」に配置される
            auto spawnUi = [&](const char* marker)
            {
                PendingSpawnRequest req;
                req.modelPath = marker;
                req.position = {0.0f, 0.0f, 0.0f};
                ctx.pendingSpawns.push_back(req);
            };
            if (ImGui::MenuItem("Canvas（UIルート）"))       spawnUi("__ui_canvas__");
            if (ImGui::MenuItem("Image（画像/単色矩形）"))   spawnUi("__ui_image__");
            if (ImGui::MenuItem("Text（テキスト）"))         spawnUi("__ui_text__");
            if (ImGui::MenuItem("Button（ボタン）"))         spawnUi("__ui_button__");
            if (ImGui::MenuItem("Slider（スライダー）"))     spawnUi("__ui_slider__");
            if (ImGui::MenuItem("Toggle（トグル）"))         spawnUi("__ui_toggle__");
            if (ImGui::MenuItem("ScrollView（スクロール）")) spawnUi("__ui_scrollview__");
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace dx12e
