// UI テストのレーン B の本体。宣言は gui/UiTestHarnessShared.h の lane-B:decl、
// 登録は gui/UiTestHarness.cpp の kTests の lane-B ブロックに足す。
#include "gui/UiTestHarnessShared.h"

#include "core/AppAccess.h"
#include "core/Application.h"
#include "core/PathResolver.h"
#include "core/SafeRemove.h"
#include "editor/AssetBrowserLogic.h"
#include "editor/panels/AssetBrowserPanel.h"
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

namespace
{
// 三角形 1 枚だけの最小の .glb を書く（読み込みエラーのログを出さないため、中身は本物にする）。
void WriteTinyGlb(const fs::path& path)
{
    std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],\"buffers\":[{\"byteLength\":36}]}";
    while (json.size() % 4) json.push_back(' ');
    const float verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint32_t jsonLen = static_cast<uint32_t>(json.size());
    const uint32_t binLen = sizeof(verts);
    const uint32_t total = 12 + 8 + jsonLen + 8 + binLen;
    auto u32 = [](std::ofstream& o, uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    std::ofstream o(path, std::ios::binary);
    o.write("glTF", 4); u32(o, 2); u32(o, total);
    u32(o, jsonLen); o.write("JSON", 4); o.write(json.data(), static_cast<std::streamsize>(json.size()));
    u32(o, binLen);  o.write("BIN\0", 4); o.write(reinterpret_cast<const char*>(verts), sizeof(verts));
}

// 一覧が期待の件数になるまで待つ（走査はワーカースレッド）。
bool WaitEntries(ImGuiTestContext* ctx, AssetBrowserPanel* ab, size_t expect)
{
    for (int i = 0; i < 600; ++i)
    {
        ctx->Yield(1);
        if (!ab->IsScanning() && ab->EntryCount() == expect) { ctx->Yield(2); return true; }
    }
    ctx->LogWarning("一覧が %zu 件になりません（現在 %zu 件）", expect, ab->EntryCount());
    return false;
}

bool WaitExists(ImGuiTestContext* ctx, const fs::path& p, bool want)
{
    for (int i = 0; i < 240; ++i)
    {
        std::error_code ec;
        if (fs::exists(p, ec) == want) { ctx->Yield(3); return true; }
        ctx->Yield(1);
    }
    return false;
}

// 名前欄は ##FileGrid の中の ##Items（子の子）にある。
ImGuiID ItemsWindow(ImGuiTestContext* ctx)
{
    ImGuiID id = ChildWindowId(ctx, "//アセットブラウザ/##FileGrid/##Items");
    if (id != 0) return id;
    ImGuiContext& g = *ctx->UiContext;
    for (ImGuiWindow* w : g.Windows)
        if (w->WasActive && std::strstr(w->Name, "##Items") != nullptr && std::strstr(w->Name, "アセットブラウザ") != nullptr) return w->ID;
    return 0;
}

// 選択 → F2 → 名前欄の初期選択範囲が「0 .. stemBytes」か。
bool BeginF2(ImGuiTestContext* ctx, AssetBrowserPanel* ab, const fs::path& target, int stemBytes)
{
    const ImGuiID win = FocusWindow(ctx, kWinAssets);
    if (win == 0) { LogStep(ctx, "アセットブラウザの窓が無い"); return false; }
    ab->SelectOnly(target);
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_F2);
    ImGuiID items = 0;
    for (int i = 0; i < 30 && !(items = ItemsWindow(ctx), ItemExistsIn(ctx, items, "**/##Rename")); ++i) ctx->Yield(1);
    if (!ItemExistsIn(ctx, items, "**/##Rename")) { LogStep(ctx, "##Rename が出ない（items=%u）", items); return false; }
    ctx->Yield(8);   // warmup（3 フレーム）+ 選択の掛け直し（5 フレーム）を越える
    ctx->SetRef(items);
    const ImGuiID fid = ctx->ItemInfo("**/##Rename", ImGuiTestOpFlags_NoError).ID;
    ImGuiInputTextState* st = ImGui::GetInputTextState(fid);
    if (!st) { LogStep(ctx, "InputTextState が無い"); return false; }
    LogStep(ctx, "F2 の選択: start=%d end=%d（期待 0..%d）", st->GetSelectionStart(), st->GetSelectionEnd(), stemBytes);
    return st->GetSelectionStart() == 0 && st->GetSelectionEnd() == stemBytes;
}

bool ExtConfirmOpen()
{
    ImGuiWindow* w = ImGui::FindWindowByName("##RenameExtConfirm");
    return w != nullptr && w->WasActive;
}

