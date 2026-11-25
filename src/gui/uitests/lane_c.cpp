// UI テストのレーン C の本体。宣言は gui/UiTestHarnessShared.h の lane-C:decl、
// 登録は gui/UiTestHarness.cpp の kTests の lane-C ブロックに足す。
#include "gui/UiTestHarnessShared.h"

#include "core/Application.h"
#include "ecs/Components.h"
#include "editor/EditorContext.h"
#include "scene/Scene.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#pragma warning(pop)

namespace dx12e
{
namespace uitest
{

namespace
{
// 生成要求を積んで（人の操作と同じ経路）、選択が「新しいエンティティ 1 件」になるか確かめる。
void ExpectSelectsNewEntity(ImGuiTestContext* ctx, EditorContext* ed, Scene* scene,
                            const char* what, void (*spawn)(ImGuiTestContext*, EditorContext*))
{
    LogStep(ctx, "W05-21: %s", what);
    auto& reg = scene->GetRegistry();
    const size_t before = reg.storage<NameTag>().size();
    ed->ClearSelection();
    spawn(ctx, ed);
    ctx->Yield(8);   // 生成はフレーム境界の遅延処理
    IM_CHECK_EQ(reg.storage<NameTag>().size(), before + 1);
    IM_CHECK_EQ(ed->selectedEntities.size(), (size_t)1);
    IM_CHECK(ed->selectedEntity != entt::null);
    IM_CHECK(reg.valid(ed->selectedEntity));
    IM_CHECK(ed->selectedEntities.size() == 1 && ed->selectedEntities[0] == ed->selectedEntity);
    IM_CHECK(!ed->revealInHierarchy);   // ヒエラルキーが露出を処理した
}
} // namespace

// W05-21: 3 経路（「＋エンティティ追加」/ Ctrl+K・.glb のダブルクリックが積む生成要求）で
// 新しいエンティティが選択され、Undo で選択が無効 id を指さない。
void T_CreateEntitySelects(ImGuiTestContext* ctx)
{
    EditorContext* ed = Editor();
    Application*   app = App();
    if (!ed || !app || !app->GetScene()) { IM_ERRORF("Application / Scene を取得できません"); return; }
    Scene* scene = app->GetScene();

    // 経路 1: ヒエラルキーの「＋エンティティ追加」
    ExpectSelectsNewEntity(ctx, ed, scene, "ヒエラルキーの＋で Box",
        [](ImGuiTestContext* c, EditorContext*) { AddEntity(c, "Box"); });

    // 経路 2: Ctrl+K（コマンドパレット）は SpawnItem と同じ生成要求を積む
    ExpectSelectsNewEntity(ctx, ed, scene, "Ctrl+K 相当の生成要求（Sphere）",
        [](ImGuiTestContext*, EditorContext* e) {
            PendingSpawnRequest r; r.modelPath = "__primitive_sphere__"; r.position = {0, 0.5f, 0};
            e->pendingSpawns.push_back(r); });

    // 経路 3: アセットブラウザのダブルクリックも同じ生成要求（モデルパスの代わりに Empty で代用）
    ExpectSelectsNewEntity(ctx, ed, scene, "アセットのダブルクリック相当の生成要求（Empty）",
        [](ImGuiTestContext*, EditorContext* e) {
            PendingSpawnRequest r; r.modelPath = "__empty__";
            e->pendingSpawns.push_back(r); });

    // Undo で生成を戻したとき、選択が無効な id を指さない
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(8);
    auto& reg = scene->GetRegistry();
    for (auto s : ed->selectedEntities) IM_CHECK(reg.valid(s));
    IM_CHECK(ed->selectedEntity == entt::null || reg.valid(ed->selectedEntity));
}

} // namespace uitest
} // namespace dx12e
