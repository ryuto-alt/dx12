// UI テストのレーン B の本体。宣言は gui/UiTestHarnessShared.h の lane-B:decl、
// 登録は gui/UiTestHarness.cpp の kTests の lane-B ブロックに足す。
#include "gui/UiTestHarnessShared.h"

#include "core/Application.h"
#include "core/PathResolver.h"
#include "editor/EditorContext.h"
#include "project/ProjectManager.h"
#include "ecs/Components.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "scene/SceneSettingsHash.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_engine.h>
#include <imgui_te_context.h>
#pragma warning(pop)

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace dx12e
{
namespace uitest
{

namespace
{
namespace fs = std::filesystem;

std::string ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

fs::path ScenesDir()
{
    fs::path d = fs::path(PathResolver::AssetsDir()) / "scenes";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

// 窓の中のボタンをラベルで押す。
void ClickButton(ImGuiTestContext* ctx, ImGuiID win, const char* label)
{
    ctx->SetRef(win);
    ctx->ItemClick(label);
}

// 名前入力ダイアログを開く（create=true: 新規シーン / false: 名前を付けて保存）。
void OpenSceneNameDialog(EditorContext* ed, bool create, const char* name)
{
    ed->newSceneDialogIsCreate = create;
    std::memset(ed->newSceneNameBuf, 0, sizeof(ed->newSceneNameBuf));
    strncpy_s(ed->newSceneNameBuf, name, _TRUNCATE);
    ed->showNewSceneDialog = true;
}
}  // namespace

// 既存の名前で名前を付けて保存 → 確認が出る → キャンセルで既存ファイルが不変 → 上書きで書き換わる。
void T_SaveAsOverwriteConfirm(ImGuiTestContext* ctx)
{
    EditorContext* ed = Editor();
    Application* app = App();
    IM_CHECK(ed != nullptr && app != nullptr);
    if (!ed || !app) return;

    const std::string origPath = ed->currentScenePath;
    const fs::path target = ScenesDir() / "UiTestSaveAsExisting.json";
    const std::string marker = "{\"uitest_marker\":\"keep-me\"}";
    {
        std::ofstream o(target, std::ios::binary);
        o << marker;
    }

    LogStep(ctx, "既存の名前で「名前を付けて保存」を開く");
    OpenSceneNameDialog(ed, /*create=*/false, "UiTestSaveAsExisting");
    ctx->Yield(4);
    ImGuiID dlg = FocusWindow(ctx, "//名前を付けて保存###NewScenePopup");
    IM_CHECK(dlg != 0);
    if (dlg == 0) return;
    ClickButton(ctx, dlg, "保存");
    ctx->Yield(6);

    LogStep(ctx, "上書きの確認が出る（まだ書かれていない）");
    ImGuiID confirm = FocusWindow(ctx, "//名前を付けて保存###SaveAsOverwritePopup");
    IM_CHECK(confirm != 0);
    IM_CHECK(ReadAll(target) == marker);
    IM_CHECK(ed->currentScenePath == origPath);

    LogStep(ctx, "キャンセルで既存ファイルとパスが不変");
    if (confirm != 0) ClickButton(ctx, confirm, "キャンセル");
    ctx->Yield(6);
    IM_CHECK(ReadAll(target) == marker);
    IM_CHECK(ed->currentScenePath == origPath);

    LogStep(ctx, "大文字小文字だけ違う名前でも確認が出る（キャンセルで不変）");
    OpenSceneNameDialog(ed, /*create=*/false, "uitestsaveasexisting");
    ctx->Yield(4);
    dlg = FocusWindow(ctx, "//名前を付けて保存###NewScenePopup");
    IM_CHECK(dlg != 0);
    if (dlg != 0) ClickButton(ctx, dlg, "保存");
    ctx->Yield(6);
    confirm = FocusWindow(ctx, "//名前を付けて保存###SaveAsOverwritePopup");
    IM_CHECK(confirm != 0);
    if (confirm != 0) ClickButton(ctx, confirm, "キャンセル");
    ctx->Yield(6);
    IM_CHECK(ReadAll(target) == marker);
    IM_CHECK(ed->currentScenePath == origPath);

    LogStep(ctx, "もう一度開いて「上書き」でファイルが書き換わりパスが移る");
    OpenSceneNameDialog(ed, /*create=*/false, "UiTestSaveAsExisting");
    ctx->Yield(4);
    dlg = FocusWindow(ctx, "//名前を付けて保存###NewScenePopup");
    IM_CHECK(dlg != 0);
    if (dlg != 0) ClickButton(ctx, dlg, "保存");
    ctx->Yield(6);
    confirm = FocusWindow(ctx, "//名前を付けて保存###SaveAsOverwritePopup");
    IM_CHECK(confirm != 0);
    if (confirm != 0) ClickButton(ctx, confirm, "上書き");
    ctx->Yield(6);
    IM_CHECK(ReadAll(target) != marker);
    IM_CHECK(fs::path(ed->currentScenePath).filename() == "UiTestSaveAsExisting.json");

    LogStep(ctx, "後始末（元のパスへ戻す）");
    ed->currentScenePath = origPath;
    if (!origPath.empty()) ProjectManager::SaveLastOpenedScene(origPath);
    std::error_code ec;
    fs::remove(target, ec);
}

// 未保存の編集がある状態で Ctrl+N 相当（名前 Forest）→ 確認で「保存して続行」→
// 旧シーンのファイルに旧シーンの中身、scenes/Forest.json が実在、パスと「前回のシーン」が同じ。
void T_NewSceneNameKept(ImGuiTestContext* ctx)
{
    EditorContext* ed = Editor();
    Application* app = App();
    IM_CHECK(ed != nullptr && app != nullptr);
    if (!ed || !app) return;
    Scene* scene = app->GetScene();
    IM_CHECK(scene != nullptr);
    if (!scene) return;

    const std::string origPath = ed->currentScenePath;
    const bool origAi = ed->aiSessionEver;
    const std::string snapshot = app->SaveSceneSnapshot();   // 終わったら元のシーンへ戻す
    const fs::path oldFile = ScenesDir() / "UiTestOld.json";
    const fs::path forest  = ScenesDir() / "UiTestForest.json";
    std::error_code ec;
    fs::remove(oldFile, ec);
    fs::remove(forest, ec);

    LogStep(ctx, "旧シーン（印の付いたエンティティ入り）を UiTestOld.json として保存し、未保存の編集を作る");
    scene->SpawnBox("UiTestOldMarker", {0, 0.5f, 0}, {0, 0, 0}, {1, 1, 1});
    IM_CHECK(SceneSerializer::Save(*scene, oldFile.string(), PathResolver::AssetsDir()));
    ed->currentScenePath = oldFile.string();
    ed->MarkSceneSaved(SceneSettingsFingerprint(*scene));
    scene->SpawnBox("UiTestUnsavedMarker", {1, 0.5f, 0}, {0, 0, 0}, {1, 1, 1});
    ed->savedSettingsHash ^= 1;   // 汚れた状態にする
    IM_CHECK(ed->IsSceneDirty());
    ed->aiSessionEver = false;    // MCP 接続済みだと確認が出ず黙って保存されるため

    LogStep(ctx, "Ctrl+N の名前入力で UiTestForest を作成 → 未保存の確認");
    OpenSceneNameDialog(ed, /*create=*/true, "UiTestForest");
    ctx->Yield(4);
    ImGuiID dlg = FocusWindow(ctx, "//新規シーン###NewScenePopup");
    IM_CHECK(dlg != 0);
    if (dlg != 0) ClickButton(ctx, dlg, "作成");
    ctx->Yield(8);
    IM_CHECK(ed->currentScenePath == oldFile.string());   // 確認の前に新しいパスへ変わらない
    IM_CHECK(!fs::exists(forest, ec));
    IM_CHECK(ed->showUnsavedConfirm);

    LogStep(ctx, "「保存して続行」");
    ImGuiID confirm = FocusWindow(ctx, "//##UnsavedConfirm");
    IM_CHECK(confirm != 0);
    if (confirm != 0) ClickButton(ctx, confirm, "保存して続行");
    for (int i = 0; i < 240 && (ed->pendingNewScene || ed->showUnsavedConfirm); ++i) ctx->Yield();
    ctx->Yield(10);

    LogStep(ctx, "旧ファイルに旧シーンの中身、Forest が実在、パスと前回のシーンが Forest");
    IM_CHECK(fs::exists(forest, ec));
    const std::string oldJson = ReadAll(oldFile);
    IM_CHECK(oldJson.find("UiTestOldMarker") != std::string::npos);
    IM_CHECK(oldJson.find("UiTestUnsavedMarker") != std::string::npos);
    const std::string forestJson = ReadAll(forest);
    IM_CHECK(forestJson.find("UiTestOldMarker") == std::string::npos);
    IM_CHECK(fs::path(ed->currentScenePath).filename() == "UiTestForest.json");
    IM_CHECK(fs::path(ProjectManager::LoadLastOpenedScene()).filename() == "UiTestForest.json");

    LogStep(ctx, "後始末（元のシーンとパスへ戻す）");
    ed->aiSessionEver = origAi;
    if (!snapshot.empty())
    {
        app->RequestSceneRestore(snapshot);
        for (int i = 0; i < 240 && !ed->pendingLoadPath.empty(); ++i) ctx->Yield();
        ctx->Yield(60);
        fs::remove(snapshot, ec);
    }
    ed->currentScenePath = origPath;
    if (!origPath.empty()) ProjectManager::SaveLastOpenedScene(origPath);
    fs::remove(oldFile, ec);
    fs::remove(forest, ec);
}

// 未保存の確認でキャンセル → 前回の保存先が残らず、その後の無題の新規シーンは空パスになる。
void T_NewSceneCancelKeepsNoPath(ImGuiTestContext* ctx)
{
    EditorContext* ed = Editor();
    Application* app = App();
    IM_CHECK(ed != nullptr && app != nullptr);
    if (!ed || !app) return;
    Scene* scene = app->GetScene();
    IM_CHECK(scene != nullptr);
    if (!scene) return;

    const std::string origPath = ed->currentScenePath;
    const bool origAi = ed->aiSessionEver;
    const std::string snapshot = app->SaveSceneSnapshot();
    const fs::path cancelled = ScenesDir() / "UiTestCancelled.json";
    std::error_code ec;
    fs::remove(cancelled, ec);

    LogStep(ctx, "未保存の編集を作り、Ctrl+N の名前入力で作成 → 確認でキャンセル");
    scene->SpawnBox("UiTestUnsavedMarker2", {1, 0.5f, 0}, {0, 0, 0}, {1, 1, 1});
    ed->savedSettingsHash ^= 1;
    IM_CHECK(ed->IsSceneDirty());
    ed->aiSessionEver = false;
    OpenSceneNameDialog(ed, /*create=*/true, "UiTestCancelled");
    ctx->Yield(4);
    ImGuiID dlg = FocusWindow(ctx, "//新規シーン###NewScenePopup");
    IM_CHECK(dlg != 0);
    if (dlg != 0) ClickButton(ctx, dlg, "作成");
    ctx->Yield(8);
    IM_CHECK(ed->showUnsavedConfirm);
    ImGuiID confirm = FocusWindow(ctx, "//##UnsavedConfirm");
    IM_CHECK(confirm != 0);
    if (confirm != 0) ClickButton(ctx, confirm, "キャンセル");
    for (int i = 0; i < 240 && (ed->pendingNewScene || ed->showUnsavedConfirm); ++i) ctx->Yield();
    ctx->Yield(10);

    LogStep(ctx, "要求ごと捨てられ、保存先が残らず、ファイルも作られない");
    IM_CHECK(!ed->pendingNewScene);
    IM_CHECK(ed->pendingNewScenePath.empty());
    IM_CHECK(ed->currentScenePath == origPath);
    IM_CHECK(!fs::exists(cancelled, ec));

    LogStep(ctx, "その後の無題の新規シーン（MCP new_scene 相当）は空パス");
    ed->pendingNewSceneSkipConfirm = true;
    ed->pendingNewScene = true;
    for (int i = 0; i < 240 && ed->pendingNewScene; ++i) ctx->Yield();
    ctx->Yield(10);
    IM_CHECK(ed->currentScenePath.empty());
    IM_CHECK(!fs::exists(cancelled, ec));

    LogStep(ctx, "後始末（元のシーンとパスへ戻す）");
    ed->aiSessionEver = origAi;
    if (!snapshot.empty())
    {
        app->RequestSceneRestore(snapshot);
        for (int i = 0; i < 240 && !ed->pendingLoadPath.empty(); ++i) ctx->Yield();
        ctx->Yield(60);
        fs::remove(snapshot, ec);
    }
    ed->currentScenePath = origPath;
    if (!origPath.empty()) ProjectManager::SaveLastOpenedScene(origPath);
}

// 仮想入力で Box を作る → Play → Stop → 選択が Box、Ctrl+Z で Box が消える（W05-11）。
void T_PlayStopKeepsUndo(ImGuiTestContext* ctx)
{
    EditorContext* ed = Editor();
    Application* app = App();
    IM_CHECK(ed != nullptr && app != nullptr);
    if (!ed || !app) return;
    Scene* scene = app->GetScene();
    IM_CHECK(scene != nullptr);
    if (!scene) return;

    const std::string snapshot = app->SaveSceneSnapshot();   // 終わったら元のシーンへ戻す
    const std::string origPath = ed->currentScenePath;
    auto& reg = scene->GetRegistry();

    LogStep(ctx, "Play に要るカメラを足し、仮想入力で Box を作る");
    {
        const entt::entity cam = reg.create();
        reg.emplace<NameTag>(cam, NameTag{"UiTestPlayCam"});
        reg.emplace<Transform>(cam);
        CameraComponent cc; cc.isActive = true;
        reg.emplace<CameraComponent>(cam, cc);
    }
    const size_t before = reg.view<NameTag>().size();
    const size_t depth0 = ed->undoSystem.UndoDepth();
    std::vector<entt::entity> existing;
    for (auto e : reg.view<NameTag>()) existing.push_back(e);
    AddEntity(ctx, "Point Light");   // Mesh を持たない種別（Mesh 所有時は履歴を捨てる暫定仕様のため）
    IM_CHECK(reg.view<NameTag>().size() == before + 1);
    IM_CHECK(ed->undoSystem.UndoDepth() == depth0 + 1);
    entt::entity box = entt::null;   // 新しいエンティティの自動選択は W05-21 の担当なので、ここでは差分から見つけて選ぶ
    for (auto e : reg.view<NameTag>())
        if (std::find(existing.begin(), existing.end(), e) == existing.end()) box = e;
    if (box != entt::null) ed->Select(box);
    IM_CHECK(box != entt::null && reg.valid(box));
    const size_t depth = ed->undoSystem.UndoDepth();

    // Scene が Mesh を所有していると履歴は捨てられる（解放済み Mesh* の書き戻しを避ける暫定仕様）。
    const bool ownsMeshes = scene->GetOwnedMeshCount() > 0;

    LogStep(ctx, "Play → Stop");
    app->RequestMode(Application::EngineMode::Playing);
    for (int i = 0; i < 240 && app->GetEngineMode() != Application::EngineMode::Playing; ++i) ctx->Yield();
    IM_CHECK(app->GetEngineMode() == Application::EngineMode::Playing);
    ctx->Yield(10);
    app->RequestMode(Application::EngineMode::Editor);
    for (int i = 0; i < 240 && app->GetEngineMode() != Application::EngineMode::Editor; ++i) ctx->Yield();
    IM_CHECK(app->GetEngineMode() == Application::EngineMode::Editor);
    ctx->Yield(10);

    LogStep(ctx, "Undo 履歴と選択が残り、id が同じで、Ctrl+Z でエンティティが消える");
    if (ownsMeshes)
    {
        // Mesh 所有シーン: 履歴は捨てられるが、選択は GUID から戻り、落ちないこと。
        IM_CHECK(ed->undoSystem.UndoDepth() == 0);
        IM_CHECK(ed->selectedEntity != entt::null);
        goto cleanup;
    }
    IM_CHECK(ed->undoSystem.UndoDepth() == depth);
    IM_CHECK(ed->selectedEntity == box);          // id も選択も同じ
    IM_CHECK(reg.valid(box));
    ctx->SetRef("//$FOCUSED");
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(reg.view<NameTag>().size() == before);
    IM_CHECK(!reg.valid(box) || !reg.all_of<NameTag>(box));

cleanup:
    LogStep(ctx, "後始末（元のシーンへ戻す）");
    if (!snapshot.empty())
    {
        app->RequestSceneRestore(snapshot);
        for (int i = 0; i < 240 && !ed->pendingLoadPath.empty(); ++i) ctx->Yield();
        ctx->Yield(60);
        std::error_code ec;
        fs::remove(snapshot, ec);
    }
    ed->currentScenePath = origPath;
}

} // namespace uitest
} // namespace dx12e