// 本体。IM_CHECK で途中終了しても、呼び出し側が後始末（モーダルを閉じる / マスク復帰 / 削除）をする。
void RenameKeepsExtBody(ImGuiTestContext* ctx, AssetBrowserPanel* ab, const fs::path& work)
{
    const fs::path jp = fs::path(u8"日本.glb");    // パスは UTF-8 から作る（ACP 解釈を避ける）
    const fs::path jpNew = fs::path(u8"新.glb");
    constexpr uint32_t kModelMask = 1u << static_cast<int>(abl::Kind::Model);
    WriteTinyGlb(work / "x.glb");
    WriteTinyGlb(work / jp);
    WriteTinyGlb(work / "y.glb");
    WriteTinyGlb(work / "z.glb");

    LogStep(ctx, "使い捨てフォルダへ移動");
    ab->SetListView(false);
    ab->NavigateTo(work);
    IM_CHECK(WaitEntries(ctx, ab, 4));
    const ImGuiID win = FocusWindow(ctx, kWinAssets);
    IM_CHECK(win != 0);

    LogStep(ctx, "x.glb: F2 → 初期選択は拡張子の前まで → b と打って Enter → b.glb");
    IM_CHECK(BeginF2(ctx, ab, work / "x.glb", 1));
    ctx->KeyChars("b");
    ctx->KeyPress(ImGuiKey_Enter);
    IM_CHECK(WaitExists(ctx, work / "b.glb", true));
    IM_CHECK(!fs::exists(work / "x.glb"));
    IM_CHECK(WaitEntries(ctx, ab, 4));
    IM_CHECK(ab->DrawnCellsLastFrame() > 0);
    LogStep(ctx, "パネル自身の分類: Model だけ表示すると .glb 4 件が全部残る");
    ab->SetKindMask(kModelMask);
    ctx->Yield(3);
    IM_CHECK(ab->VisibleCount() == 4 + 1);   // + 「..」の行（フォルダは常に通る）
    ab->SetKindMask(0);
    ctx->Yield(2);
    LogStep(ctx, "サムネイル: 改名後の b.glb のハンドルが取れる");
    {
        Application* app = App();
        IM_CHECK(app != nullptr);
        if (!app) return;
        const std::string key = (work / "b.glb").string();
        bool got = false;
        for (int i = 0; i < 600 && !got; ++i)
        {
            got = AppAccess::ModelThumbHandle(*app, key) != 0;
            if (!got) ctx->Yield(1);
        }
        IM_CHECK(got);
    }

    LogStep(ctx, "日本.glb: 初期選択は 6 バイト（日本）→ 新 と打って 新.glb");
    IM_CHECK(BeginF2(ctx, ab, work / jp, 6));
    ctx->KeyChars("新");   // 新
    ctx->KeyPress(ImGuiKey_Enter);
    IM_CHECK(WaitExists(ctx, work / jpNew, true));
    IM_CHECK(!fs::exists(work / jp));
    IM_CHECK(WaitEntries(ctx, ab, 4));

    LogStep(ctx, "y.glb: b.png と打つと確認 → キャンセルで不変");
    IM_CHECK(BeginF2(ctx, ab, work / "y.glb", 1));
    ctx->KeyPress(ImGuiKey_A | ImGuiMod_Ctrl);   // 拡張子ごと選び直して上書きする
    ctx->KeyChars("b.png");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(6);
    IM_CHECK(ExtConfirmOpen());
    ctx->SetRef("//##RenameExtConfirm");
    ctx->ItemClick("キャンセル");
    ctx->Yield(6);
    IM_CHECK(!ExtConfirmOpen());
    IM_CHECK(fs::exists(work / "y.glb") && !fs::exists(work / "b.png"));

    LogStep(ctx, "同じ操作で確認 → 「名前変更する」で b.png になる");
    IM_CHECK(BeginF2(ctx, ab, work / "y.glb", 1));
    ctx->KeyPress(ImGuiKey_A | ImGuiMod_Ctrl);   // 拡張子ごと選び直して上書きする
    ctx->KeyChars("b.png");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(6);
    ctx->SetRef("//##RenameExtConfirm");
    ctx->ItemClick("名前変更する");
    IM_CHECK(WaitExists(ctx, work / "b.png", true));
    IM_CHECK(!fs::exists(work / "y.glb"));

    LogStep(ctx, "z.glb → q.png と打つと確認 →「拡張子を保つ」で q.png.glb になる");
    IM_CHECK(BeginF2(ctx, ab, work / "z.glb", 1));
    ctx->KeyPress(ImGuiKey_A | ImGuiMod_Ctrl);
    ctx->KeyChars("q.png");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(6);
    IM_CHECK(ExtConfirmOpen());
    ctx->SetRef("//##RenameExtConfirm");
    ctx->ItemClick("###RenameKeepExt");
    IM_CHECK(WaitExists(ctx, work / "q.png.glb", true));
    IM_CHECK(!fs::exists(work / "z.glb") && !fs::exists(work / "q.png"));

    LogStep(ctx, "パネルの分類: Model だけ表示すると b.png は外れ、.glb 3 件が残る");
    IM_CHECK(WaitEntries(ctx, ab, 4));
    ab->SetKindMask(kModelMask);
    ctx->Yield(3);
    IM_CHECK(ab->VisibleCount() == 3 + 1);   // + 「..」の行
}
}  // namespace

