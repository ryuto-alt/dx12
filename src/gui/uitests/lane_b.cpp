// UI テストのレーン B の本体。宣言は gui/UiTestHarnessShared.h の lane-B:decl、
// 登録は gui/UiTestHarness.cpp の kTests の lane-B ブロックに足す。
#include "gui/UiTestHarnessShared.h"

#include "core/Application.h"
#include "core/PathResolver.h"
#include "editor/EditorContext.h"
#include "project/ProjectManager.h"
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

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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

// 窓の中の項目を「描画順の番号」でクリックする。
// DebugLabel は日本語が二重にエンコードされ（実測）、ラベルの ID パスも一致しないため、ラベルでは引けない。
// expectCount は項目数の確認（レイアウトが変わって別の項目を押さないための保険）。
bool ClickItemIndex(ImGuiTestContext* ctx, ImGuiID win, int index, int expectCount)
{
    ctx->SetRef(win);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, win, 3);
    if (items.GetSize() != expectCount || index >= items.GetSize() || items[index] == nullptr || items[index]->ID == 0)
    {
        IM_ERRORF("項目の並びが想定と違います: count=%d（想定 %d）, index=%d", items.GetSize(), expectCount, index);
        return false;
    }
    ctx->MouseMove(items[index]->ID);
    ctx->MouseClick(0);
    return true;
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
    ImGuiID dlg = FocusWindow(ctx, "//名前を付けて保存##NewScenePopup");
    IM_CHECK(dlg != 0);
    if (dlg == 0) return;
    ClickItemIndex(ctx, dlg, 1, 3);
    ctx->Yield(6);

    LogStep(ctx, "上書きの確認が出る（まだ書かれていない）");
    ImGuiID confirm = FocusWindow(ctx, "//名前を付けて保存##SaveAsOverwritePopup");
    IM_CHECK(confirm != 0);
    IM_CHECK(ReadAll(target) == marker);
    IM_CHECK(ed->currentScenePath == origPath);

    LogStep(ctx, "キャンセルで既存ファイルとパスが不変");
    if (confirm != 0) ClickItemIndex(ctx, confirm, 1, 2);
    ctx->Yield(6);
    IM_CHECK(ReadAll(target) == marker);
    IM_CHECK(ed->currentScenePath == origPath);

    LogStep(ctx, "もう一度開いて「上書き」でファイルが書き換わりパスが移る");
    OpenSceneNameDialog(ed, /*create=*/false, "UiTestSaveAsExisting");
    ctx->Yield(4);
    dlg = FocusWindow(ctx, "//名前を付けて保存##NewScenePopup");
    IM_CHECK(dlg != 0);
    if (dlg != 0) ClickItemIndex(ctx, dlg, 1, 3);
    ctx->Yield(6);
    confirm = FocusWindow(ctx, "//名前を付けて保存##SaveAsOverwritePopup");
    IM_CHECK(confirm != 0);
    if (confirm != 0) ClickItemIndex(ctx, confirm, 0, 2);
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
    ImGuiID dlg = FocusWindow(ctx, "//新規シーン##NewScenePopup");
    IM_CHECK(dlg != 0);
    if (dlg != 0) ClickItemIndex(ctx, dlg, 1, 3);
    ctx->Yield(8);
    IM_CHECK(ed->currentScenePath == oldFile.string());   // 確認の前に新しいパスへ変わらない
    IM_CHECK(!fs::exists(forest, ec));
    IM_CHECK(ed->showUnsavedConfirm);

    LogStep(ctx, "「保存して続行」");
    ImGuiID confirm = FocusWindow(ctx, "//##UnsavedConfirm");
    IM_CHECK(confirm != 0);
    if (confirm != 0) ClickItemIndex(ctx, confirm, 0, 3);
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

} // namespace uitest
} // namespace dx12e