// F2 で名前変更しても拡張子が残る（BUG-09）。.glb は種別 Model（パネルの分類）とサムネイルを保つ。拡張子を変えると確認が出る。
void T_AssetRenameKeepsExt(ImGuiTestContext* ctx)
{
    EditorContext* ed = Editor();
    IM_CHECK(ed != nullptr && ed->assetBrowser != nullptr);
    if (!ed || !ed->assetBrowser) return;
    AssetBrowserPanel* ab = ed->assetBrowser;
    const fs::path assets = fs::path(PathResolver::AssetsDir()).lexically_normal();
    const fs::path work = assets / "__ab_rename_ext_test";
    const uint32_t origMask = ab->KindMask();
    std::error_code ec;
    saferm::RemoveAll(work, ec, assets);
    fs::create_directories(work);

    RenameKeepsExtBody(ctx, ab, work);

    // 成功でも失敗でも通す後始末（残すと後続のテストが巻き込まれる）
    LogStep(ctx, "後始末");
    if (ExtConfirmOpen())
    {
        ctx->SetRef("//##RenameExtConfirm");
        ctx->ItemClick("キャンセル");
        ctx->Yield(4);
    }
    ab->SetKindMask(origMask);
    ab->NavigateTo(assets);
    ctx->Yield(10);
    saferm::RemoveAll(work, ec, assets);
}


// Lua の公開プロパティ: スライダーを動かす → Ctrl+Z で元の値 → Ctrl+Y で戻る。
// 「既定値に戻す」→ Ctrl+Z 1 回で全部戻る（W05-14）。
void T_LuaPropUndo(ImGuiTestContext* ctx)
{
    EditorContext* ed = Editor();
    Application* app = App();
    IM_CHECK(ed != nullptr && app != nullptr);
    if (!ed || !app) return;
    Scene* scene = app->GetScene();
    IM_CHECK(scene != nullptr);
    if (!scene) return;
    auto& reg = scene->GetRegistry();

    const std::string snapshot = app->SaveSceneSnapshot();   // 終わったら元のシーンへ戻す

    LogStep(ctx, "LuaScript（Spinner）を付けたエンティティを作って選ぶ");
    const entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{"UiTestLuaProp"});
    reg.emplace<Transform>(e);
    reg.emplace<EntityGuid>(e, EntityGuid{0x70A14ull});
    // 使い捨てプロジェクトには components/ が無いので、プロパティ宣言だけのスクリプトを assets に書く
    const fs::path scriptDir = fs::path(PathResolver::AssetsDir()) / "scripts";
    const fs::path scriptFile = scriptDir / "uitest_lua_prop.lua";
    {
        std::error_code mec;
        fs::create_directories(scriptDir, mec);
        std::ofstream sf(scriptFile, std::ios::binary);
        sf << "properties = {\n"
              "  { name = \"speed\", type = \"float\", default = 90.0, min = -720, max = 720, label = \"speed\" },\n"
              "  { name = \"axis\", type = \"vec3\", default = {0, 1, 0}, label = \"axis\" },\n"
              "  { name = \"spin\", type = \"bool\", default = true, label = \"spin\" },\n"
              "}\n"
              "function OnUpdate(self, dt) end\n";
    }
    LuaScript ls;
    ls.scriptPath = "scripts/uitest_lua_prop.lua";
    reg.emplace<LuaScript>(e, std::move(ls));
    ed->Select(e);
    ImGuiID insp = FocusWindow(ctx, kWinInspector);
    IM_CHECK(insp != 0);
    ctx->Yield(6);   // SyncScriptProps が既定値（speed = 90）で props を作る
    IM_CHECK(reg.get<LuaScript>(e).props.size() >= 1);
    const double speed0 = reg.get<LuaScript>(e).props[0].num;
    const size_t depth0 = ed->undoSystem.UndoDepth();

    LogStep(ctx, "速度を 200 にする → Undo 1 件 → Ctrl+Z で元の値 → Ctrl+Y で戻る");
    ctx->SetRef(kWinInspector);
    ctx->ItemInputValue("**/speed/##v", 200.0f);
    ctx->Yield(6);
    IM_CHECK(reg.get<LuaScript>(e).props[0].num == 200.0);
    IM_CHECK(ed->undoSystem.UndoDepth() == depth0 + 1);
    ctx->SetRef("//$FOCUSED");
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(reg.get<LuaScript>(e).props[0].num == speed0);
    ctx->KeyPress(ImGuiKey_Y | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(reg.get<LuaScript>(e).props[0].num == 200.0);

    LogStep(ctx, "マウスで数フレーム動かすドラッグは 1 件にまとまる");
    {
        const size_t d = ed->undoSystem.UndoDepth();
        ctx->SetRef(kWinInspector);
        ctx->ItemDragWithDelta("**/speed/##v", ImVec2(40.0f, 0.0f));
        ctx->Yield(6);
        IM_CHECK(reg.get<LuaScript>(e).props[0].num != 200.0);
        IM_CHECK(ed->undoSystem.UndoDepth() == d + 1);
        ctx->SetRef("//$FOCUSED");
        ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
        ctx->Yield(6);
        IM_CHECK(reg.get<LuaScript>(e).props[0].num == 200.0);   // 1 回で掴む前へ戻る
    }

    LogStep(ctx, "掴んだまま選択が変わっても Undo が 1 件積まれる");
    {
        const entt::entity other = reg.create();
        reg.emplace<NameTag>(other, NameTag{"UiTestLuaPropOther"});
        reg.emplace<Transform>(other);
        reg.emplace<EntityGuid>(other, EntityGuid{0x70A15ull});
        const size_t d = ed->undoSystem.UndoDepth();
        ctx->SetRef(kWinInspector);
        ctx->MouseMove("**/speed/##v");
        ctx->MouseDown(0);
        ctx->MouseMoveToPos(ImVec2(ctx->UiContext->IO.MousePos.x + 30.0f, ctx->UiContext->IO.MousePos.y));
        ctx->Yield(3);
        IM_CHECK(ed->undoSystem.UndoDepth() == d);   // 掴んでいる間は積まない
        ed->Select(other);
        ctx->Yield(4);
        ctx->MouseUp(0);
        ctx->Yield(2);
        IM_CHECK(ed->undoSystem.UndoDepth() == d + 1);
        ed->Select(e);
        ctx->Yield(4);
        IM_CHECK(ed->undoSystem.UndoDepth() == d + 1);   // 選び直しても遅れて積まれない
        reg.get<LuaScript>(e).props[0].num = 200.0;
        ctx->Yield(2);
    }

    LogStep(ctx, "既定値に戻す → Ctrl+Z 1 回で全部戻る");
    {
        auto& props = reg.get<LuaScript>(e).props;
        for (auto& p : props)
            if (p.type == ScriptPropType::Bool) p.b = false;      // 既定値（true）から外しておく
        if (props.size() > 1) props[1].vec = {1.0f, 0.0f, 0.0f};  // axis も外す
    }
    ctx->Yield(2);
    const size_t depth1 = ed->undoSystem.UndoDepth();
    const auto before = reg.get<LuaScript>(e).props;
    ctx->SetRef(kWinInspector);
    ctx->ItemClick("**/既定値に戻す");
    ctx->Yield(6);
    IM_CHECK(reg.get<LuaScript>(e).props[0].num == 90.0);
    IM_CHECK(ed->undoSystem.UndoDepth() == depth1 + 1);       // 複合 1 件
    ctx->SetRef("//$FOCUSED");
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
    const auto& after = reg.get<LuaScript>(e).props;
    IM_CHECK(after.size() == before.size());
    for (size_t i = 0; i < before.size() && i < after.size(); ++i)
        IM_CHECK(after[i].num == before[i].num && after[i].b == before[i].b
                 && after[i].vec.x == before[i].vec.x && after[i].vec.y == before[i].vec.y);

    LogStep(ctx, "後始末（元のシーンへ戻す）");
    if (!snapshot.empty())
    {
        app->RequestSceneRestore(snapshot);
        for (int i = 0; i < 240 && !ed->pendingLoadPath.empty(); ++i) ctx->Yield();
        ctx->Yield(60);
        std::error_code ec;
        fs::remove(snapshot, ec);
    }
    std::error_code rec;
    fs::remove(scriptFile, rec);
}

} // namespace uitest
} // namespace dx12e
