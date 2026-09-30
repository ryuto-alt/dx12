#include "gui/UiTestHarness.h"
#include "core/Application.h"
#include "core/CrashHandler.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/Version.h"
#include "core/VirtualGuard.h"   // テスト中は ShellExecute / ダイアログを止める（guard::TestRunActive）
#include "ecs/Components.h"
#include "editor/EditorContext.h"
#include "editor/InspectorLogic.h"   // [I] insp::SplitList（最近使ったコンポーネントの確認）
#include "editor/EditorPrefs.h"       // [I] 折りたたみ / 最近使ったの永続値
#include "editor/PropertyGrid.h"       // [I] pg::InternalClipboard（値コピーの確認）
#include "editor/panels/AssetBrowserPanel.h"   // [A] アセットブラウザの計測 / 操作
#include "editor/AssetBrowserLogic.h"
#include <shlobj.h>   // DROPFILES（OS ファイルドロップのテスト）
#include "editor/SelectionOutline.h"   // ビューポートの輪郭（マスク描画の実績を見る）
#include "editor/ViewportLogic.h"     // アスペクト / スナップのプリセット
#include "editor/EditorIcons.h"   // ICON_PLUS（追加ボタンの参照名）
#include "editor/EditorCommands.h" // コマンド表（ショートカット / window.* コマンド）
#include "editor/ToolWindows.h"    // ツール窓レジストリ
#include "editor/WorkspaceLogic.h"   // ワークスペース / レイアウト（1b W）
#include "editor/BottomDock.h"       // 下部ドックの登録タブ
#include "editor/EditorPrefs.h"       // レイアウトの保存（テスト中は書き込まれない）
#include "editor/EditorCommandTable.h"
#include "ecs/EditorFlags.h"      // [H] 非表示 / ロック
#include "editor/HierarchyLogic.h"   // [H] ヒエラルキーの純ロジック
#include "editor/Toast.h"          // トースト通知
#include "editor/EditorTheme.h"   // DPI: theme::Px（診断パネルの寸法）
#include "editor/UiWidgets.h"     // DPI テストの等幅フォント測定（ui::PushMono）
#include "editor/LauncherScreen.h"  // プロジェクトランチャーの検査
#include "project/LauncherLogic.h"
#include "core/DpiScale.h"         // DPI: 表示倍率テスト（--dpi-scale と同じオーバーライド）
#include "gui/DeepDiagnostics.h"
#include "scene/Scene.h"
#include "ui/UISystem.h"   // ゲーム UI のフォーカス検査（合成ポインタ / WantsNav）
#include "scene/SceneSerializer.h"
#include "physics/PhysicsSystem.h"
#include "renderer/Material.h"
#include "renderer/Mesh.h"
#include "terrain/SculptIO.h"
#include "terrain/SculptMesh.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_engine.h>
#include <imgui_te_context.h>
#include <imgui_te_ui.h>
#include <imgui_te_utils.h>
#include <imgui_te_exporters.h>
#include "gui/ImGuizmo.h"   // ImVec2/ImDrawList を使うので imgui.h より後に置くこと
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <vector>

// 配布ゲームを実際に起動して確かめるため（下の T_BuildGame）
#include <Windows.h>

namespace dx12e
{
namespace
{

// ===================== 対象ウィンドウ =====================
// テストから窓を指すときは必ず先頭に "//"（絶対参照）を付ける。
// 付けないと「直前の SetRef からの相対パス」として別 ID にハッシュされ、
// 窓は出ているのに「見つかりません」と誤判定してテストが黙ってスキップされる。
const char* kWinHierarchyName = "ヒエラルキー";   // ImGui::Begin に渡している生の名前（内部 API 用）

const char* kWinHierarchy   = "//ヒエラルキー";
const char* kWinInspector   = "//インスペクター";
const char* kWinConsole     = "//コンソール";
const char* kWinAssets      = "//アセットブラウザ";
const char* kWinToolbar     = "//##Toolbar";
const char* kWinVfx         = "//パーティクルエディタ###VfxEditorPanelFloating";
const char* kWinUiEditor    = "//UIエディタ###UiEditorPanelFloating";
const char* kWinMaterial    = "//マテリアルエディタ###MaterialEditorFloating";
const char* kWinMatLibrary  = "//マテリアルライブラリ (Poly Haven)###MaterialLibraryFloating";
const char* kWinBuild       = "//ビルド設定";
// 後から増えた独立フローティング窓（NoDocking なので右下のツール窓群には入らない）
const char* kWinLighting    = "//ライティング###LightingPanelFloating";
const char* kWinTerrain     = "//地形ツール###TerrainToolFloating";
const char* kWinSculpt      = "//スカルプト（異形）###SculptToolFloating";
const char* kWinNavMesh     = "//ナビメッシュ###NavMeshFloating";
const char* kWinAudioMixer  = "//オーディオミキサー###AudioMixerFloating";

// パーティクルエディタは中身を BeginChild で 3 分割している。子ウィンドウの中の項目は
// 親窓からの ID パスでは引けない（子窓の実 ID は "親名/str_id_XXXXXXXX" のハッシュ）ので、
// WindowInfo に子窓まで辿らせて ID を取ってから触る。
const char* kWinVfxAssetList = "//パーティクルエディタ###VfxEditorPanelFloating/##VfxAssetList";
const char* kWinVfxMain      = "//パーティクルエディタ###VfxEditorPanelFloating/##VfxMain";
const char* kWinVfxEmission  = "//パーティクルエディタ###VfxEditorPanelFloating/##VfxMain/##VfxEmission";

const char* kBtnAddEntity    = ICON_PLUS " エンティティ追加";      // EditorIcons.h（Lucide グリフ + ラベル）
const char* kBtnAddComponent = ICON_PLUS " コンポーネント追加";
const char* kMenuScript      = "スクリプト";

// ===================== 実行中の状態 =====================
// テスト関数はキャプチャ不可のラムダ（関数ポインタ）なので、共有状態はファイル静的に置く。
Application* g_app = nullptr;

// 各テストが実行中に吐いたエラーログ。クラッシュしなくても「静かに壊れている」を拾うため、
// テスト名 → 収集した行、で残して診断パネルに出す（失敗扱いにはしない＝誤検知で赤くしない）。
std::map<std::string, std::string> g_testNotes;
// --ui-tests-skip で除外するテスト名（UiTestHarness::SetSkipList）
std::set<std::string> g_skipTests;
// --ui-tests-only で「これだけ走らせる」テスト名（空 = 全部。UiTestHarness::SetOnlyList）
std::set<std::string> g_onlyTests;
uint64_t g_logCursor = 0;

EditorContext* Ed() { return g_app ? g_app->GetEditorContext() : nullptr; }

// テストの手順をクラッシュレポート(dx12_crash.log)とテストログの両方へ残す。
// 落ちたときに「どのテストのどの手順で死んだか」が一発で分かるのが目的。
void Step(ImGuiTestContext* ctx, const char* fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    CrashHandler::Breadcrumb(buf);
    if (ctx) ctx->LogInfo("%s", buf);
}

// ---- ログ監視（テスト中に出たエラーを集める）----
void LogWatchBegin()
{
    std::vector<LogEntry> drain;
    g_logCursor = Logger::ReadBuffered(g_logCursor, drain);
}

std::string LogWatchEnd()
{
    std::vector<LogEntry> fresh;
    g_logCursor = Logger::ReadBuffered(g_logCursor, fresh);

    std::string out;
    int count = 0;
    for (const LogEntry& e : fresh)
    {
        if (e.level < 4) continue;             // 4=err 5=critical のみ拾う
        if (++count > 8) { out += "  ...\n"; break; }
        out += "  [" + e.time + "] " + e.text + "\n";
    }
    return out;
}

// ===================== ウィンドウ / 項目の共通ヘルパ =====================

// 窓を前面（ドックタブなら選択状態）にして、中身が実際に描かれる状態にする。
// ドックされた窓はタブが選ばれていないと ImGui::Begin が false を返し、
// パネル側が即 return するので中の項目がひとつも存在しない＝何も検査できない。
// 戻り値は窓 ID（0 なら窓が無い）。
// ※ 窓の解決に ImGuiTestContext::WindowInfo(パス) は使わない。ImGuiTestRef のパスは '/' を
//   区切りとして解釈するため、"Skybox / IBL" や "MCP / AI Bridge" のように名前に '/' を含む窓が
//   永久に「見つからない」判定になる。ImGui に直接名前で引かせて ID で扱う。
ImGuiID FocusWindow(ImGuiTestContext* ctx, const char* windowRef)
{
    const char* name = (std::strncmp(windowRef, "//", 2) == 0) ? windowRef + 2 : windowRef;
    ImGuiWindow* window = ImGui::FindWindowByName(name);
    if (window == nullptr || !window->WasActive)
        return 0;   // 窓オブジェクトは残っていても、描かれていなければ「出ていない」
    ctx->WindowFocus(window->ID);
    ctx->Yield(3);
    return window->ID;
}

// 子ウィンドウ込みで窓 ID を引く（"//窓名/##子ID" 形式）。SetRef/GetID は子窓を辿らないので、
// 子ウィンドウの中の項目を触るときは必ずこれで ID を取ってから SetRef すること。
ImGuiID ChildWindowId(ImGuiTestContext* ctx, const char* pathRef)
{
    return ctx->WindowInfo(pathRef, ImGuiTestOpFlags_NoError).ID;
}

// 窓の中の項目を「ID パス」で指す。子ウィンドウがあれば名前を挟む（"##VfxMain/Luaコードをコピー"）。
//
// ★ ラベル走査（GatherItems + DebugLabel の文字列一致）は使わないこと。
//   ImGuiTestItemInfo::DebugLabel は 32 バイトで打ち切られるため日本語ラベルは途中で切れて当たらず、
//   さらに一部のウィジェット（Combo 等）はそもそも DebugLabel を登録しない。
//   その結果「項目が無い」と誤判定して、検査が黙って素通りする（＝緑なのに何も見ていない）。
bool ItemExistsIn(ImGuiTestContext* ctx, ImGuiID windowId, const char* path)
{
    if (windowId == 0) return false;
    ctx->SetRef(windowId);
    return ctx->ItemExists(path);
}

// 窓の中の項目をクリックする。見つからなければテスト失敗（=「押せない」は異常）。
bool ClickPath(ImGuiTestContext* ctx, ImGuiID windowId, const char* path)
{
    if (!ItemExistsIn(ctx, windowId, path))
    {
        IM_ERRORF("項目が見つかりません: %s", path);
        return false;
    }
    ctx->ItemClick(path);
    return true;
}

// 集めた項目のうち、最後に生成されたエンティティ行（=リスト末尾側）を返す。
// ✚ ボタン等の非エンティティ項目は飛ばす。見つからなければ nullptr。
const ImGuiTestItemInfo* SelectLastItem(ImGuiTestItemList& items)
{
    for (int i = items.GetSize() - 1; i >= 0; --i)
    {
        const ImGuiTestItemInfo* item = items[i];
        if (item == nullptr || item->ID == 0 || item->Window == nullptr) continue;
        if (std::strstr(item->DebugLabel, ICON_PLUS) != nullptr) continue;
        if (std::strstr(item->DebugLabel, "HierBg") != nullptr) continue;      // 空白の受け皿（行ではない）
        if (std::strncmp(item->DebugLabel, "##", 2) == 0) continue;            // 行の右端の目 / 鍵ボタン（ホバー中の行にだけ出る。行ではない）
        if (item->RectFull.GetHeight() > 60.0f) continue;                       // 行より明らかに大きい＝空白領域
        return item;
    }
    return nullptr;
}

// ヒエラルキーに並ぶエンティティ行を集めて、順にクリックする。
// 落ちる/固まるのはたいてい「選択が変わった次のフレームで Inspector が描く」瞬間なので、
// 1 件ごとに数フレーム回して Inspector 側の描画まで到達させる。
void ClickEveryHierarchyItem(ImGuiTestContext* ctx, int maxItems)
{
    // ★行は毎回集め直す。ヒエラルキーは ImGuiListClipper で【可視行しか作らない】ので、
    //   クリックで選択が変わって行がスクロール/展開されると、最初に拾った ID が
    //   その場から消えて "Unable to locate item" で落ちる（T_InspectorOpenAll は
    //   同じ理由で既に集め直す形に直っていたが、こちらだけ古いままだった）。
    int clicked = 0;
    for (int i = 0; i < maxItems; ++i)
    {
        ctx->SetRef(kWinHierarchy);
        ImGuiTestItemList items;
        ctx->GatherItems(&items, "", 3);
        if (i >= items.GetSize()) break;

        const ImGuiTestItemInfo* item = items[i];
        if (item == nullptr || item->ID == 0 || item->Window == nullptr) continue;
        if (std::strstr(item->DebugLabel, ICON_PLUS) != nullptr) continue;
        if (std::strstr(item->DebugLabel, "HierBg") != nullptr || item->RectFull.GetHeight() > 60.0f) continue;   // 空白の受け皿
        if (std::strncmp(item->DebugLabel, "##", 2) == 0) continue;   // 行の右端の目 / 鍵ボタン
        if (!ctx->ItemExists(item->ID)) continue;   // クリッパで消えた行は飛ばす

        ctx->MouseMove(item->ID);
        ctx->MouseClick(0);
        ctx->Yield(3);          // Inspector の再描画まで進める
        ++clicked;
    }
    ctx->LogInfo("clicked %d hierarchy items", clicked);
    IM_CHECK_NO_RET(clicked > 0);   // 1件も押せていない＝検査できていない
}

// 現在フォーカス中のポップアップから、ラベルに needle を含む項目をクリックする。
// ItemClick はラベル中の '/' をパス区切りとして解釈するため、"UI Layout (VBox/HBox/Grid)"
// のようなスラッシュ入りラベルは名前指定では引けない。ID 直指定で回避する。
bool ClickPopupItemContaining(ImGuiTestContext* ctx, const char* needle)
{
    ctx->SetRef("//$FOCUSED");
    ImGuiTestItemList items;
    ctx->GatherItems(&items, "", 2);
    for (int i = 0; i < items.GetSize(); ++i)
    {
        const ImGuiTestItemInfo* item = items[i];
        if (item == nullptr || item->ID == 0) continue;
        if (std::strstr(item->DebugLabel, needle) == nullptr) continue;
        ctx->MouseMove(item->ID);
        ctx->MouseClick(0);
        return true;
    }
    ctx->LogWarning("popup item not found: %s", needle);
    return false;
}

// ヒエラルキーの「✚ エンティティ追加」から種別を1つ足す
void AddEntity(ImGuiTestContext* ctx, const char* type)
{
    ctx->SetRef(kWinHierarchy);
    ctx->ItemClick(kBtnAddEntity);
    ctx->Yield();
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick(type);
    ctx->Yield(6);   // spawn はフレーム境界の遅延処理なので余分に回す
}

// 直近に足したエンティティを選択する
void SelectLastEntity(ImGuiTestContext* ctx)
{
    ctx->SetRef(kWinHierarchy);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, "", 3);
    if (const ImGuiTestItemInfo* last = SelectLastItem(items))
    {
        ctx->MouseMove(last->ID);
        ctx->MouseClick(0);
        ctx->Yield(4);
    }
}

// EditorContext のトグルで窓を開き、前面に出して ID を返す。開けなければテスト失敗。
// 右下のドックノードは「ツール窓が 0 個 → 1 個」で作り直されるため、出るまで数十フレーム待つ。
ImGuiID OpenToolWindow(ImGuiTestContext* ctx, bool EditorContext::* flag, const char* windowRef)
{
    EditorContext* ed = Ed();
    if (ed == nullptr) { IM_ERRORF("EditorContext を取得できません"); return 0; }

    ed->*flag = true;
    for (int i = 0; i < 8; ++i)
    {
        ctx->Yield(6);
        if (const ImGuiID id = FocusWindow(ctx, windowRef))
            return id;
    }
    IM_ERRORF("窓が開きません: %s", windowRef);
    return 0;
}

void CloseToolWindow(ImGuiTestContext* ctx, bool EditorContext::* flag)
{
    if (EditorContext* ed = Ed())
        ed->*flag = false;
    ctx->Yield(4);
}

// ===================== テスト本体 =====================
// ※ ここで絶対に触ってはいけないもの:
//    ファイル/フォルダ選択ダイアログを開く項目（ファイルメニューの「シーンを開く」「保存」、
//    ビルド設定の「参照...」、マテリアルの「画像から作成...」）。Win32 のモーダルは
//    ImGui のフレームを止めるので、自動テストがそこで永久に固まる。

// ---- 基本操作 ----

void T_HierarchyClickAll(ImGuiTestContext* ctx)
{
    Step(ctx, "ヒエラルキーの全項目をクリック");
    ClickEveryHierarchyItem(ctx, 40);
}

void T_SpawnAllEntityTypes(ImGuiTestContext* ctx)
{
    const char* types[] = { "Box", "Sphere", "Plane", "Empty", "Camera",
                            "Directional Light", "Point Light", "Spot Light" };
    for (const char* type : types)
    {
        Step(ctx, "エンティティ生成: %s", type);
        AddEntity(ctx, type);
        SelectLastEntity(ctx);
    }
}

void T_InspectorOpenAll(ImGuiTestContext* ctx)
{
    // 行は毎回集め直す。ヒエラルキーは ImGuiListClipper で可視行しか作らないので、
    // 選択やスクロールで前に拾った ID が消え、"Unable to locate item" で落ちていた。
    for (int i = 0; i < 12; ++i)
    {
        ctx->SetRef(kWinHierarchy);
        ImGuiTestItemList items;
        ctx->GatherItems(&items, "", 3);
        if (i >= items.GetSize()) break;
        const ImGuiTestItemInfo* item = items[i];
        if (item == nullptr || item->ID == 0) continue;
        if (!ctx->ItemExists(item->ID)) continue;
        Step(ctx, "インスペクターの全セクションを開く (%d件目)", i + 1);
        ctx->MouseMove(item->ID);
        ctx->MouseClick(0);
        ctx->Yield(3);
        ctx->SetRef(kWinInspector);
        ctx->ItemOpenAll("", 2);   // 折りたたみを全部開く＝全編集 UI を描かせる
        ctx->Yield(3);
        ctx->SetRef(kWinHierarchy);
    }
}

void T_UndoRedoStress(ImGuiTestContext* ctx)
{
    Step(ctx, "Undo/Redo 用にエンティティを生成");
    AddEntity(ctx, "Box");
    AddEntity(ctx, "Empty");
    ctx->SetRef(kWinHierarchy);
    for (int i = 0; i < 6; ++i)
    {
        Step(ctx, "Undo %d 回目", i + 1);
        ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
        ctx->Yield(3);
    }
    for (int i = 0; i < 6; ++i)
    {
        Step(ctx, "Redo %d 回目", i + 1);
        ctx->KeyPress(ImGuiKey_Y | ImGuiMod_Ctrl);
        ctx->Yield(3);
    }
}

void T_EditMenuOps(ImGuiTestContext* ctx)
{
    Step(ctx, "コピー/貼り付け/複製/削除 用にエンティティを生成");
    AddEntity(ctx, "Box");
    SelectLastEntity(ctx);

    // 編集メニュー経由（メニュー自体の描画・活性判定も同時に検査できる）
    const char* ops[] = { "編集/コピー", "編集/貼り付け", "編集/複製", "編集/削除" };
    for (const char* op : ops)
    {
        Step(ctx, "メニュー: %s", op);
        ctx->SetRef(kWinToolbar);
        ctx->MenuClick(op);
        ctx->Yield(6);   // 貼り付け/複製/削除はフレーム境界の遅延処理
        SelectLastEntity(ctx);
    }
}

void T_GizmoAndViewModes(ImGuiTestContext* ctx)
{
    AddEntity(ctx, "Box");
    SelectLastEntity(ctx);

    // ギズモ切替（W=移動 E=回転 R=拡縮）。選択中に毎フレーム ImGuizmo が描かれる状態で切り替える
    const ImGuiKey keys[] = { ImGuiKey_W, ImGuiKey_E, ImGuiKey_R, ImGuiKey_W };
    for (ImGuiKey k : keys)
    {
        Step(ctx, "ギズモモード切替");
        ctx->KeyPress(k);
        ctx->Yield(4);
    }

    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    Step(ctx, "2D ビューへ切替");
    ed->view2D = true;
    ctx->Yield(8);
    Step(ctx, "3D ビューへ戻す");
    ed->view2D = false;
    ctx->Yield(8);
}

// ---- ビューポートの帯（フェーズ 1a）----

// カメラの向きが軸に揃った（ビューキューブ / ブックマークの補間が終わった）とみなす。
static bool CameraAxisAligned(const EditorContext* ed)
{
    const float m = (std::max)({ std::fabs(ed->camFwd.x), std::fabs(ed->camFwd.y), std::fabs(ed->camFwd.z) });
    return m > 0.995f;
}

void T_ViewportBar(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    ed->view2D = false;
    ed->uiEditMode = false;
    ed->flyMode = false;
    ed->vpPrefs = EditorContext::ViewportPrefs{};   // 既定へ
    ed->ClearSelection();
    ctx->Yield(8);

    // 後続のテストにカメラの向きを持ち越さない: 開始時の視点をブックマーク 9 へ控え、終わりに戻す
    ed->pendingBookmarkSet = 9;
    ctx->Yield(4);
    const DirectX::XMFLOAT3 startPos = ed->camPos;

    ImGuiWindow* bar = ImGui::FindWindowByName("##ViewportBar");
    IM_CHECK(bar != nullptr && bar->WasActive);
    if (bar == nullptr) return;

    // 3D の矩形は帯の直下から始まる（帯は 3D の外＝ゲームの絵に重ならない）
    Step(ctx, "帯は 3D の矩形の外にある");
    IM_CHECK_NO_RET(ed->viewportY >= bar->Pos.y + bar->Size.y - 1.0f);
    IM_CHECK_NO_RET(ed->viewportW > 100.0f && ed->viewportH > 100.0f);
    auto ratioIs = [&](float want) { return std::fabs(ed->viewportW / ed->viewportH - want) < 0.02f; };
    IM_CHECK_NO_RET(ratioIs(16.0f / 9.0f));   // 既定は従来どおり 16:9

    // アスペクト: 帯のメニューから選ぶ（ポップオーバーの中の項目も実際に押す）
    Step(ctx, "アスペクトを 4:3 へ（帯 > アスペクト）");
    ctx->SetRef("//##ViewportBar");
    ctx->ItemClick("vpAspect/##ldb");
    ctx->Yield(3);
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick("4:3");
    ctx->Yield(6);
    IM_CHECK_NO_RET(ed->vpPrefs.aspectIndex == 3);
    IM_CHECK_NO_RET(ratioIs(4.0f / 3.0f));
    for (int i = 0; i < vp::kAspectCount; ++i)
    {
        ed->vpPrefs.aspectIndex = i;
        ctx->Yield(4);
        if (vp::kAspects[i].ratio > 0.0f)
            IM_CHECK_NO_RET(ratioIs(vp::kAspects[i].ratio));
        else
            IM_CHECK_NO_RET(ed->viewportW >= bar->Size.x * 0.5f);   // フィット = 領域いっぱい
        IM_CHECK_NO_RET(ed->viewportX >= bar->Pos.x - 1.0f && ed->viewportX + ed->viewportW <= bar->Pos.x + bar->Size.x + 1.0f);
    }
    ed->vpPrefs.aspectIndex = vp::kAspectDefault;
    ctx->Yield(4);
    IM_CHECK_NO_RET(ratioIs(16.0f / 9.0f));

    // 帯を隠せば従来と同じ矩形（＝帯の高さぶん 3D が広がる。決定論スクショの基準）
    Step(ctx, "帯を隠す / 戻す");
    const float yWith = ed->viewportY, hWith = ed->viewportH;
    ed->vpPrefs.barVisible = false;
    ctx->Yield(6);
    {
        ImGuiWindow* b2 = ImGui::FindWindowByName("##ViewportBar");
        IM_CHECK_NO_RET(b2 == nullptr || !b2->WasActive);
    }
    IM_CHECK_NO_RET(ed->viewportY <= yWith + 0.5f);
    IM_CHECK_NO_RET(ed->viewportH >= hWith - 0.5f);
    ed->vpPrefs.barVisible = true;
    ctx->Yield(6);
    IM_CHECK_NO_RET(std::fabs(ed->viewportH - hWith) < 1.0f);   // 元の矩形へ戻る（往復で縮まない）

    // スナップのポップオーバー（量のチップを押すと EditorContext に反映される）
    Step(ctx, "スナップ量を変える（帯 > スナップ）");
    const float savedSnap = ed->snapTranslate;
    ctx->SetRef("//##ViewportBar");
    ctx->ItemClick("vpSnap/##ldb");
    ctx->Yield(3);
    ctx->SetRef("//$FOCUSED");
    if (ctx->ItemExists("t5/##chip"))
    {
        ctx->ItemClick("t5/##chip");
        ctx->Yield(3);
        IM_CHECK_NO_RET(std::fabs(ed->snapTranslate - vp::kSnapTranslate[5]) < 1e-5f);
        IM_CHECK_NO_RET(!ed->vpHudText.empty());   // 変更は帯の一時表示に出る
    }
    else
        IM_ERRORF("スナップのチップが見つかりません");
    ed->snapTranslate = savedSnap;
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(4);

    // ビューキューブ: クリックでなめらかに軸へ揃う
    Step(ctx, "ビューキューブをクリック");
    ctx->SetRef("//##ViewportBar");
    bool jumped = false;
    if (const ImGuiTestItemInfo cubeInfo = ctx->ItemInfo("##viewCube", ImGuiTestOpFlags_NoError); cubeInfo.ID != 0)
    {
        const ImVec2 c = cubeInfo.RectFull.GetCenter();
        const float s = cubeInfo.RectFull.GetWidth();
        const ImVec2 offs[4] = { ImVec2(-0.20f, 0.06f), ImVec2(0.20f, 0.06f), ImVec2(0.0f, -0.22f), ImVec2(0.0f, 0.24f) };
        for (const ImVec2& o : offs)
        {
            const DirectX::XMFLOAT3 before = ed->camFwd;
            ctx->MouseMoveToPos(ImVec2(c.x + o.x * s, c.y + o.y * s));
            ctx->MouseClick(0);
            for (int f = 0; f < 600 && !CameraAxisAligned(ed); ++f) ctx->Yield();
            ctx->Yield(2);
            const float d = std::fabs(ed->camFwd.x - before.x) + std::fabs(ed->camFwd.y - before.y) + std::fabs(ed->camFwd.z - before.z);
            if (d > 0.05f || CameraAxisAligned(ed)) { jumped = true; break; }
        }
    }
    IM_CHECK_NO_RET(jumped);
    IM_CHECK_NO_RET(CameraAxisAligned(ed));

    // ブックマーク: Ctrl+2 で保存 → 別の面へ動かす → 2 で戻る
    Step(ctx, "カメラブックマークの保存と呼び出し");
    ctx->MouseMoveToPos(ImVec2(ed->viewportX + ed->viewportW * 0.5f, ed->viewportY + ed->viewportH * 0.5f));
    ctx->Yield(2);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_2);
    ctx->Yield(4);
    const DirectX::XMFLOAT3 savedPos = ed->camPos;
    const DirectX::XMFLOAT3 savedFwd = ed->camFwd;
    {
        ctx->SetRef("//##ViewportBar");
        if (const ImGuiTestItemInfo cubeInfo = ctx->ItemInfo("##viewCube", ImGuiTestOpFlags_NoError); cubeInfo.ID != 0)
        {
            const ImVec2 c = cubeInfo.RectFull.GetCenter();
            const float s = cubeInfo.RectFull.GetWidth();
            // 今と違う向きになるまで、別の場所を順にクリックする
            const ImVec2 offs[4] = { ImVec2(0.0f, -0.24f), ImVec2(0.22f, 0.0f), ImVec2(-0.22f, 0.0f), ImVec2(0.0f, 0.24f) };
            for (const ImVec2& o : offs)
            {
                ctx->MouseMoveToPos(ImVec2(c.x + o.x * s, c.y + o.y * s));
                ctx->MouseClick(0);
                for (int f = 0; f < 600; ++f) { ctx->Yield(); if (CameraAxisAligned(ed) && f > 30) break; }
                ctx->Yield(3);
                const float d = std::fabs(ed->camFwd.x - savedFwd.x) + std::fabs(ed->camFwd.y - savedFwd.y) + std::fabs(ed->camFwd.z - savedFwd.z);
                if (d > 0.3f) break;
            }
        }
    }
    ctx->MouseMoveToPos(ImVec2(ed->viewportX + ed->viewportW * 0.5f, ed->viewportY + ed->viewportH * 0.5f));
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_2);
    for (int f = 0; f < 800; ++f)
    {
        ctx->Yield();
        const float dp = std::fabs(ed->camPos.x - savedPos.x) + std::fabs(ed->camPos.y - savedPos.y) + std::fabs(ed->camPos.z - savedPos.z);
        if (dp < 0.05f && f > 4) break;
    }
    const float dp = std::fabs(ed->camPos.x - savedPos.x) + std::fabs(ed->camPos.y - savedPos.y) + std::fabs(ed->camPos.z - savedPos.z);
    IM_CHECK_NO_RET(dp < 0.1f);

    // 他のポップオーバー（ビューモード / 表示 / カメラ / ブックマーク）が開けて、閉じられる
    Step(ctx, "帯のポップオーバーを順に開く");
    const char* pops[] = { "vpViewMode/##ldb", "vpShow/##ldb", "vpCamera/##ldb", "vpBookmarks/##ldb" };
    for (const char* p : pops)
    {
        ctx->SetRef("//##ViewportBar");
        ctx->ItemClick(p);
        ctx->Yield(3);
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
    }
    ed->vpPrefs = EditorContext::ViewportPrefs{};
    ctx->Yield(4);

    // 開始時の視点へ戻す
    ed->pendingBookmarkJump = 9;
    for (int f = 0; f < 800; ++f)
    {
        ctx->Yield();
        const float d = std::fabs(ed->camPos.x - startPos.x) + std::fabs(ed->camPos.y - startPos.y) + std::fabs(ed->camPos.z - startPos.z);
        if (d < 0.02f && f > 4) break;
    }
    ctx->Yield(4);
}

void T_ViewportSelection(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    ed->view2D = false;
    ed->uiEditMode = false;
    ed->vpPrefs = EditorContext::ViewportPrefs{};

    // 選択アウトライン（エディタ専用の後処理）: 選ぶと専用パスが立ち上がり、マスクを描く
    Step(ctx, "Box を選択 → 輪郭パスが描く");
    AddEntity(ctx, "Box");
    SelectLastEntity(ctx);
    const entt::entity addedBox = ed->selectedEntity;
    ctx->Yield(30);
    IM_CHECK_NO_RET(ed->outlinePass != nullptr);
    if (ed->outlinePass)
    {
        IM_CHECK_NO_RET(ed->outlinePass->IsReady());
        IM_CHECK_NO_RET(ed->outlinePass->LastMaskDraws() >= 1);
    }

    // ホバー強調: ビューポートの上でマウスを動かしても破綻しない（CPU のレイ）
    Step(ctx, "ビューポート上でマウスを動かす（ホバー）");
    for (int i = 0; i < 6; ++i)
    {
        ctx->MouseMoveToPos(ImVec2(ed->viewportX + ed->viewportW * (0.3f + 0.1f * i), ed->viewportY + ed->viewportH * 0.55f));
        ctx->Yield(3);
    }

    // ビューモード（デプス / 法線 / ワイヤ …）を順に切り替えても描画が落ちない
    Step(ctx, "ビューモードを順に切り替える");
    for (int m = 0; m < vp::kViewModeCount; ++m)
    {
        ed->vpPrefs.viewMode = m;
        ctx->Yield(6);
    }
    ed->vpPrefs.viewMode = vp::kViewModeLit;
    ed->clusterDebugMode = 0;
    ctx->Yield(4);

    // 表示フラグを全部落として戻す
    Step(ctx, "表示フラグを切り替える");
    ed->vpPrefs.showGizmo = false; ed->vpPrefs.showIcons = false; ed->vpPrefs.showLightHandles = false;
    ed->vpPrefs.showViewCube = false; ed->vpPrefs.outline = false; ed->vpPrefs.showBounds = true;
    ctx->Yield(8);
    ed->vpPrefs = EditorContext::ViewportPrefs{};
    ctx->Yield(4);

    // 矩形選択: 空所から左ドラッグ → 離すと矩形に収まる物が選ばれる。空所を押せなかった（物に当たった）ときは検査しない。
    Step(ctx, "矩形選択");
    ed->ClearSelection();
    ctx->Yield(4);
    const ImVec2 a(ed->viewportX + 6.0f, ed->viewportY + 6.0f);
    const ImVec2 b(ed->viewportX + ed->viewportW - 6.0f, ed->viewportY + ed->viewportH - 6.0f);
    ctx->MouseMoveToPos(a);
    ctx->Yield(2);
    ctx->MouseDown(0);
    ctx->Yield(2);
    bool sawMarquee = false;
    for (int i = 1; i <= 4; ++i)
    {
        ctx->MouseMoveToPos(ImVec2(a.x + (b.x - a.x) * i / 4.0f, a.y + (b.y - a.y) * i / 4.0f));
        ctx->Yield(2);
        sawMarquee = sawMarquee || ed->marqueeActive;
    }
    ctx->MouseUp(0);
    ctx->Yield(4);
    ctx->LogInfo("marquee=%d selected=%d", sawMarquee ? 1 : 0, static_cast<int>(ed->selectedEntities.size()));
    if (sawMarquee)
        IM_CHECK_NO_RET(!ed->selectedEntities.empty());
    IM_CHECK_NO_RET(!ed->marqueeActive);   // 離したら矩形は終わる
    ed->ClearSelection();
    ctx->Yield(4);
    // 足した Box を片付ける（後続のテストへ持ち越さない）
    if (addedBox != entt::null)
        ed->pendingDeletions.push_back(addedBox);
    ctx->Yield(8);
}

// ---- コンポーネント ----

void T_AddAllComponents(ImGuiTestContext* ctx)
{
    AddEntity(ctx, "Empty");
    SelectLastEntity(ctx);

    const char* comps[] = {
        "Point Light", "Directional Light", "Spot Light", "Camera",
        "Audio Source", "Gimmick", "Particle Emitter", "Trail Renderer", "Trigger",
        "UI Canvas", "UI Rect", "UI Image", "UI Text", "UI Button",
        "UI Slider", "UI Toggle", "UI Scroll View", "UI Layout", "UI Animator",
        "RigidBody", "Box Collider", "Sphere Collider", "Capsule Collider",
        "Character Controller", "Network Identity", "Network Transform",
    };
    for (const char* comp : comps)
    {
        Step(ctx, "コンポーネント追加: %s", comp);
        ctx->SetRef(kWinInspector);
        ctx->ItemClick(kBtnAddComponent);
        ctx->Yield(2);
        // 検索付きポップアップ（フェーズ 1b）: 検索欄が自動フォーカスされるので、ラベルを打って Enter（先頭の候補が付く）。
        // ★一致した先頭がその項目であること自体が「ラベルで引ける」の確認になる。
        ctx->KeyChars(comp);
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(4);   // 追加直後の Inspector 再描画まで進める
    }

    // 全部載った状態でインスペクターを開き切る（レイアウト不整合はここで出る）
    Step(ctx, "全コンポーネントを載せたままインスペクターを開き切る");
    ctx->SetRef(kWinInspector);
    ctx->ItemOpenAll("", 2);
    ctx->Yield(6);
}

void T_AttachScript(ImGuiTestContext* ctx)
{
    Step(ctx, "Empty を生成して選択");
    AddEntity(ctx, "Empty");
    SelectLastEntity(ctx);

    Step(ctx, "スクリプトのサブメニューを開く（スクリプト走査 + プロパティ解析が走る）");
    ctx->SetRef(kWinInspector);
    ctx->ItemClick(kBtnAddComponent);
    ctx->Yield(2);
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick(kMenuScript);
    ctx->Yield(4);           // ScanScriptComponents + ParsePropertySchema が走る
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
}

// ---- パネル ----

// ツール窓を何度開け閉めしても、ビューポートとパネルの分割が一切変わらないこと。
//
// ★何を守っているか（2 つの不具合）
//   (1) ツール窓を開閉するたびにドックを壊して作り直していたため、ユーザーがドラッグで決めた幅が
//       既定へ戻った。
//   (2) 作り直しのたびに「実ノードの寸法から吸い上げた分割比」の丸め誤差が積もり、開閉を繰り返すほど
//       ビューポートが縮んだ（実機で 1145x645 → 320x180）。
//   今はツール窓が右カラムのインスペクターのタブとして入るだけで、ドックは作り直さない。
//   ここでは「ヒエラルキーの幅を広げてから、窓を 6 往復開閉しても、幅もビューポートも動かない」を確かめる。
void T_DockLayoutSurvivesToolToggle(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);

    const char* kHierarchy = "\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc";

    Step(ctx, "ツール窓を全部閉じた状態から始める（診断パネルは検査中なので触らない）");
    const bool wasDiag = ed->showEngineDiagnostics;
    tools::CloseAll(*ed);
    ed->showEngineDiagnostics = wasDiag;
    ctx->Yield(6);

    ImGuiWindow* hier = ImGui::FindWindowByName(kHierarchy);
    IM_CHECK(hier != nullptr);
    IM_CHECK(hier->DockNode != nullptr);

    Step(ctx, "ヒエラルキーの幅を既定から動かす（ユーザーのドラッグ相当）");
    const f32 before = hier->DockNode->Size.x;
    ImGui::DockBuilderSetNodeSize(hier->DockNode->ID, ImVec2(before * 1.6f, hier->DockNode->Size.y));
    ImGui::DockBuilderFinish(hier->DockNode->ID);
    ctx->Yield(4);

    hier = ImGui::FindWindowByName(kHierarchy);
    IM_CHECK(hier != nullptr && hier->DockNode != nullptr);
    const f32 widened = hier->DockNode->Size.x;
    IM_CHECK_GT(widened, before * 1.2f);       // 実際に広がったことを確かめてから本題へ

    const f32 vpW0 = ed->viewportW, vpH0 = ed->viewportH;
    IM_CHECK_GT(vpW0, 100.0f);
    IM_CHECK_GT(vpH0, 50.0f);

    // 右タブの窓（ドックに入る）と浮かぶ窓（NoDocking）を混ぜて 6 往復
    for (int cycle = 0; cycle < 6; ++cycle)
    {
        Step(ctx, "ツール窓を開閉 %d/6", cycle + 1);
        ed->showPostProcess = true;
        ed->showSkybox = true;
        ed->showEngineSettings = true;
        ed->showVersionControl = true;
        ed->showLighting = true;
        ctx->Yield(8);

        const f32 vpWNow = ed->viewportW, vpHNow = ed->viewportH;
        if (std::fabs(vpWNow - vpW0) > 1.0f || std::fabs(vpHNow - vpH0) > 1.0f)
        { IM_ERRORF("ツール窓を開いたらビューポートが変わった: %.1fx%.1f → %.1fx%.1f", vpW0, vpH0, vpWNow, vpHNow); break; }

        ed->showPostProcess = false;
        ed->showSkybox = false;
        ed->showEngineSettings = false;
        ed->showVersionControl = false;
        ed->showLighting = false;
        ctx->Yield(8);
    }

    hier = ImGui::FindWindowByName(kHierarchy);
    IM_CHECK(hier != nullptr && hier->DockNode != nullptr);
    const f32 after = hier->DockNode->Size.x;
    ctx->LogInfo("hierarchy width: 既定 %.1f → 広げた %.1f → 開閉後 %.1f / viewport %.1fx%.1f → %.1fx%.1f",
                 before, widened, after, vpW0, vpH0, ed->viewportW, ed->viewportH);
    IM_CHECK_LT(std::fabs(after - widened), 1.5f);              // 幅が 1px 以上動いていない
    IM_CHECK_LT(std::fabs(ed->viewportW - vpW0), 1.0f);         // ビューポートが縮んでいない
    IM_CHECK_LT(std::fabs(ed->viewportH - vpH0), 1.0f);
}

// Inspector の一括編集: 複数選択していると、プライマリの変更が同じ選択の全員へ届くこと。
//
// ★何を守っているか
//   Inspector は ctx.selectedEntity（プライマリ）しか見ておらず、複数選択しても
//   1 体ぶんしか編集できなかった。ライトを 40 灯選んで明るさを揃える、といった
//   レベル調整のたびに 40 回選び直して 40 回いじる必要があった。
//   直す側は EndEdit<T> で「プライマリのスナップショットと現在値のバイト差分＝触った
//   フィールド」だけを他の選択へ写す。**変えていないフィールド（色など）は相手の値のまま**で
//   なければならないので、そこも一緒に見る。Undo も 1 エントリであること。
// ゲーム UI のフォーカスが解放されること（HUD を 1 回押すとパッドが死ぬ問題）。
//
// ★何を守っているか
//   UISystem::m_focused はマウスクリックで設定されるのに、解放する経路が
//   ResetRuntimeState（Play/Stop）しか無かった。m_wantsNav は
//     !focusables.empty() && (m_focused != null || dir>=0 || confirm)
//   なので、一度どこかのウィジェットを押すと **永久に true** になる。
//   これは Lua の input:isUiCapturingNav() にそのまま出るので、ドキュメントが勧める
//     if input:isUiCapturingNav() then return end
//   を書いたゲームは HUD のボタンを 1 回押しただけで以後ずっと入力を捨てる。
//   ゲーム UI の入力は Play 中しか走らないので、このテストは Play へ入って
//   合成ポインタ（UISystem::InjectPointerClick）で押す。
void T_UiFocusReleases(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Scene* scene = g_app->GetScene();
    UISystem* ui = g_app->GetUiSystem();
    EditorContext* ed = Ed();
    IM_CHECK(scene != nullptr && ui != nullptr && ed != nullptr);

    Step(ctx, "キャンバスとボタンを作る");
    // ヒエラルキーのメニュー階層を辿るより、パネルと同じマーカーを直接積む方が確実。
    // （HierarchyPanel の「✚ エンティティ追加 > UI」が積んでいるのと同じもの）
    auto spawnUi = [&](const char* marker) {
        PendingSpawnRequest req;
        req.modelPath = marker;
        req.position  = {0.0f, 0.0f, 0.0f};
        ed->pendingSpawns.push_back(req);
        ctx->Yield(8);
    };
    auto& reg = scene->GetRegistry();
    spawnUi("__ui_canvas__");
    spawnUi("__ui_button__");
    entt::entity button = entt::null;
    for (auto e : reg.view<UIButton>()) button = e;
    IM_CHECK(button != entt::null);

    Step(ctx, "Play へ入る（ゲーム UI の入力は Play 中しか走らない）");
    g_app->RequestMode(Application::EngineMode::Playing);
    int frames = 0;
    while (g_app->GetEngineMode() != Application::EngineMode::Playing && frames < 240)
    { ctx->Yield(); ++frames; }
    IM_CHECK(g_app->GetEngineMode() == Application::EngineMode::Playing);
    ctx->Yield(6);

    // ボタンの中心（ビューポートローカル px）を出す。
    // ★解像度を決め打ちにすると ConstantPixel のキャンバスで位置がずれて押せない
    //   （実際に踏んだ: 1280x720 決め打ちでクリックが空振りした）。
    //   実際に描いているビューポートで解決すること。
    const ImVec2 vp = ui->LastViewportSize();
    IM_CHECK(vp.x > 1.0f && vp.y > 1.0f);
    std::vector<UiResolvedRect> rects;
    UISystem::ResolveRects(reg, 0.0f, 0.0f, vp.x, vp.y, rects);
    const UiResolvedRect* rr = nullptr;
    for (const auto& r : rects) if (r.e == button) { rr = &r; break; }
    IM_CHECK(rr != nullptr);

    Step(ctx, "ボタンを押す → UI が方向入力を掴む");
    ui->InjectPointerClick((rr->min.x + rr->max.x) * 0.5f, (rr->min.y + rr->max.y) * 0.5f);
    ctx->Yield(6);
    if (!ui->WantsNav())
    { IM_ERRORF("ボタンを押したのに WantsNav() が false（フォーカスが乗っていない）"); }

    Step(ctx, "何も無い所を押す → 掴みを手放すこと");
    ui->InjectPointerClick(8.0f, 8.0f);   // 左上隅。ウィジェットは置いていない
    ctx->Yield(6);
    if (ui->WantsNav())
    { IM_ERRORF("何も無い所を押したのに WantsNav() が true のまま"
                "（フォーカスが解放されない＝ゲーム側の入力が死ぬ）"); }

    Step(ctx, "Stop して後片付け");
    g_app->RequestMode(Application::EngineMode::Editor);
    frames = 0;
    while (g_app->GetEngineMode() != Application::EngineMode::Editor && frames < 240)
    { ctx->Yield(); ++frames; }
    ctx->Yield(6);
    // Stop はシーンを作り直すので id が変わる。名前で拾い直して消す。
    for (auto [e, tag] : scene->GetRegistry().view<NameTag>().each())
        if (tag.name.rfind("UI Canvas", 0) == 0 || tag.name.rfind("Canvas", 0) == 0)
            ed->pendingDeletions.push_back(e);
    ctx->Yield(8);
}

void T_InspectorMultiEdit(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);
    auto& reg = scene->GetRegistry();

    Step(ctx, "ポイントライトを 3 灯作る");
    entt::entity lights[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        AddEntity(ctx, "Point Light");
        SelectLastEntity(ctx);
        lights[i] = ed->selectedEntity;
        IM_CHECK(reg.valid(lights[i]) && reg.all_of<PointLight>(lights[i]));
    }

    // 「色は各自バラバラ / range は同じ」から始める。
    // 触っていない色まで揃ってしまう実装だと、この差が消えて検出できる。
    for (int i = 0; i < 3; ++i)
    {
        auto& pl = reg.get<PointLight>(lights[i]);
        pl.range = 10.0f;
        pl.color = { 0.2f * (i + 1), 0.5f, 1.0f - 0.2f * i };
    }
    const DirectX::XMFLOAT3 keepColor1 = reg.get<PointLight>(lights[1]).color;
    ctx->Yield(3);

    Step(ctx, "3 灯まとめて選択する（プライマリは lights[0]）");
    ed->selectedEntities.assign(std::begin(lights), std::end(lights));
    ed->selectedEntity = lights[0];
    ctx->Yield(4);

    Step(ctx, "Inspector の『距離 Range』を 1 回だけ動かす");
    ctx->SetRef(kWinInspector);
    // pg:: のウィジェットは PushID(ラベル) + "##v" という ID になる（PropertyGrid.h 参照）
    ctx->ItemInputValue("**/\xe8\xb7\x9d\xe9\x9b\xa2 Range/##v", 42.0f);   // 距離 Range
    ctx->Yield(6);

    for (int i = 0; i < 3; ++i)
    {
        const f32 r = reg.get<PointLight>(lights[i]).range;
        if (std::fabs(r - 42.0f) > 0.05f)
        { IM_ERRORF("ライト%d の range が届いていない（%.2f、期待 42.0）", i, r); }
    }

    // 触っていない色は相手の値のままでなければならない（全体コピーになっていないことの確認）。
    const DirectX::XMFLOAT3 c1 = reg.get<PointLight>(lights[1]).color;
    if (std::fabs(c1.x - keepColor1.x) > 1e-4f || std::fabs(c1.z - keepColor1.z) > 1e-4f)
    { IM_ERRORF("触っていない色まで上書きされている（%.2f,%.2f,%.2f）", c1.x, c1.y, c1.z); }

    Step(ctx, "Undo 1 回で 3 灯とも戻ること");
    ed->pendingUndo = true;
    ctx->Yield(8);
    for (int i = 0; i < 3; ++i)
    {
        const f32 r = reg.get<PointLight>(lights[i]).range;
        if (std::fabs(r - 10.0f) > 0.05f)
        { IM_ERRORF("Undo 1 回でライト%d が戻っていない（%.2f、期待 10.0）", i, r); }
    }

    // 後片付け
    for (auto e : lights) ed->pendingDeletions.push_back(e);
    ed->selectedEntities.clear();
    ed->selectedEntity = entt::null;
    ctx->Yield(6);
}

void T_OpenAllToolWindows(ImGuiTestContext* ctx)
{
    IM_CHECK(Ed() != nullptr);

    // 「フラグ → 窓」。ツールメニュー経由ではなく直接立てる（メニューにはダイアログ項目が混じるため）
    // optional=true: 環境によって存在しない窓（MCP ブリッジ未起動など）。出なくても失敗にしない。
    // textOnlyOk=true: 中身が案内テキストだけになる状態が正常な窓。GatherItems は ID 付き
    //   アイテムしか拾わない（TextDisabled は ID 無し）ので、空判定から除外する。
    struct Entry { bool EditorContext::* flag; std::string window; const char* label; bool optional; bool textOnlyOk; };
    // ★窓の一覧は editor/ToolWindows.h のレジストリから作る（窓を足しても、ここの追加漏れが起きない）。
    //   ImGui 上の窓名を持つもの（右タブの窓とトランジション）が対象。浮かぶ窓は T_NewFloatingPanels が見る。
    //   optional: 環境によって存在しない窓（MCP ブリッジ未起動）。textOnlyOk: 案内文だけが正常な窓
    //   （マスター OFF / 有効エフェクト 0 件のポストパラメータ。Application.cpp の showPostParams 参照）。
    // ※ 診断パネル自身はここに入れない（検査中に開閉・フォーカス移動しない）。
    std::vector<Entry> kEntries;
    for (const tools::Desc& d : tools::kAll)
    {
        if (!d.imguiName || !d.imguiName[0]) continue;
        kEntries.push_back({ d.flag, std::string("//") + d.imguiName, d.title,
                             std::strcmp(d.id, "mcp") == 0, std::strcmp(d.id, "postParams") == 0 });
    }

    // まとめて開く。1つずつ開閉すると「ツール窓が 0 個」になった瞬間に右下ドックノードが
    // 畳まれ、次の窓が数フレーム 0 サイズ扱い＝「出ていない」と誤判定されるため。
    // 元の開閉状態は最後に戻す（診断パネル自身を閉じてしまわないように）。
    Step(ctx, "ツール窓をすべて開く");
    std::vector<char> wasOpen(kEntries.size(), 0);
    for (size_t i = 0; i < kEntries.size(); ++i)
    {
        wasOpen[i] = Ed()->*(kEntries[i].flag);
        Ed()->*(kEntries[i].flag) = true;
    }
    ctx->Yield(30);

    // 失敗はここでは投げずに集める（1件エラーにするとテストが中断して残りを検査できない）
    std::string bad;
    for (const Entry& e : kEntries)
    {
        Step(ctx, "ツール窓を検査: %s", e.label);
        const ImGuiID id = FocusWindow(ctx, e.window.c_str());
        if (id == 0)
        {
            if (e.optional)
                ctx->LogWarning("窓が出ません(この環境では正常・スキップ): %s", e.label);
            else
                bad += std::string("\n  ・") + e.label + " : 窓が出ない";
            continue;
        }

        // 中身がひとつも描かれていない＝窓は出たが空、も異常として拾う
        if (!e.textOnlyOk)
        {
            ctx->SetRef(id);
            ImGuiTestItemList items;
            ctx->GatherItems(&items, "", 4);
            if (items.GetSize() == 0)
                bad += std::string("\n  ・") + e.label + " : 中身が空";
        }
        ctx->Yield(3);
    }

    for (size_t i = 0; i < kEntries.size(); ++i)
        Ed()->*(kEntries[i].flag) = wasOpen[i] != 0;
    ctx->Yield(6);

    if (!bad.empty())
        IM_ERRORF("開けなかったツール窓があります:%s", bad.c_str());
}

void T_ConsolePanel(ImGuiTestContext* ctx)
{
    const ImGuiID win = FocusWindow(ctx, kWinConsole);
    if (win == 0) { IM_ERRORF("コンソールが見つかりません"); return; }

    Step(ctx, "コンソールのトグルを操作");
    for (const char* cb : { "折りたたみ", "自動スクロール", "エラーで前面", "Play時クリア" })
    {
        ClickPath(ctx, win, cb);
        ctx->Yield(2);
        ClickPath(ctx, win, cb);   // 元に戻す
        ctx->Yield(2);
    }

    Step(ctx, "コンソールの検索フィルタに入力");
    ctx->SetRef(win);
    ctx->ItemInputValue("##consolefilter", "diag");
    ctx->Yield(4);
    ctx->ItemInputValue("##consolefilter", "");
    ctx->Yield(4);

    Step(ctx, "コンソールから Lua を実行");
    ctx->SetRef(win);
    ctx->ItemInputValue("##luainput", "log('エンジン診断: コンソール実行テスト')");
    ctx->Yield(8);

    Step(ctx, "コンソールをクリア");
    ClickPath(ctx, win, "クリア");
    ctx->Yield(4);
}

void T_AssetBrowser(ImGuiTestContext* ctx)
{
    const ImGuiID win = FocusWindow(ctx, kWinAssets);
    if (win == 0) { IM_ERRORF("アセットブラウザが見つかりません"); return; }

    // フォルダツリーを順にクリックする。フォルダ走査とサムネイル生成が走る経路。
    // ※ ItemOpenAll は使わない（子フォルダの無いノードは Leaf なので「開けない」判定で失敗する）
    ctx->SetRef(win);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, "", 4);
    IM_CHECK_NO_RET(items.GetSize() > 0);

    int clicked = 0;
    for (int i = 0; i < items.GetSize() && clicked < 12; ++i)
    {
        const ImGuiTestItemInfo* item = items[i];
        if (item == nullptr || item->ID == 0) continue;
        if (item->DebugLabel[0] == '\0') continue;   // 名前の無い項目（子窓の枠そのもの）は押せない
        if (ctx->ItemInfo(item->ID, ImGuiTestOpFlags_NoError).ID == 0) continue;   // 直前のクリックで消えた項目（フォルダ移動でツリーが変わる等）
        Step(ctx, "アセットブラウザの項目をクリック: %s", item->DebugLabel);
        ctx->ItemClick(item->ID);
        ctx->Yield(6);   // 一覧の再構築 + サムネイル生成まで進める
        ++clicked;
    }
    ctx->LogInfo("clicked %d asset browser items", clicked);
    IM_CHECK_NO_RET(clicked > 0);
    ctx->Yield(6);
}

// ===================== [A] アセットブラウザ =====================

// 1 万ファイルのフォルダ（scratchpad の使い捨てプロジェクト <assets>/perf/big。tools は scratchpad\e1b\A\gen10k.py）で
// ブラウザ Render() の CPU 時間を測る。フォルダが無いプロジェクトでは何もせず通る（通常の全件実行を汚さない）。
void T_AssetPerf10k(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr && ed->assetBrowser != nullptr);
    namespace fs = std::filesystem;
    const fs::path big = fs::path(PathResolver::AssetsDir()) / "perf" / "big";
    std::error_code ec;
    if (!fs::is_directory(big, ec)) { ctx->LogInfo("perf/big が無いのでスキップ（性能計測は 1 万ファイルの使い捨てプロジェクトで）"); return; }

    Step(ctx, "1 万ファイルのフォルダへ移動");
    ed->assetBrowser->NavigateTo(big);
    // 走査(ワーカー)の完了を待つ
    for (int i = 0; i < 600 && ed->assetBrowser->EntryCount() < 100; ++i) ctx->Yield(1);
    ctx->LogInfo("entries = %zu", ed->assetBrowser->EntryCount());

    Step(ctx, "60 フレームの Render() CPU ms を計測");
    std::vector<float> ms;
    for (int i = 0; i < 60; ++i) { ctx->Yield(1); ms.push_back(ed->assetBrowser->LastRenderMs()); }
    std::vector<float> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    float sum = 0.0f; for (float v : ms) sum += v;
    ctx->LogInfo("asset browser Render(): avg %.3f ms / median %.3f ms / p95 %.3f ms / max %.3f ms (60 frames, entries=%zu)",
                 sum / static_cast<float>(ms.size()), sorted[sorted.size() / 2], sorted[static_cast<size_t>(sorted.size() * 0.95)],
                 sorted.back(), ed->assetBrowser->EntryCount());

    Step(ctx, "元のフォルダへ戻す");
    ed->assetBrowser->NavigateTo(fs::path(PathResolver::AssetsDir()));
    ctx->Yield(10);
}

// 一覧が期待の件数になるまで待つ（走査はワーカースレッド。最大 600 フレーム）。
static bool AbWaitEntries(ImGuiTestContext* ctx, AssetBrowserPanel* ab, size_t expect)
{
    for (int i = 0; i < 600; ++i)
    {
        ctx->Yield(1);
        if (!ab->IsScanning() && ab->EntryCount() == expect) { ctx->Yield(2); return true; }
    }
    ctx->LogWarning("一覧が %zu 件になりません（現在 %zu 件）", expect, ab->EntryCount());
    return false;
}

// フォルダ作成 → 名前変更 → 複製 → 移動 → 削除（使い捨てフォルダ <assets>/__ab_ops_test の中だけ）/ 種別フィルタ / 検索 / グリッド⇔リスト /
// フォルダの移動（パンくず）/ 参照のあるファイルの名前変更で警告が出る。実アセットは触らない。
void T_AssetBrowserOps(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr && ed->assetBrowser != nullptr);
    AssetBrowserPanel* ab = ed->assetBrowser;
    namespace fs = std::filesystem;
    const fs::path assets = fs::path(PathResolver::AssetsDir()).lexically_normal();
    const fs::path work = assets / "__ab_ops_test";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work);
    auto put = [&](const char* name, const char* body) { std::ofstream(work / name, std::ios::binary) << body; };
    put("wall.png", "not a real png");
    put("b.lua", "-- b");
    put("c.lua", "-- c");
    put("d.dxmat", "{\"version\":1,\"name\":\"d\",\"metallic\":0.0,\"roughness\":0.5,\"uvTiling\":[1,1]}");

    Step(ctx, "使い捨てフォルダへ移動して 4 件が並ぶ");
    ab->SetListView(false);
    ab->NavigateTo(work);
    IM_CHECK(AbWaitEntries(ctx, ab, 4));
    IM_CHECK(ab->CurrentDir() == work);
    const size_t up = 1;   // 「..」の分（ルート以外のフォルダでは先頭に出る）
    IM_CHECK_EQ(ab->VisibleCount(), static_cast<size_t>(4) + up);

    Step(ctx, "種別フィルタ（スクリプト / テクスチャ / マテリアル / 解除）");
    ab->SetKindMask(abl::KindBit(abl::Kind::Script));
    ctx->Yield(3);
    IM_CHECK_EQ(ab->VisibleCount(), static_cast<size_t>(2) + up);
    ab->SetKindMask(abl::KindBit(abl::Kind::Texture));
    ctx->Yield(3);
    IM_CHECK_EQ(ab->VisibleCount(), static_cast<size_t>(1) + up);
    ab->SetKindMask(abl::ChipMask(abl::Kind::Material));
    ctx->Yield(3);
    IM_CHECK_EQ(ab->VisibleCount(), static_cast<size_t>(1) + up);
    ab->SetKindMask(0);
    ctx->Yield(3);
    IM_CHECK_EQ(ab->VisibleCount(), static_cast<size_t>(4) + up);

    Step(ctx, "検索（.lua）");
    ab->SetSearch("lua");
    IM_CHECK(AbWaitEntries(ctx, ab, 2));
    IM_CHECK_EQ(ab->VisibleCount(), static_cast<size_t>(2));   // 検索中は「..」が出ない
    ab->SetSearch("");
    IM_CHECK(AbWaitEntries(ctx, ab, 4));

    Step(ctx, "グリッド ⇔ リスト（帯のボタンを実際に押す）");
    const ImGuiID win = FocusWindow(ctx, kWinAssets);
    IM_CHECK(win != 0);
    ctx->SetRef(win);
    ctx->ItemClick("##ViewList/##ib");   // ui::IconButton は PushID(id) の中に "##ib" の項目を持つ
    ctx->Yield(3);
    IM_CHECK(ab->ListView());
    IM_CHECK(ab->DrawnCellsLastFrame() > 0);
    ctx->ItemClick("##ViewGrid/##ib");
    ctx->Yield(3);
    IM_CHECK(!ab->ListView());
    IM_CHECK(ab->DrawnCellsLastFrame() > 0);

    Step(ctx, "新しいフォルダを作る → 名前変更 → 複製 → 移動 → 削除");
    fs::path made;
    IM_CHECK(ab->TestCreateFolder(&made));
    IM_CHECK(fs::is_directory(made));
    IM_CHECK(AbWaitEntries(ctx, ab, 5));
    IM_CHECK(ab->TestRename(made, "renamed"));
    IM_CHECK(fs::is_directory(work / "renamed") && !fs::exists(made));
    IM_CHECK(AbWaitEntries(ctx, ab, 5));
    fs::path copy;
    IM_CHECK(ab->TestDuplicate(work / "renamed", &copy));
    IM_CHECK(copy.filename() == "renamed_copy" && fs::is_directory(copy));
    IM_CHECK(AbWaitEntries(ctx, ab, 6));
    IM_CHECK(ab->TestMove(copy, work / "renamed"));
    IM_CHECK(fs::is_directory(work / "renamed" / "renamed_copy") && !fs::exists(copy));
    IM_CHECK(AbWaitEntries(ctx, ab, 5));
    IM_CHECK(!ab->TestMove(work / "renamed", work / "renamed" / "renamed_copy"));   // 自分の子孫へは動かせない
    IM_CHECK(!ab->TestMove(work / "b.lua", work / "renamed" / "nope"));            // 存在しない移動先
    IM_CHECK(ab->TestDelete(work / "renamed"));
    IM_CHECK(AbWaitEntries(ctx, ab, 4));

    Step(ctx, "同名があるときの名前変更は失敗する / 使えない名前は弾く");
    IM_CHECK(!ab->TestRename(work / "b.lua", "c.lua"));
    IM_CHECK(!ab->TestRename(work / "b.lua", "a/b.lua"));
    IM_CHECK(!ab->TestRename(work / "b.lua", "bad?.lua"));
    IM_CHECK(fs::exists(work / "b.lua"));
    IM_CHECK(ab->TestRename(work / "b.lua", "b2.lua"));
    IM_CHECK(AbWaitEntries(ctx, ab, 4));
    IM_CHECK(fs::exists(work / "b2.lua") && !fs::exists(work / "b.lua"));

    Step(ctx, "選択（複数選択の API）");
    ab->SelectOnly(work / "c.lua");
    IM_CHECK_EQ(ab->SelectionCount(), static_cast<size_t>(1));
    IM_CHECK(ab->IsSelected(work / "c.lua"));

    Step(ctx, "パンくずの親へ戻る");
    ab->NavigateTo(assets);
    ctx->Yield(4);
    IM_CHECK(ab->CurrentDir() == assets);

    ab->SetKindMask(0);
    ab->SetListView(false);
    fs::remove_all(work, ec);
    ctx->Yield(5);
}

// エクスプローラーからのファイルドロップ（WM_DROPFILES）→ 現在のフォルダへコピー。拡張子ホワイトリスト / 名前衝突の連番 / フォルダごとのドロップ。
// 実際に OS のメッセージ（HDROP）を自分の窓へ PostMessage する（SendInput ではないのでフォーカスも奪わない）。
struct AbFindHwnd { HWND best = nullptr; long area = 0; };
static BOOL CALLBACK AbEnumWindows(HWND h, LPARAM lp)
{
    auto* f = reinterpret_cast<AbFindHwnd*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid != ::GetCurrentProcessId() || ::GetParent(h) != nullptr) return TRUE;
    RECT rc{};
    ::GetClientRect(h, &rc);
    const long a = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (a > f->area) { f->area = a; f->best = h; }
    return TRUE;
}

static bool AbPostDrop(HWND hwnd, const std::vector<std::wstring>& paths, int x, int y)
{
    size_t chars = 1;
    for (const auto& p : paths) chars += p.size() + 1;
    const size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL h = ::GlobalAlloc(GHND, bytes);
    if (!h) return false;
    auto* df = static_cast<DROPFILES*>(::GlobalLock(h));
    df->pFiles = sizeof(DROPFILES);
    df->pt.x = x; df->pt.y = y;
    df->fNC = FALSE;
    df->fWide = TRUE;
    wchar_t* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths) { std::wmemcpy(dst, p.c_str(), p.size() + 1); dst += p.size() + 1; }
    *dst = 0;
    ::GlobalUnlock(h);
    if (!::PostMessageW(hwnd, WM_DROPFILES, reinterpret_cast<WPARAM>(h), 0)) { ::GlobalFree(h); return false; }
    return true;
}

void T_AssetOsDrop(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr && ed->assetBrowser != nullptr);
    AssetBrowserPanel* ab = ed->assetBrowser;
    namespace fs = std::filesystem;
    const fs::path assets = fs::path(PathResolver::AssetsDir()).lexically_normal();
    const fs::path dest = assets / "__ab_drop_test";
    const fs::path src = fs::temp_directory_path() / "dx12e_ab_drop_src";
    std::error_code ec;
    fs::remove_all(dest, ec);
    fs::remove_all(src, ec);
    fs::create_directories(dest);
    fs::create_directories(src / "pack" / "inner");
    auto put = [&](const fs::path& p, const char* body) { std::ofstream(p, std::ios::binary) << body; };
    put(src / "ok.png", "png-bytes");
    put(src / "tex2.jpg", "jpg-bytes");
    put(src / "virus.exe", "MZ");
    put(src / "pack" / "a.lua", "-- a");
    put(src / "pack" / "inner" / "b.hlsl", "// b");
    put(src / "pack" / "notes.zip", "zip");

    AbFindHwnd f;
    ::EnumWindows(AbEnumWindows, reinterpret_cast<LPARAM>(&f));
    IM_CHECK(f.best != nullptr);

    Step(ctx, "使い捨てフォルダを開いて、ファイルとフォルダをドロップする");
    ab->NavigateTo(dest);
    IM_CHECK(AbWaitEntries(ctx, ab, 0));
    IM_CHECK(AbPostDrop(f.best, {(src / "ok.png").wstring(), (src / "tex2.jpg").wstring(), (src / "virus.exe").wstring(), (src / "pack").wstring()}, 5, 5));
    for (int i = 0; i < 600 && !fs::exists(dest / "pack" / "inner" / "b.hlsl"); ++i) ctx->Yield(1);
    ctx->Yield(10);
    IM_CHECK(fs::exists(dest / "ok.png"));
    IM_CHECK(fs::exists(dest / "tex2.jpg"));
    IM_CHECK(!fs::exists(dest / "virus.exe"));                       // 拡張子ホワイトリスト外は取り込まない
    IM_CHECK(fs::exists(dest / "pack" / "a.lua"));                   // フォルダごとのドロップは階層を保つ
    IM_CHECK(fs::exists(dest / "pack" / "inner" / "b.hlsl"));
    IM_CHECK(!fs::exists(dest / "pack" / "notes.zip"));
    IM_CHECK(ed->pendingOsDrops.empty());

    Step(ctx, "同じファイルをもう一度ドロップ → 連番で共存する（上書きしない）");
    IM_CHECK(AbPostDrop(f.best, {(src / "ok.png").wstring()}, 5, 5));
    for (int i = 0; i < 600 && !fs::exists(dest / "ok_1.png"); ++i) ctx->Yield(1);
    IM_CHECK(fs::exists(dest / "ok.png") && fs::exists(dest / "ok_1.png"));

    Step(ctx, "一覧に反映される（変更監視）");
    IM_CHECK(AbWaitEntries(ctx, ab, 4));   // ok.png / ok_1.png / tex2.jpg / pack

    ab->NavigateTo(assets);
    fs::remove_all(dest, ec);
    fs::remove_all(src, ec);
    ctx->Yield(5);
}


// 後から増えたフローティング窓（ライティング / 地形ツール）が、開いて中身が描かれ、
// 極端なサイズでも落ちず、ちゃんと閉じられるか。
// T_OpenAllToolWindows は右下ドックに入るツール窓だけを見ているので、
// NoDocking の独立窓であるこれらは 1 件も検査されていなかった。
void T_NewFloatingPanels(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);

    struct Entry { bool EditorContext::* flag; const char* window; const char* label; };
    static const Entry kPanels[] = {
        { &EditorContext::showLighting,      kWinLighting, "ライティング" },
        { &EditorContext::showTerrainEditor, kWinTerrain,  "地形ツール"   },
        { &EditorContext::showSculptEditor,  kWinSculpt,   "スカルプト"   },
        { &EditorContext::showNavMesh,       kWinNavMesh,  "ナビメッシュ" },
        { &EditorContext::showAudioMixer,    kWinAudioMixer, "オーディオミキサー" },
    };

    // 失敗は集めて最後にまとめて投げる（1 件で中断すると残りが検査されない）
    std::string bad;
    for (const Entry& e : kPanels)
    {
        Step(ctx, "パネルを開く: %s", e.label);
        const bool wasOpen = ed->*(e.flag);
        ed->*(e.flag) = true;

        ImGuiID id = 0;
        for (int i = 0; i < 8 && id == 0; ++i)
        {
            ctx->Yield(6);
            id = FocusWindow(ctx, e.window);
        }
        if (id == 0)
        {
            bad += std::string("\n  ・") + e.label + " : 窓が出ない";
            ed->*(e.flag) = wasOpen;
            ctx->Yield(4);
            continue;
        }

        // 窓は出たが中身が 1 つも無い（描画が即 return している）も異常として拾う
        ctx->SetRef(id);
        ImGuiTestItemList items;
        ctx->GatherItems(&items, "", 4);
        if (items.GetSize() == 0)
            bad += std::string("\n  ・") + e.label + " : 中身が空";

        // 極小サイズでもレイアウトが壊れないか（SameLine 前提の行がゼロ幅で割る温床）
        Step(ctx, "極小サイズへリサイズ: %s", e.label);
        ctx->WindowResize(id, ImVec2(260.0f, 200.0f));
        ctx->Yield(8);
        Step(ctx, "元のサイズへ戻す: %s", e.label);
        ctx->WindowResize(id, ImVec2(430.0f, 760.0f));
        ctx->Yield(8);

        Step(ctx, "パネルを閉じる: %s", e.label);
        ed->*(e.flag) = false;
        ctx->Yield(8);
        if (FocusWindow(ctx, e.window) != 0)
            bad += std::string("\n  ・") + e.label + " : 閉じても窓が残っている";

        ed->*(e.flag) = wasOpen;   // ユーザーの開閉状態は必ず戻す
        ctx->Yield(4);
    }

    if (!bad.empty())
        IM_ERRORF("フローティングパネルに問題があります:%s", bad.c_str());
}

// ---- [P] 設定窓の pg:: 2 カラム化（フェーズ 1b）----
// ポストプロセス / SSAO / SSR・SSGI / ボリュメトリックフォグ / Skybox の各窓の行が pg:: の ID
// （表 / 行ラベル / ##v）で引けること、値が実際に設定へ届くこと、エフェクトの ↺ で既定へ戻ること、
// トーンマップの行があることを確かめる。設定は必ず元へ戻す（未保存扱いを残さない）。
void T_SettingsWindowsPg(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);

    const auto ppBackup   = scene->GetPostSettings();
    const auto ssaoBackup = scene->GetSSAOSettings();
    const auto ssrBackup  = scene->GetSsrSettings();
    const auto ssgiBackup = scene->GetSsgiSettings();
    const auto fogBackup  = scene->GetVolumetricFogSettings();
    const auto skyBackup  = scene->GetSkyboxSettings();

    auto expectNear = [&](float got, float want, const char* what)
    {
        if (std::fabs(got - want) > 0.02f)
            IM_ERRORF("%s が設定へ届いていない（%.3f、期待 %.3f）", what, got, want);
    };
    auto input = [&](ImGuiID win, const char* path, float v, const char* what) -> bool
    {
        if (!ItemExistsIn(ctx, win, path))
        {
            IM_ERRORF("行が見つかりません（%s）: %s", what, path);
            return false;
        }
        ctx->SetRef(win);
        ctx->ItemInputValue(path, v);
        ctx->Yield(4);
        return true;
    };

    // ---- Post Process（エフェクト一覧。パラメータは子窓 ##postlist の中の pg:: 表）----
    Step(ctx, "Post Process: 露出だけ有効にして値を入れる");
    {
        auto& pp = scene->GetPostSettings();
        pp = PostProcessSettings{};
        pp.enabled = true;
        pp.exposureOn = true;
    }
    const float exposureDefault = PostProcessSettings{}.exposure;
    if (const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showPostProcess, "//Post Process"))
    {
        ctx->Yield(6);
        if (!ItemExistsIn(ctx, win, "##ppmaster/トーンマップ/##v"))
            IM_ERRORF("トーンマップの行が見つかりません（##ppmaster/トーンマップ/##v）");
        const ImGuiID list = ChildWindowId(ctx, "//Post Process/##postlist");
        if (list == 0) IM_ERRORF("エフェクト一覧の子窓が見つかりません");
        else if (input(list, "露出 Exposure/##fxparams/値/##v", 3.0f, "露出の値"))
        {
            expectNear(scene->GetPostSettings().exposure, 3.0f, "露出");
            Step(ctx, "Post Process: ↺ でこのエフェクトだけ既定へ戻る");
            ClickPath(ctx, list, "露出 Exposure/↺");
            ctx->Yield(4);
            expectNear(scene->GetPostSettings().exposure, exposureDefault, "↺ 後の露出");
        }
    }
    CloseToolWindow(ctx, &EditorContext::showPostProcess);

    // ---- Post Process パラメータ（詰めた窓）----
    Step(ctx, "Post Process パラメータ: 値を入れる");
    if (const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showPostParams, "//Post Process パラメータ"))
    {
        ctx->Yield(6);
        if (input(win, "露出 Exposure/##fxparams/値/##v", 2.0f, "露出の値（パラメータ窓）"))
            expectNear(scene->GetPostSettings().exposure, 2.0f, "露出（パラメータ窓）");
    }
    CloseToolWindow(ctx, &EditorContext::showPostParams);

    // ---- SSAO ----
    Step(ctx, "SSAO: 半径を入れる");
    scene->GetSSAOSettings().enabled = true;
    if (const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showSSAO, "//SSAO"))
        if (input(win, "##ssao/半径 Radius/##v", 1.5f, "SSAO 半径"))
            expectNear(scene->GetSSAOSettings().radius, 1.5f, "SSAO 半径");
    CloseToolWindow(ctx, &EditorContext::showSSAO);

    // ---- SSR / SSGI ----
    Step(ctx, "SSR / SSGI: 強度を入れる");
    scene->GetSsrSettings().enabled = true;
    scene->GetSsgiSettings().enabled = true;
    if (const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showScreenSpaceGi, "//SSR / SSGI"))
    {
        if (input(win, "##ssrssgi/強度/##v", 0.5f, "SSR 強度"))
            expectNear(scene->GetSsrSettings().intensity, 0.5f, "SSR 強度");
        if (input(win, "##ssrssgi/SSGI 強度/##v", 1.25f, "SSGI 強度"))
            expectNear(scene->GetSsgiSettings().intensity, 1.25f, "SSGI 強度");
    }
    CloseToolWindow(ctx, &EditorContext::showScreenSpaceGi);

    // ---- Volumetric Fog ----
    Step(ctx, "Volumetric Fog: 濃度を入れる");
    scene->GetVolumetricFogSettings().enabled = true;
    if (const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showVolumetricFog, "//Volumetric Fog"))
        if (input(win, "##fog/濃度/##v", 0.1f, "フォグ濃度"))
            expectNear(scene->GetVolumetricFogSettings().density, 0.1f, "フォグ濃度");
    CloseToolWindow(ctx, &EditorContext::showVolumetricFog);

    // ---- Skybox / IBL ----
    Step(ctx, "Skybox / IBL: IBL 強度を入れる");
    if (const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showSkybox, "//Skybox / IBL"))
        if (input(win, "##skybox/IBL Intensity/##v", 2.0f, "IBL 強度"))
            expectNear(scene->GetSkyboxSettings().iblIntensity, 2.0f, "IBL 強度");
    CloseToolWindow(ctx, &EditorContext::showSkybox);

    // 後片付け（設定を元へ）。ランタイム値は Skybox 窓が開いている間しか写さないので、
    // 戻した後にもう一度だけ窓を開いて反映させる必要は無い（元の値がそのまま次フレームの正）。
    scene->GetPostSettings()          = ppBackup;
    scene->GetSSAOSettings()          = ssaoBackup;
    scene->GetSsrSettings()           = ssrBackup;
    scene->GetSsgiSettings()          = ssgiBackup;
    scene->GetVolumetricFogSettings() = fogBackup;
    scene->GetSkyboxSettings()        = skyBackup;
    // Skybox 窓が開いている間だけ「ランタイム値 ← 設定」を写すので、戻した値を反映させるため一瞬だけ開く。
    if (OpenToolWindow(ctx, &EditorContext::showSkybox, "//Skybox / IBL"))
        ctx->Yield(4);
    CloseToolWindow(ctx, &EditorContext::showSkybox);
}

void T_LayoutReset(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    Step(ctx, "ドックレイアウトをリセット（ドックツリーの再構築）");
    // リセットは全ツール窓を閉じる。診断パネル自身も表の一員だが、検査の途中で閉じないよう戻しておく。
    const bool wasDiag = ed->showEngineDiagnostics;
    ed->resetLayout = true;
    ctx->Yield(20);
    ed->showEngineDiagnostics = wasDiag;
    IM_CHECK_NO_RET(!ed->resetLayout);   // 消費されていない＝リセットが走っていない
}

// ===================== 1b W: ワークスペース / レイアウト保存 / 下部ドックのスロット / ツール窓の配置先 =====================
// 共通: 検査の前に「レイアウトをリセット」して基準を揃え、終わったらもう一度リセットして次のテストへ持ち越さない
// （リセットは診断パネルも閉じるので、検査中の診断パネルは戻す）。永続化はテスト中は書かない（WorkspaceManager::PersistAllowed）。
void W_ResetLayout(ImGuiTestContext* ctx, EditorContext* ed)
{
    const bool wasDiag = ed->showEngineDiagnostics;
    ed->resetLayout = true;
    ctx->Yield(14);
    ed->showEngineDiagnostics = wasDiag;
}

// ワークスペース 6 種を順に切り替える: 開く窓が期待どおり・ビューポートの矩形がアスペクトどおりで崩れない・
// 切り替えても各ワークスペースの「最後の状態」が保たれる・level へ戻ると基準のビューポートへ戻る。
void T_WorkspaceSwitch(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(g_app != nullptr && ed != nullptr);
    const cmd::Env env{g_app->GetScene(), PathResolver::AssetsDir()};

    Step(ctx, "プリセットの整合（全窓 id がレジストリに在る・対応するコマンドが在る）");
    for (const ws::Preset& p : ws::Presets())
    {
        IM_CHECK(cmd::FindCommand(std::string("workspace.") + p.id) != nullptr);
        for (const std::string& id : p.layout.open)
            if (tools::Find(id.c_str()) == nullptr) IM_ERRORF("プリセット %s の窓 id がレジストリに無い: %s", p.id, id.c_str());
        for (const std::string& id : p.layout.bottomTabs)
            if (bottomdock::Find(id) == nullptr) IM_ERRORF("プリセット %s の下部タブ id が未登録: %s", p.id, id.c_str());
    }

    Step(ctx, "既定へリセットして基準のビューポートを測る");
    W_ResetLayout(ctx, ed);
    IM_CHECK(ed->currentWorkspace == "level");
    const f32 vpW0 = ed->viewportW, vpH0 = ed->viewportH;
    IM_CHECK_GT(vpW0, 100.0f);
    IM_CHECK_GT(vpH0, 50.0f);
    ctx->LogInfo("基準: viewport %.1fx%.1f  layout L%.3f R%.3f B%.3f / measured L%.4f R%.4f B%.4f", vpW0, vpH0,
                 ed->dockRatioLeft, ed->dockRatioRight, ed->dockRatioBottom, ed->dockMeasLeft, ed->dockMeasRight, ed->dockMeasBottom);

    for (const ws::Preset& p : ws::Presets())
    {
        Step(ctx, "ワークスペース「%s」へ切り替える", p.id);
        cmd::Execute(*ed, env, std::string("workspace.") + p.id);
        ctx->Yield(12);
        IM_CHECK(ed->currentWorkspace == p.id);

        std::string bad;
        for (const tools::Desc& d : tools::kAll)
        {
            if (d.flag == &EditorContext::showEngineDiagnostics) continue;
            const bool want = ws::Contains(p.layout.open, d.id);
            if ((ed->*(d.flag)) != want) bad += std::string("\n  ・") + d.id + (want ? "（開くはず）" : "（閉じるはず）");
        }
        for (const bottomdock::Tab& t : bottomdock::Tabs())
        {
            const bool want = ws::Contains(p.layout.bottomTabs, t.id);
            if (t.open != want) bad += std::string("\n  ・下部タブ ") + t.id + (want ? "（開くはず）" : "（閉じるはず）");
        }
        if (!bad.empty()) IM_ERRORF("ワークスペース %s の窓の開閉が期待と違う:%s", p.id, bad.c_str());

        // ビューポートは崩れない（16:9 のはめ込みのまま・0 にならない）
        const f32 w = ed->viewportW, h = ed->viewportH;
        ctx->LogInfo("  viewport %.1fx%.1f  layout L%.3f R%.3f B%.3f / measured L%.4f R%.4f B%.4f", w, h,
                     ed->dockRatioLeft, ed->dockRatioRight, ed->dockRatioBottom, ed->dockMeasLeft, ed->dockMeasRight, ed->dockMeasBottom);
        IM_CHECK_GT(w, 100.0f);
        IM_CHECK_GT(h, 50.0f);
        IM_CHECK_LT(std::fabs(w / h - 16.0f / 9.0f), 0.03f);
    }

    Step(ctx, "ワークスペースの「最後の状態」が保たれる（material に窓を足して往復）");
    cmd::Execute(*ed, env, "workspace.material");
    ctx->Yield(10);
    ed->showLighting = true;   // material のプリセットには無い窓を足す
    cmd::Execute(*ed, env, "workspace.vfx");
    ctx->Yield(10);
    IM_CHECK(!ed->showLighting);
    cmd::Execute(*ed, env, "workspace.material");
    ctx->Yield(10);
    IM_CHECK(ed->showLighting);            // 足した窓が残っている
    IM_CHECK(ed->showMaterialEditor);      // プリセットの窓も残っている

    Step(ctx, "レベル編集へ戻ると全部閉じ、ビューポートが基準へ戻る");
    cmd::Execute(*ed, env, "workspace.level");
    ctx->Yield(14);
    for (const tools::Desc& d : tools::kAll)
        if (d.flag != &EditorContext::showEngineDiagnostics && (ed->*(d.flag)))
            IM_ERRORF("レベル編集なのに窓が開いている: %s", d.id);
    ctx->LogInfo("viewport 基準 %.1fx%.1f → 往復後 %.1fx%.1f", vpW0, vpH0, ed->viewportW, ed->viewportH);
    IM_CHECK_LT(std::fabs(ed->viewportW - vpW0), 4.0f);
    IM_CHECK_LT(std::fabs(ed->viewportH - vpH0), 4.0f);

    Step(ctx, "未知のワークスペース id は何も変えない");
    ed->pendingWorkspace = "no_such_workspace";
    ctx->Yield(4);
    IM_CHECK(ed->currentWorkspace == "level");

    W_ResetLayout(ctx, ed);
}

// 名前つきレイアウト: 分割比を動かして保存 → リセット → 復元で元の比と窓の開閉へ戻る / 削除 / 壊れたレイアウトは何も変えない。
void T_LayoutSaveRestore(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    const char* kHierarchy = "ヒエラルキー";
    const std::string kName = "ui_test_layout";

    W_ResetLayout(ctx, ed);
    const f32 defaultLeft = ed->dockRatioLeft;
    ImGuiWindow* hier = ImGui::FindWindowByName(kHierarchy);
    IM_CHECK(hier != nullptr && hier->DockNode != nullptr);

    Step(ctx, "ヒエラルキーの幅を広げ、ツール窓を 2 つ開いて、名前つきで保存する");
    const f32 w0 = hier->DockNode->Size.x;
    ImGui::DockBuilderSetNodeSize(hier->DockNode->ID, ImVec2(w0 * 1.4f, hier->DockNode->Size.y));
    ImGui::DockBuilderFinish(hier->DockNode->ID);
    ctx->Yield(8);
    hier = ImGui::FindWindowByName(kHierarchy);
    IM_CHECK(hier != nullptr && hier->DockNode != nullptr);
    const f32 widened = hier->DockNode->Size.x;
    IM_CHECK_GT(widened, w0 * 1.2f);
    IM_CHECK_GT(ed->dockRatioLeft, defaultLeft + 0.02f);   // ユーザーの操作として分割比に取り込まれた
    const f32 savedLeft = ed->dockRatioLeft;
    ed->showPostProcess = true;
    ed->showLighting = true;
    ctx->Yield(4);
    ed->pendingLayoutSaveName = kName;
    ctx->Yield(4);
    bool listed = false;
    for (const std::string& n : ed->savedLayoutNames) listed = listed || n == kName;
    IM_CHECK(listed);

    Step(ctx, "リセットすると既定へ戻る");
    W_ResetLayout(ctx, ed);
    IM_CHECK(!ed->showPostProcess && !ed->showLighting);
    IM_CHECK_LT(std::fabs(ed->dockRatioLeft - defaultLeft), 0.005f);

    Step(ctx, "復元すると保存時の比・窓の開閉・実際の幅へ戻る");
    ed->pendingLayoutRestore = kName;
    ctx->Yield(12);
    IM_CHECK(ed->showPostProcess && ed->showLighting);
    IM_CHECK_LT(std::fabs(ed->dockRatioLeft - savedLeft), 0.01f);
    hier = ImGui::FindWindowByName(kHierarchy);
    IM_CHECK(hier != nullptr && hier->DockNode != nullptr);
    ctx->LogInfo("hierarchy width: 既定 %.1f → 広げた %.1f → 復元 %.1f", w0, widened, hier->DockNode->Size.x);
    IM_CHECK_LT(std::fabs(hier->DockNode->Size.x - widened), 8.0f);

    Step(ctx, "壊れたレイアウトを復元しても今の配置を変えない（黙って既定にもしない）");
    prefs::SetString((std::string(ws::NamedLayoutPrefix()) + "ui_test_broken").c_str(), "{oops");
    ed->pendingLayoutRestore = "ui_test_broken";
    ctx->Yield(6);
    IM_CHECK(ed->showPostProcess && ed->showLighting);
    prefs::Erase((std::string(ws::NamedLayoutPrefix()) + "ui_test_broken").c_str());

    Step(ctx, "削除すると一覧から消える");
    ed->pendingLayoutDelete = kName;
    ctx->Yield(4);
    listed = false;
    for (const std::string& n : ed->savedLayoutNames) listed = listed || n == kName;
    IM_CHECK(!listed);

    Step(ctx, "空 / 長すぎる名前は保存しない");
    const size_t before = ed->savedLayoutNames.size();
    ed->pendingLayoutSaveName = "   ";
    ctx->Yield(3);
    ed->pendingLayoutSaveName = std::string(200, 'x');
    ctx->Yield(3);
    IM_CHECK_EQ(ed->savedLayoutNames.size(), before);

    ui::ToastClearAll();
    W_ResetLayout(ctx, ed);
}

// 下部ドックのスロット: 登録タブ（ダミーのタイムライン）を開くと中央下ノードへ入る・ビューポートの矩形が変わらない・
// 最大化 / 元に戻す（コマンドとタブ帯のダブルクリック）で寸法が往復・最大化中もゲーム絵の矩形が 0 にならない・再登録で置き換わる。
void T_BottomDockSlot(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    W_ResetLayout(ctx, ed);

    bottomdock::Tab* tab = bottomdock::Find("timeline");
    IM_CHECK(tab != nullptr);
    IM_CHECK(!tab->open);
    IM_CHECK_GT(ed->dockNodeBottom, 0u);
    const f32 vpW0 = ed->viewportW, vpH0 = ed->viewportH;

    Step(ctx, "ダミータブを開くと中央下ノード（アセットブラウザと同じ）へ入り、ビューポートは変わらない");
    bottomdock::SetOpen("timeline", true);
    ctx->Yield(12);
    const std::string winName = bottomdock::WindowName(*bottomdock::Find("timeline"));
    ImGuiWindow* w = ImGui::FindWindowByName(winName.c_str());
    IM_CHECK(w != nullptr && w->DockNode != nullptr);
    IM_CHECK_EQ(w->DockNode->ID, static_cast<ImGuiID>(ed->dockNodeBottom));
    ImGuiWindow* assets = ImGui::FindWindowByName("アセットブラウザ");
    IM_CHECK(assets != nullptr && assets->DockNode != nullptr);
    IM_CHECK_EQ(assets->DockNode->ID, w->DockNode->ID);
    IM_CHECK_LT(std::fabs(ed->viewportW - vpW0), 1.0f);
    IM_CHECK_LT(std::fabs(ed->viewportH - vpH0), 1.0f);

    Step(ctx, "最大化（コマンド）: 下部ドックが広がり、ビューポートは縮むが 0 にならない");
    const f32 nodeH0 = w->DockNode->Size.y;
    ed->bottomDockMaximizeToggle = true;
    ctx->Yield(10);
    IM_CHECK(ed->bottomDockMaximized);
    w = ImGui::FindWindowByName(winName.c_str());
    IM_CHECK(w != nullptr && w->DockNode != nullptr);
    ctx->LogInfo("最大化: 下部ノード高 %.1f → %.1f / viewport %.1fx%.1f → %.1fx%.1f", nodeH0, w->DockNode->Size.y, vpW0, vpH0, ed->viewportW, ed->viewportH);
    IM_CHECK_GT(w->DockNode->Size.y, nodeH0 * 1.25f);
    IM_CHECK_LT(ed->viewportH, vpH0 - 30.0f);
    IM_CHECK_GT(ed->viewportH, 40.0f);
    IM_CHECK_GT(ed->viewportW, 60.0f);

    Step(ctx, "元に戻す: 寸法が往復する");
    ed->bottomDockMaximizeToggle = true;
    ctx->Yield(10);
    IM_CHECK(!ed->bottomDockMaximized);
    IM_CHECK_LT(std::fabs(ed->viewportW - vpW0), 3.0f);
    IM_CHECK_LT(std::fabs(ed->viewportH - vpH0), 3.0f);

    Step(ctx, "タブ帯のダブルクリックで最大化 / もう一度で元に戻る");
    for (int round = 0; round < 2; ++round)
    {
        w = ImGui::FindWindowByName(winName.c_str());
        IM_CHECK(w != nullptr && w->DockNode != nullptr && w->DockNode->TabBar != nullptr);
        const ImRect bar = w->DockNode->TabBar->BarRect;
        ctx->MouseMoveToPos(ImVec2(bar.Max.x - ui::Px(60.0f), (bar.Min.y + bar.Max.y) * 0.5f));
        ctx->MouseDoubleClick(0);
        ctx->Yield(10);
        IM_CHECK_EQ(ed->bottomDockMaximized, round == 0);
    }
    IM_CHECK_LT(std::fabs(ed->viewportH - vpH0), 3.0f);

    Step(ctx, "閉じてから開き直すとまた下部ドックへ入る");
    bottomdock::SetOpen("timeline", false);
    ctx->Yield(6);
    bottomdock::SetOpen("timeline", true);
    ctx->Yield(10);
    w = ImGui::FindWindowByName(winName.c_str());
    IM_CHECK(w != nullptr && w->DockNode != nullptr);
    IM_CHECK_EQ(w->DockNode->ID, static_cast<ImGuiID>(ed->dockNodeBottom));

    Step(ctx, "同じ id で再登録すると置き換わる（開閉は保つ。シーケンサーが本物を差し込む経路）");
    const size_t nTabs = bottomdock::Tabs().size();
    bool drawn = false;
    static bool* s_drawn = nullptr;
    s_drawn = &drawn;
    bottomdock::Register({"timeline", "タイムライン（ダミー）", ICON_FILM, [](EditorContext&) { if (s_drawn) *s_drawn = true; }});
    ctx->Yield(6);
    s_drawn = nullptr;   // 以降は呼ばれても触らない（drawn はこの関数のローカル）
    IM_CHECK_EQ(bottomdock::Tabs().size(), nTabs);
    IM_CHECK(bottomdock::IsOpen("timeline"));
    IM_CHECK(drawn);
    // 元のダミーの本文へ戻す（他のテストの見た目に影響させない）
    bottomdock::Register({"timeline", "タイムライン（ダミー）", ICON_FILM, [](EditorContext&) {
        ImGui::Spacing();
        ImGui::TextDisabled("ここにシーケンサーのタイムラインが入ります（下部ドックのスロット。動作確認用のダミー）。");
        ImGui::TextDisabled("タブ帯をダブルクリックすると下部ドックが広がります（もう一度で元に戻ります）。");
    }});

    bottomdock::SetOpen("timeline", false);
    W_ResetLayout(ctx, ed);
}

// ツール窓の配置先（右タブ / 右分割 / 下部 / フローティング）: 窓を開いたまま配置先を変えると実際のドックノードが変わる・
// ビューポートは動かない・右分割の作り直しでも寸法が保たれる・元へ戻せる。
void T_ToolDockSlots(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    W_ResetLayout(ctx, ed);
    const char* kSkybox = "Skybox / IBL";
    const f32 vpW0 = ed->viewportW, vpH0 = ed->viewportH;
    ImGuiWindow* insp = ImGui::FindWindowByName("インスペクター");
    IM_CHECK(insp != nullptr && insp->DockNode != nullptr);
    const ImGuiID rightNode = insp->DockNode->ID;

    ed->showSkybox = true;
    ctx->Yield(8);
    ImGuiWindow* w = ImGui::FindWindowByName(kSkybox);
    IM_CHECK(w != nullptr && w->DockNode != nullptr);
    IM_CHECK_EQ(w->DockNode->ID, rightNode);   // 既定は右カラムのタブ（インスペクターと同じノード）

    Step(ctx, "下部ドックへ移す");
    ed->pendingToolSlots.push_back({"skybox", static_cast<int>(ws::Slot::BottomTab)});
    ctx->Yield(10);
    w = ImGui::FindWindowByName(kSkybox);
    IM_CHECK(w != nullptr && w->DockNode != nullptr);
    IM_CHECK_EQ(w->DockNode->ID, static_cast<ImGuiID>(ed->dockNodeBottom));

    Step(ctx, "右カラムを分割して並べる（レイアウトの作り直し。ビューポートは動かない）");
    ed->pendingToolSlots.push_back({"skybox", static_cast<int>(ws::Slot::RightSplit)});
    ctx->Yield(14);
    IM_CHECK_GT(ed->dockNodeRightSplit, 0u);
    w = ImGui::FindWindowByName(kSkybox);
    IM_CHECK(w != nullptr && w->DockNode != nullptr);
    IM_CHECK_EQ(w->DockNode->ID, static_cast<ImGuiID>(ed->dockNodeRightSplit));
    insp = ImGui::FindWindowByName("インスペクター");
    IM_CHECK(insp != nullptr && insp->DockNode != nullptr);
    IM_CHECK(insp->DockNode->ID != w->DockNode->ID);   // インスペクターと別ノード＝並んで見える
    ctx->LogInfo("viewport %.1fx%.1f → %.1fx%.1f", vpW0, vpH0, ed->viewportW, ed->viewportH);
    IM_CHECK_LT(std::fabs(ed->viewportW - vpW0), 3.0f);
    IM_CHECK_LT(std::fabs(ed->viewportH - vpH0), 3.0f);

    Step(ctx, "フローティングへ（ドックから外れる）");
    ed->pendingToolSlots.push_back({"skybox", static_cast<int>(ws::Slot::Floating)});
    ctx->Yield(14);
    w = ImGui::FindWindowByName(kSkybox);
    IM_CHECK(w != nullptr);
    IM_CHECK(w->DockNode == nullptr);
    IM_CHECK_LT(std::fabs(ed->viewportW - vpW0), 3.0f);

    Step(ctx, "右タブへ戻す");
    ed->pendingToolSlots.push_back({"skybox", static_cast<int>(ws::Slot::RightTab)});
    ctx->Yield(14);
    w = ImGui::FindWindowByName(kSkybox);
    insp = ImGui::FindWindowByName("インスペクター");
    IM_CHECK(w != nullptr && w->DockNode != nullptr && insp != nullptr && insp->DockNode != nullptr);
    IM_CHECK_EQ(w->DockNode->ID, insp->DockNode->ID);

    Step(ctx, "配置先が動かせない窓（既定がフローティング）は無視される");
    ed->pendingToolSlots.push_back({"lighting", static_cast<int>(ws::Slot::BottomTab)});
    ed->pendingToolSlots.push_back({"no_such_tool", 1});
    ctx->Yield(4);

    W_ResetLayout(ctx, ed);
}

// ---- プロジェクトランチャー ----
// プロジェクトを閉じてランチャーへ戻り、タブ切替 / 新規フォームの検証表示 / テンプレ選択 / 検索 / アニメ設定を
// 内部状態（LauncherScreen::Debug）で確かめ、最後に同じプロジェクトを開き直して他のテストへ戻す。
void T_LauncherScreen(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(g_app != nullptr && ed != nullptr);
    using dx12e::LauncherScreen;
    const std::string projRoot = PathResolver::BaseDir();
    if (projRoot.empty()) { ctx->LogWarning("プロジェクトが開かれていないためランチャーの検査を飛ばします"); return; }

    Step(ctx, "プロジェクトを閉じてランチャーへ戻る");
    if (ed->IsSceneDirty()) ed->unsavedChoice = EditorContext::UnsavedChoice::Discard;   // 未保存の確認モーダルを出さない
    ed->pendingCloseProject = true;
    for (int i = 0; i < 90 && !LauncherScreen::Debug().visible; ++i) ctx->Yield(2);
    IM_CHECK(LauncherScreen::Debug().visible);

    const bool animOrig = LauncherScreen::Debug().animations;

    Step(ctx, "ナビの 5 タブを切り替える");
    for (int t = 0; t < 5; ++t)
    {
        LauncherScreen::DebugSetTab(t);
        ctx->Yield(4);
        IM_CHECK_EQ(LauncherScreen::Debug().tab, t);
    }

    Step(ctx, "新規フォーム: 不正な名前 / 相対パス / 日本語 / 正常を検証表示に反映する");
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string tmp = dx12e::launcher::PathToUtf8(fs::temp_directory_path(ec) / "uno_launcher_uitest");
    LauncherScreen::DebugSetTab(1);
    ctx->Yield(4);
    LauncherScreen::DebugSetForm("bad:name", tmp);
    ctx->Yield(6);
    IM_CHECK(LauncherScreen::Debug().formHasError);
    IM_CHECK(LauncherScreen::Debug().formFirstIssueCode == "name.chars");
    LauncherScreen::DebugSetForm("Good_Name", "relative\\path");
    ctx->Yield(6);
    IM_CHECK(LauncherScreen::Debug().formHasError);
    IM_CHECK(LauncherScreen::Debug().formFirstIssueCode == "loc.relative");
    LauncherScreen::DebugSetForm("日本語のゲーム", tmp);
    ctx->Yield(6);
    IM_CHECK(!LauncherScreen::Debug().formHasError);
    IM_CHECK(LauncherScreen::Debug().formHasWarn);            // 日本語は警告（作成は止めない）
    LauncherScreen::DebugSetForm("Good_Name", tmp + "\\nested\\deeper");
    ctx->Yield(6);
    IM_CHECK(!LauncherScreen::Debug().formHasError);           // 保存場所が無くても作成される（情報だけ）

    Step(ctx, "テンプレートの選択");
    const int nT = static_cast<int>(dx12e::launcher::Templates().size());
    IM_CHECK_GT(nT, 3);
    LauncherScreen::DebugSelectTemplate(nT - 1);
    ctx->Yield(3);
    IM_CHECK_EQ(LauncherScreen::Debug().templateIndex, nT - 1);
    LauncherScreen::DebugSelectTemplate(0);

    Step(ctx, "最近のプロジェクトの検索");
    LauncherScreen::DebugSetTab(0);
    LauncherScreen::DebugReloadRecents();
    ctx->Yield(6);
    const int total = LauncherScreen::Debug().recentCount;
    LauncherScreen::DebugSetSearch("zzzz_no_such_project_zzzz");
    ctx->Yield(3);
    IM_CHECK_EQ(LauncherScreen::Debug().visibleRecentCount, 0);
    LauncherScreen::DebugSetSearch("");
    ctx->Yield(3);
    IM_CHECK_EQ(LauncherScreen::Debug().visibleRecentCount, total);

    Step(ctx, "アニメーション設定の切替（画面の状態だけ。保存はしない）");
    LauncherScreen::DebugSetAnimations(false);
    ctx->Yield(3);
    IM_CHECK(!LauncherScreen::Debug().animations);
    LauncherScreen::DebugSetAnimations(animOrig);

    Step(ctx, "同じプロジェクトを開き直す（他のテストへ戻す）");
    LauncherScreen::DebugRequestOpen(projRoot);
    for (int i = 0; i < 120 && LauncherScreen::Debug().visible; ++i) ctx->Yield(2);
    IM_CHECK(!LauncherScreen::Debug().visible);
    ctx->Yield(30);
    ctx->Yield(300);   // 段階ロード（テクスチャの先読み → シーン構築）が終わるまで
    IM_CHECK(!Ed()->currentScenePath.empty());
}

// ---- UI エディタ ----

void T_UiEditorSpawnAll(ImGuiTestContext* ctx)
{
    Step(ctx, "UI エディタを開く");
    const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showUiEditor, kWinUiEditor);
    if (win == 0) return;

    for (const char* btn : { "+ Canvas", "+ 画像", "+ テキスト", "+ ボタン",
                             "+ スライダー", "+ トグル", "+ スクロール" })
    {
        Step(ctx, "UI 要素を配置: %s", btn);
        ClickPath(ctx, win, btn);
        ctx->Yield(8);   // spawn はフレーム境界の遅延処理
    }

    CloseToolWindow(ctx, &EditorContext::showUiEditor);
}

void T_UiEditorViewOps(ImGuiTestContext* ctx)
{
    const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showUiEditor, kWinUiEditor);
    if (win == 0) return;

    Step(ctx, "UI エディタの表示操作（Fit / 100% / グリッド / 市松）");
    for (const char* btn : { "Fit", "100%", "グリッド", "市松" })
    {
        ClickPath(ctx, win, btn);
        ctx->Yield(4);
    }

    // 基準解像度を全プリセット切り替える（レイアウト計算とアンカー解決の総なめ）
    Step(ctx, "画面サイズプリセットを全通り切り替える");
    ctx->SetRef(win);
    ctx->ComboClickAll("##UiEdScreenSize");
    ctx->Yield(6);

    // 極端に小さい窓にしてもレイアウトが壊れないか（ゼロ除算・負サイズの温床）
    Step(ctx, "UI エディタを極小サイズへリサイズ");
    ctx->WindowResize(win, ImVec2(240.0f, 160.0f));
    ctx->Yield(8);
    Step(ctx, "UI エディタを元のサイズへ戻す");
    ctx->WindowResize(win, ImVec2(900.0f, 640.0f));
    ctx->Yield(8);

    CloseToolWindow(ctx, &EditorContext::showUiEditor);
}

// ---- パーティクル ----

void T_VfxEditorAllKinds(ImGuiTestContext* ctx)
{
    Step(ctx, "パーティクルエディタを開く");
    const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showVfxEditor, kWinVfx);
    if (win == 0) return;

    Step(ctx, "新規 VFX アセットを作る");
    ClickPath(ctx, ChildWindowId(ctx, kWinVfxAssetList), "＋ 新規 New");
    ctx->Yield(6);

    // 見た目 8 種 / 合成 / 向き を全通り選ぶ（それぞれ別シェーダ・別パラメータ経路）。
    // 「向き Orient」は kind によって出ないことがあるので存在チェック付き。
    const ImGuiID emission = ChildWindowId(ctx, kWinVfxEmission);
    // ★フェーズ 1b: 3 つのコンボは pg:: の 2 カラム表（"##vfxEmission"）の行になった。
    //   行のコンボの実 ID は「表 / 行ラベル(PushID) / ##v」。
    struct VfxCombo { const char* path; const char* name; };
    for (const VfxCombo combo : { VfxCombo{"##vfxEmission/見た目 Kind/##v", "見た目 Kind"},
                                  VfxCombo{"##vfxEmission/合成 Blend/##v", "合成 Blend"},
                                  VfxCombo{"##vfxEmission/向き Orient/##v", "向き Orient"} })
    {
        if (!ItemExistsIn(ctx, emission, combo.path))
        {
            ctx->LogWarning("コンボが見つかりません(スキップ): %s", combo.name);
            continue;
        }
        Step(ctx, "%s を全通り切り替える", combo.name);
        ctx->SetRef(emission);
        ctx->ComboClickAll(combo.path);
        ctx->Yield(6);
    }

    Step(ctx, "プレビューをクリア");
    ClickPath(ctx, ChildWindowId(ctx, kWinVfxMain), "プレビューをクリア Clear");
    ctx->Yield(6);

    CloseToolWindow(ctx, &EditorContext::showVfxEditor);
}

void T_VfxApplyAndSpawn(ImGuiTestContext* ctx)
{
    Step(ctx, "適用先の Box を生成して選択");
    AddEntity(ctx, "Box");
    SelectLastEntity(ctx);

    const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showVfxEditor, kWinVfx);
    if (win == 0) return;

    const ImGuiID main = ChildWindowId(ctx, kWinVfxMain);

    // 「選択エンティティへ適用」は ParticleEmitter を持つエンティティが選択されていないと
    // 無効化される（Box だけでは押せない）。存在だけ確かめて、押せない場合は飛ばす。
    Step(ctx, "選択エンティティへ適用");
    if (ItemExistsIn(ctx, main, "選択エンティティへ適用"))
    {
        ctx->ItemClick("選択エンティティへ適用", 0, ImGuiTestOpFlags_NoError);
        ctx->Yield(8);
    }

    Step(ctx, "新規エンティティとして配置");
    ClickPath(ctx, main, "新規エンティティとして配置");
    ctx->Yield(10);

    Step(ctx, "Lua コードをコピー");
    ClickPath(ctx, main, "Luaコードをコピー");
    ctx->Yield(4);

    CloseToolWindow(ctx, &EditorContext::showVfxEditor);
}

// ---- マテリアル ----

void T_MaterialEditor(ImGuiTestContext* ctx)
{
    Step(ctx, "マテリアルエディタを開く（プレビュー用の描画リソースを確保する）");
    const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showMaterialEditor, kWinMaterial);
    if (win == 0) return;

    Step(ctx, "新規マテリアル");
    ClickPath(ctx, win, "新規 New");
    ctx->Yield(8);

    // プレビュー形状トグル（ラベルが押すたびに入れ替わる）
    Step(ctx, "プレビュー形状を 球体 → 平面 → 球体");
    ClickPath(ctx, win, "球体 Sphere");
    ctx->Yield(8);
    ClickPath(ctx, win, "平面 Plane");
    ctx->Yield(8);

    Step(ctx, "パラメータの折りたたみを開き切る");
    ctx->SetRef(win);
    ctx->ItemOpenAll("", 2);
    ctx->Yield(6);

    // ※「画像から作成... Import Image」「保存 Save」は押さない
    //   （前者は Win32 のファイルダイアログで固まる／後者は assets を汚す）
    CloseToolWindow(ctx, &EditorContext::showMaterialEditor);
    ctx->Yield(6);
}

void T_MaterialLibrary(ImGuiTestContext* ctx)
{
    Step(ctx, "マテリアルライブラリ (Poly Haven) を開いて描画");
    const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showMaterialLibrary, kWinMatLibrary);
    if (win == 0) return;

    // ダウンロードは走らせない（ネットワークと外部ファイル書き込みが絡むため描画だけ）。
    // 未ダウンロード状態では説明テキストしか出ない＝操作可能な項目は 0 件でも正常。
    ctx->SetRef(win);
    ctx->Yield(20);

    CloseToolWindow(ctx, &EditorContext::showMaterialLibrary);
}

// ---- 再生 ----

u32 g_srvUsedAfterFirstCycle = 0;   // T_PlayStopCycle 内でのみ使う

void T_PlayStopCycle(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);

    // ★RT 影を一時的に ON にする。バインドレス SRV（Mesh の VB/IB）は DXR が有効な
    //   フレームでしか払い出されないので、OFF のままだと下のリーク検査が空振りする
    //   （実際、有効化しないと 2 件のリークをどちらも見逃した）。
    Scene* scene = g_app->GetScene();
    bool savedRtShadow = false;
    if (scene)
    {
        savedRtShadow = scene->GetRtSettings().shadowEnabled;
        scene->GetRtSettings().shadowEnabled = true;
        ctx->Yield(6);   // TLAS が組まれて SRV が払い出されるまで回す
    }

    for (int i = 0; i < 2; ++i)
    {
        Step(ctx, "Play へ入る (%d 回目)", i + 1);
        g_app->RequestMode(Application::EngineMode::Playing);
        int frames = 0;
        while (g_app->GetEngineMode() != Application::EngineMode::Playing && frames < 240)
        {
            ctx->Yield();
            ++frames;
        }
        if (g_app->GetEngineMode() != Application::EngineMode::Playing)
        {
            // カメラ未設定などで Play できないシーンは珍しくないので失敗にはしない
            ctx->LogWarning("Play に入れませんでした（アクティブなカメラが無い？）");
            return;
        }

        Step(ctx, "Play 中のまま描画を回す");
        ctx->Yield(45);

        Step(ctx, "Stop してエディタへ戻る (%d 回目)", i + 1);
        g_app->RequestMode(Application::EngineMode::Editor);
        frames = 0;
        while (g_app->GetEngineMode() != Application::EngineMode::Editor && frames < 240)
        {
            ctx->Yield();
            ++frames;
        }
        IM_CHECK(g_app->GetEngineMode() == Application::EngineMode::Editor);
        ctx->Yield(10);

        // ★1 往復目の直後のヒープ使用量を基準にして、2 往復目で増えていないか見る。
        //   Play/Stop はシーンを丸ごと作り直すので、確保しっぱなしのディスクリプタが
        //   あると往復のたびに単調増加し、最後は AllocateIndex が例外を投げて
        //   **シーンビューが真っ黒のまま戻らない**。実際 2 件あった:
        //     - Mesh の RT 用バインドレス SRV（VB/IB 各 1）がデストラクタで返らない
        //     - m_materialOverrideSrvCache が entt の version 込みキーで毎回取り直す
        //   1 往復目は初回確保が混ざるので基準にしない。
        const u32 used = g_app->GetDiagRenderHealth().srvHeapCapacity
                       - g_app->GetDiagRenderHealth().srvHeapFree;
        if (i == 0) { g_srvUsedAfterFirstCycle = used; }
        else if (used > g_srvUsedAfterFirstCycle)
        {
            IM_ERRORF("Play/Stop でディスクリプタが戻っていません: %u -> %u (+%u/往復)",
                      g_srvUsedAfterFirstCycle, used, used - g_srvUsedAfterFirstCycle);
        }
    }

    if (scene) scene->GetRtSettings().shadowEnabled = savedRtShadow;   // 元に戻す
}

// ---- ビルド ----

void T_BuildGame(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;

    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);

    // 出力先を一時フォルダへ差し替える（ユーザーの設定は最後に必ず戻す）
    const std::string savedOutputDir = ed->buildConfig.outputDir;
    const bool        savedOpen      = ed->buildConfig.openFolderAfterBuild;

    std::error_code ec;
    const fs::path outDir = fs::temp_directory_path(ec) / "dx12_diagnostics_build";
    fs::create_directories(outDir, ec);

    ed->buildConfig.outputDir            = outDir.string();
    ed->buildConfig.openFolderAfterBuild = false;   // 完了時に Explorer を開かせない
    ed->buildErrorMsg.clear();
    ed->lastBuildDir.clear();

    Step(ctx, "ビルド設定を開く（出力先: %s）", outDir.string().c_str());
    const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showBuildSettings, kWinBuild);
    if (win != 0)
    {
        Step(ctx, "ビルドを実行（数十秒かかることがあります）");
        ClickPath(ctx, win, "ビルド");
    }
    else
    {
        ed->pendingBuildGame = true;
    }

    // 実行はフレーム境界。BuildGame() 自体は同期実行なので pendingBuildGame が降りたら完了。
    int frames = 0;
    while (ed->pendingBuildGame && frames < 600)
    {
        ctx->Yield();
        ++frames;
    }
    ctx->Yield(4);

    const bool        failed   = (ed->buildErrorFlash > 0.0f);
    const std::string buildDir = ed->lastBuildDir;
    const std::string errMsg   = ed->errorMessage;

    // 失敗時は中央モーダルが出るので閉じておく（次のテストが操作できなくなる）
    ctx->PopupCloseAll();
    ctx->Yield(2);

    // 後片付け（設定を戻す）
    ed->buildConfig.outputDir            = savedOutputDir;
    ed->buildConfig.openFolderAfterBuild = savedOpen;
    CloseToolWindow(ctx, &EditorContext::showBuildSettings);

    if (failed)
        IM_ERRORF("ビルドに失敗しました: %s", errMsg.empty() ? "(詳細は dx12_engine.log)" : errMsg.c_str());

    IM_CHECK_NO_RET(!buildDir.empty());
    if (!buildDir.empty())
    {
        Step(ctx, "成果物を確認: %s", buildDir.c_str());
        // exe 名はゲーム名から生成される（BuildGame と同じサニタイズ。空なら "Game"）
        std::string exeStem;
        for (char c : std::string(ed->buildConfig.title))
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ')
                exeStem += c;
        while (!exeStem.empty() && exeStem.back()  == ' ') exeStem.pop_back();
        while (!exeStem.empty() && exeStem.front() == ' ') exeStem.erase(exeStem.begin());
        if (exeStem.empty()) exeStem = "Game";
        const fs::path exePath = fs::path(buildDir) / (exeStem + ".exe");
        IM_CHECK_NO_RET(fs::exists(exePath));
        IM_CHECK_NO_RET(fs::exists(fs::path(buildDir) / "game.pak"));

        // ★成果物が「在る」ことしか見ていなかったので、**起動直後に必ずクラッシュする
        //   ゲームを 3 日間出荷できていた**（Application::Run の if に波括弧が無く、
        //   MCP ブリッジを持たないゲームモードで null 参照していた）。
        //   実際に起動して数秒生きているかを見る。ここが唯一 GameRuntime に触るテスト。
        if (fs::exists(exePath))
        {
            Step(ctx, "配布ゲームを起動して落ちないか見る");
            // ★人の画面に窓を出さず前面も取らない: `--background=offscreen,tool`（画面外・非アクティブ・
            //   仮想入力）で起動する。以前は素の起動で、約 6 秒間ゲーム窓が前面に出てユーザー操作を奪っていた。
            //   保険として STARTUPINFO でも「最小化・非アクティブ」を指定する（--background を知らない古い exe 用）。
            std::wstring cmd = L"\"" + exePath.wstring() + L"\" --background=offscreen,tool";
            std::wstring cwd = fs::path(buildDir).wstring();
            STARTUPINFOW si{};
            si.cb          = sizeof(si);
            si.dwFlags     = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWMINNOACTIVE;
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                0, nullptr, cwd.c_str(), &si, &pi))
            {
                IM_ERRORF("ビルドしたゲームを起動できません（CreateProcess 失敗 %lu）", GetLastError());
            }
            else
            {
                // 生きていれば TIMEOUT が返る。先に終わった＝異常終了。
                const DWORD wr = WaitForSingleObject(pi.hProcess, 6000);
                if (wr == WAIT_OBJECT_0)
                {
                    DWORD code = 0;
                    GetExitCodeProcess(pi.hProcess, &code);
                    const bool crashed = fs::exists(fs::path(buildDir) / "dx12_crash.log");
                    IM_ERRORF("ビルドしたゲームが起動直後に終了しました (exit=0x%08lX%s)",
                              code, crashed ? ", dx12_crash.log あり" : "");
                }
                else
                {
                    TerminateProcess(pi.hProcess, 0);
                    WaitForSingleObject(pi.hProcess, 3000);
                }
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            // ゲーム窓にフォーカスを取られたままだと後続テストのキー入力が届かない
            ctx->Yield(30);
        }
    }

    fs::remove_all(outDir, ec);   // 一時フォルダを掃除（失敗しても無視）
}

// ---- ストレス ----

void T_RapidSelection(ImGuiTestContext* ctx)
{
    Step(ctx, "選択を高速に切り替える");
    ctx->SetRef(kWinHierarchy);
    {
        ImGuiTestItemList probe;
        ctx->GatherItems(&probe, "", 3);
        IM_CHECK_NO_RET(probe.GetSize() > 0);
    }

    for (int pass = 0; pass < 6; ++pass)
    {
        // 毎パス集め直す。クリッパーで可視行が入れ替わると前の ID は消えるため
        // （使い回すと "Unable to locate item" で落ちる）。
        ctx->SetRef(kWinHierarchy);
        ImGuiTestItemList items;
        ctx->GatherItems(&items, "", 3);
        for (int i = 0; i < items.GetSize() && i < 10; ++i)
        {
            const ImGuiTestItemInfo* item = items[i];
            if (item == nullptr || item->ID == 0) continue;
            if (std::strstr(item->DebugLabel, ICON_PLUS) != nullptr) continue;
            if (!ctx->ItemExists(item->ID)) continue;
            ctx->MouseMove(item->ID);
            ctx->MouseClick(0);
            ctx->Yield();   // 1 フレームだけ＝Inspector が描き終わる前に次の選択へ
        }
    }
    ctx->Yield(6);
}

void T_SpamAddDelete(ImGuiTestContext* ctx)
{
    for (int i = 0; i < 8; ++i)
    {
        Step(ctx, "連続生成→削除 %d 回目", i + 1);
        AddEntity(ctx, "Box");
        SelectLastEntity(ctx);
        ctx->SetRef(kWinToolbar);
        ctx->MenuClick("編集/削除");
        ctx->Yield(4);
    }
    ctx->Yield(8);
}

// ===================== 超詳細診断 =====================
// ここから下は「UI が動くか」ではなく「エンジンの機能そのものが正しいか」を見る。
// 実体（純データ検査）は gui/DeepDiagnostics.cpp。ここはその結果を検査結果へ変換する薄い層と、
// GPU/ImGuizmo が絡んで純データでは書けないぶんだけ。

// DeepDiagReport を検査結果へ流し込む。エラーが 1 件でもあればテスト失敗。
// 注意/情報は失敗にしない（誤検知で赤くすると誰も見なくなる）。
void ReportDeep(ImGuiTestContext* ctx, const DeepDiagReport& r)
{
    ctx->LogInfo("%s: %s", r.title.c_str(), r.Summary().c_str());

    for (const DeepDiagIssue& i : r.issues)
    {
        if (i.level >= 2)      ctx->LogError("%s", i.text.c_str());
        else if (i.level == 1) ctx->LogWarning("注意: %s", i.text.c_str());
        else                   ctx->LogInfo("情報: %s", i.text.c_str());
    }
    if (r.omitted > 0)
        ctx->LogWarning("他 %d 件は表示を省略した", r.omitted);
    if (!r.skipped.empty())
        ctx->LogWarning("%s", r.skipped.c_str());

    if (r.Errors() > 0)
        IM_ERRORF("%s: %d 件の問題（上の赤い行を参照）", r.title.c_str(), r.Errors());
}

// シーン RT を 1 枚読み戻して数値化する。要求はフレーム境界で消化されるので数フレーム待つ。
Application::DiagFrameStats GrabFrame(ImGuiTestContext* ctx)
{
    if (g_app == nullptr) return {};
    g_app->RequestDiagnosticFrameStats();
    for (int i = 0; i < 180; ++i)
    {
        ctx->Yield();
        const Application::DiagFrameStats s = g_app->TakeDiagnosticFrameStats();
        if (s.valid) return s;
    }
    return {};
}

// Transform に NaN/Inf が入っていないか
bool TransformFinite(const Transform& t)
{
    const float v[9] = { t.position.x, t.position.y, t.position.z,
                         t.rotation.x, t.rotation.y, t.rotation.z,
                         t.scale.x,    t.scale.y,    t.scale.z };
    for (float f : v) if (!std::isfinite(f)) return false;
    return true;
}

bool TransformEqual(const Transform& a, const Transform& b)
{
    return a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z
        && a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z
        && a.scale.x    == b.scale.x    && a.scale.y    == b.scale.y    && a.scale.z    == b.scale.z;
}

// Box を 1 個足して、その entt::entity を返す（ヒエラルキーの並び順に依存しない取り方）。
entt::entity AddBoxAndGet(ImGuiTestContext* ctx, entt::registry& reg)
{
    std::set<entt::entity> before;
    for (auto e : reg.view<Transform>()) before.insert(e);

    AddEntity(ctx, "Box");
    ctx->Yield(6);

    for (auto e : reg.view<Transform>())
        if (before.find(e) == before.end()) return e;
    return entt::null;
}

// ---- 純データ検査（DeepDiagnostics へ委譲）----

void T_DeepShaders(ImGuiTestContext* ctx)
{
    Step(ctx, "全シェーダー(.cso)の存在・破損・鮮度を検査");
    ReportDeep(ctx, DeepDiag::Shaders());
}

void T_DeepTextures(ImGuiTestContext* ctx)
{
    Step(ctx, "assets 配下の全画像を読み込み検証（色空間込み）");
    ReportDeep(ctx, DeepDiag::Textures());
}

void T_DeepModels(ImGuiTestContext* ctx)
{
    Step(ctx, "assets 配下の全モデルを読み込み検証");
    ReportDeep(ctx, DeepDiag::Models());
}

void T_DeepGammaConfig(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Step(ctx, "表示パイプラインのフォーマット構成を検査");
    ReportDeep(ctx, DeepDiag::Gamma(*g_app));
}

void T_DeepSceneAssets(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Step(ctx, "シーンが参照しているアセットが描画対象になっているか検査");
    ReportDeep(ctx, DeepDiag::SceneAssets(*g_app));
}

void T_DeepLighting(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Step(ctx, "灯数の上限 / 影スロット / 消灯したライト / IBL 構成を検査");
    ReportDeep(ctx, DeepDiag::Lighting(*g_app));
}

void T_DeepTerrain(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Step(ctx, "地形の .hf 整合・コライダー・ハイトマップ共有を検査");
    ReportDeep(ctx, DeepDiag::Terrain(*g_app));
}

void T_DeepPicking(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Step(ctx, "三角形精密ピッキングが破綻する条件を検査");
    ReportDeep(ctx, DeepDiag::Picking(*g_app));
}

void T_DeepInstancing(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    // 描画リストは Render() の先頭で組み直される。読む前に数フレーム回して最新にする。
    ctx->Yield(6);
    Step(ctx, "自動インスタンシングの適格率と不適格理由を集計");
    ReportDeep(ctx, DeepDiag::Instancing(*g_app));
}

void T_DeepScripts(ImGuiTestContext* ctx)
{
    Step(ctx, "assets / scripts の全 .lua を構文スキャン");
    ReportDeep(ctx, DeepDiag::Scripts());
}

// ---- GPU が絡む検査 ----

// 「エディタは動いているのに絵が出ていない」をピクセルで確かめる。
// UI 自動テストは絵を一切見ないので、描画パスが丸ごと死んでいても全部緑になりうる。
void T_DeepRenderProof(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);

    Step(ctx, "シーンビューの絵を読み戻す");
    const Application::DiagFrameStats base = GrabFrame(ctx);
    IM_CHECK(base.valid);
    IM_CHECK(base.width > 0 && base.height > 0);
    ctx->LogInfo("シーン RT %ux%u / 平均輝度 %.3f / 非黒 %.1f%%",
                 base.width, base.height, base.meanLuma, base.nonBlack * 100.0f);

    if (base.nonBlack < 0.01f)
        IM_ERRORF("シーンビューがほぼ真っ黒（非黒ピクセル %.2f%%）。描画パスが動いていない",
                  base.nonBlack * 100.0f);

    // 投影方式を変えれば絵は必ず変わる。変わらない＝シーンビューが再描画されていない
    // （＝表示が固まっていて、以降の検査も全部当てにならない）。
    Step(ctx, "投影を切り替えて絵が変わるか確認");
    const bool saved2D = ed->view2D;
    ed->view2D = !saved2D;
    ctx->Yield(10);
    const Application::DiagFrameStats flipped = GrabFrame(ctx);
    ed->view2D = saved2D;
    ctx->Yield(10);

    IM_CHECK_NO_RET(flipped.valid);
    if (flipped.valid && flipped.hash == base.hash)
        IM_ERRORF("投影を 2D↔3D で切り替えても絵が 1 ピクセルも変わらない。"
                  "シーンビューが再描画されていない");
}

// スクリーンショットの表示変換がシーンのトーンマップ設定に追従しているか。
// シーン RT はポスト処理前のリニア HDR なので、読み戻し側が設定を見ずに決め打ちすると
// 「ビューポートと違う色の PNG」が出てきて、色やコントラストの判断を丸ごと誤らせる。
// 参照されなくなったシーン所有メッシュが回収されるか。
// 以前はシーンを開き直すまで一切解放されず、生成→削除を繰り返すぶんだけ積み上がった
// （箱なら数 KB だが 512² の地形は 50MB 級）。回収はフレーム末尾で 60 フレームに 1 回走る。
// プレハブの「適用」が、彫った形（.smsh）を他インスタンスへ配るか。
// 3-way マージはパス文字列しか見ず、インスタンスは私物のコピーを持つ（＝パスが必ず違う）ため、
// 対策が無いと形状の差が常に「インスタンス側の手直し」と判定されて配られない。
// なのにログと UI は「他 N インスタンスへ反映」と言う、という嘘が出る場所。
void T_PrefabGeometryPropagate(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;
    IM_CHECK(g_app != nullptr);
    Scene* scene = g_app->GetScene();
    IM_CHECK(scene != nullptr);
    auto& reg = scene->GetRegistry();
    const std::string assets = PathResolver::AssetsDir();
    const std::string prefabRel = "prefabs/__uitest_rock.prefab";

    auto readBytes = [](const std::string& abs) -> std::string
    {
        std::ifstream ifs(abs, std::ios::binary);
        if (!ifs) return {};
        return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    };
    auto carve = [&](entt::entity e, float x)
    {
        SculptMesh* sc = reg.try_get<SculptMesh>(e);
        if (!sc || !sc->_data) return false;
        SculptBrushParams bp;
        bp.type = SculptBrushType::Draw;
        bp.radius = 1.5f; bp.strength = 1.0f;
        bp.direction = {0.0f, 1.0f, 0.0f};
        const size_t moved = sc->_data->ApplyBrush(bp, {x, 0.0f, 0.0f}, 1.0f, false, nullptr);
        if (moved == 0) return false;
        // .smsh を書き出す（パネルの自動保存に頼らず、この場で確定させる）
        if (sc->meshPath.empty()) sc->meshPath = sculpt::MakeSculptMeshRelPath(
            reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name : std::string("S"));
        fs::create_directories(fs::path(assets + sc->meshPath).parent_path());
        return sculpt::SaveSculptMeshFile(assets + sc->meshPath, *sc->_data);
    };

    Step(ctx, "素体を作ってプレハブ化 → 2 個生成");
    SculptMesh params;
    Entity src = scene->SpawnSculpt("__uitest_rock", {0.0f, 60.0f, 0.0f}, params);
    IM_CHECK(reg.valid(src.GetHandle()));
    IM_CHECK(carve(src.GetHandle(), 0.0f));   // プレハブ化前に一度彫って .smsh を確定させる
    IM_CHECK(SceneSerializer::SavePrefab(*scene, src.GetHandle(), assets + prefabRel, assets));

    std::vector<entt::entity> allA, allB;
    entt::entity a = SceneSerializer::InstantiatePrefab(*scene, assets + prefabRel, assets, &allA);
    entt::entity b = SceneSerializer::InstantiatePrefab(*scene, assets + prefabRel, assets, &allB);
    IM_CHECK(a != entt::null && b != entt::null);

    const SculptMesh* scA = reg.try_get<SculptMesh>(a);
    const SculptMesh* scB = reg.try_get<SculptMesh>(b);
    IM_CHECK(scA != nullptr && scB != nullptr);
    // インスタンスは私物のコピーを持つ（同じファイルを共有していたら以降の検査が無意味）
    IM_CHECK_STR_NE(scA->meshPath.c_str(), scB->meshPath.c_str());

    Step(ctx, "A を彫って「適用」→ 未編集の B へ形が配られる");
    IM_CHECK(carve(a, 0.5f));
    const std::string aBytes = readBytes(assets + scA->meshPath);
    IM_CHECK(!aBytes.empty());

    int propagated = 0;
    IM_CHECK(SceneSerializer::ApplyPrefabInstance(*scene, a, assets, &propagated));
    ctx->Yield(3);

    const std::string bBytes = readBytes(assets + scB->meshPath);
    if (bBytes != aBytes)
        IM_ERRORF("彫った形が他インスタンスへ配られていない（A %d バイト / B %d バイト）",
                  static_cast<int>(aBytes.size()), static_cast<int>(bBytes.size()));

    Step(ctx, "個別に彫ったインスタンスは据え置き（手直しを潰さない）");
    // b を作り直しているので取り直す
    entt::entity b2 = entt::null;
    for (auto [e, link] : reg.view<const PrefabLink>().each())
        if (link.sourcePath == prefabRel && e != a) { b2 = e; break; }
    IM_CHECK(b2 != entt::null);
    IM_CHECK(carve(b2, -0.8f));
    const SculptMesh* scB2 = reg.try_get<SculptMesh>(b2);
    IM_CHECK(scB2 != nullptr);
    const std::string b2Own = readBytes(assets + scB2->meshPath);

    IM_CHECK(carve(a, 1.2f));
    IM_CHECK(SceneSerializer::ApplyPrefabInstance(*scene, a, assets, &propagated));
    ctx->Yield(3);
    if (readBytes(assets + scB2->meshPath) != b2Own)
        IM_ERRORF("個別に彫ったインスタンスの形が上書きされた（手直しが消える）");

    // 後片付け（assets を汚さない）
    std::error_code ec;
    std::vector<entt::entity> kill;
    for (auto [e, link] : reg.view<const PrefabLink>().each())
        if (link.sourcePath == prefabRel) kill.push_back(e);
    kill.push_back(src.GetHandle());
    for (entt::entity e : kill) if (reg.valid(e)) scene->Remove(Entity(e, &reg));
    fs::remove(fs::path(assets + prefabRel), ec);
    for (const auto& de : fs::directory_iterator(fs::path(assets + "sculpt"), ec))
    {
        const std::string fn = de.path().filename().string();
        if (fn.rfind("__uitest_rock", 0) == 0 || fn.rfind("__prefab___uitest_rock", 0) == 0)
            fs::remove(de.path(), ec);
    }
}

// 親の下に置いた剛体の当たり判定が、描画位置（ワールド）に作られるか。
// 物理は長らく Transform.parent を無視してローカル値でボディを作っていたので、
// ★見えている箱と当たる箱が別の場所にあった（グループ化して動かすと必ず踏む）。
void T_PhysicsParentedBodyWorldSpace(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Scene* scene = g_app->GetScene();
    IM_CHECK(scene != nullptr);
    auto& reg = scene->GetRegistry();

    // ---- 床（静的）+ 親 (30,0,0) + 子（ローカル (0,6,0) の動的剛体）----
    Entity floorE = scene->SpawnBox("__uitest_floor", {0.0f, -60.5f, 0.0f});
    {
        auto& t = reg.get<Transform>(floorE.GetHandle());
        t.scale = {80.0f, 1.0f, 80.0f};
        reg.emplace_or_replace<RigidBody>(floorE.GetHandle(),
            RigidBody{ .motionType = MotionType::Static });
        BoxCollider bc; bc.halfExtents = {0.5f, 0.5f, 0.5f};
        reg.emplace_or_replace<BoxCollider>(floorE.GetHandle(), bc);
    }
    Entity parentE = scene->SpawnBox("__uitest_pparent", {30.0f, -60.0f, 0.0f});
    Entity childE  = scene->SpawnBox("__uitest_pchild", {0.0f, 6.0f, 0.0f});
    {
        auto& t = reg.get<Transform>(childE.GetHandle());
        t.parent = parentE.GetHandle();
        reg.emplace_or_replace<RigidBody>(childE.GetHandle(),
            RigidBody{ .motionType = MotionType::Dynamic });
        BoxCollider bc; bc.halfExtents = {0.5f, 0.5f, 0.5f};
        reg.emplace_or_replace<BoxCollider>(childE.GetHandle(), bc);
    }

    Step(ctx, "Play へ入って落ち着くまで回す");
    g_app->RequestMode(Application::EngineMode::Playing);
    int frames = 0;
    while (g_app->GetEngineMode() != Application::EngineMode::Playing && frames < 240)
    { ctx->Yield(); ++frames; }
    if (g_app->GetEngineMode() != Application::EngineMode::Playing)
    {
        ctx->LogWarning("Play に入れませんでした（アクティブなカメラが無い？）");
        return;   // 後片付けは Stop 側のスナップショット復元に任せる
    }
    ctx->Yield(150);   // 6m 落ちて静止するまで

    PhysicsSystem* phys = g_app->GetPhysicsSystem();
    IM_CHECK(phys != nullptr);
    if (phys)
    {
        auto countAt = [&](float x, float y, float z) {
            entt::entity buf[16];
            const size_t n = phys->OverlapSphere(DirectX::XMFLOAT3{x, y, z}, 1.0f, buf, 16);
            int hits = 0;
            for (size_t i = 0; i < n; ++i)
            {
                if (!reg.valid(buf[i]) || !reg.all_of<NameTag>(buf[i])) continue;
                if (reg.get<NameTag>(buf[i]).name == "__uitest_pchild") ++hits;
            }
            return hits;
        };
        // 描画される場所 = 親(30,-60,0) + ローカル(0,0.5,0)
        const int atWorld = countAt(30.0f, -59.5f, 0.0f);
        // 以前ボディが作られていた場所 = ローカル値そのまま
        const int atLocal = countAt(0.0f, 0.5f, 0.0f);
        ctx->LogInfo("parented body hits: world=%d local=%d", atWorld, atLocal);
        if (atWorld != 1)
            IM_ERRORF("親の下の剛体の当たり判定が描画位置(30,-59.5,0)に無い（親のローカル値で作られている）");
        if (atLocal != 0)
            IM_ERRORF("親の下の剛体の当たり判定がローカル座標(0,0.5,0)に残っている");
    }

    Step(ctx, "Stop してエディタへ戻す");
    g_app->RequestMode(Application::EngineMode::Editor);
    frames = 0;
    while (g_app->GetEngineMode() != Application::EngineMode::Editor && frames < 240)
    { ctx->Yield(); ++frames; }
    ctx->Yield(4);

    // 後片付け（Stop でシーンが作り直されるので名前で引き直す）
    for (const char* nm : {"__uitest_pchild", "__uitest_pparent", "__uitest_floor"})
    {
        Entity e = scene->FindEntity(nm);
        if (e.IsValid()) scene->Remove(e);
    }
    ctx->Yield(2);
}

// 親と子を「子を先に」選んで削除 → Undo で、子が親の下に戻るか。
// ★以前は祖先も削除対象に入っている子を除外していなかったので、
//   「子だけの削除コマンド」と「親の削除コマンド」が別々に積まれ、Undo は逆順なので
//   親が先に**新しいハンドル**で復元される。その後に子のコマンドが走ると
//   externalParent が無効（entt は destroy で version を進める）になり、
//   子が**シーンのルートとして**復元されて親から外れる。
//   しかも Ctrl+クリックの順番次第で出たり出なかったりした。
void T_DeleteUndoKeepsParent(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Scene* scene = g_app->GetScene();
    EditorContext* ed = Ed();
    IM_CHECK(scene != nullptr && ed != nullptr);
    auto& reg = scene->GetRegistry();

    Step(ctx, "親と子を作る");
    Entity parent = scene->SpawnBox("__uitest_delp", {0.0f, 40.0f, 0.0f});
    Entity child  = scene->SpawnBox("__uitest_delc", {1.0f,  0.0f, 0.0f});
    reg.get<Transform>(child.GetHandle()).parent = parent.GetHandle();
    ctx->Yield(3);

    // 削除前の guid を控える（復元で保たれるか見る）
    auto guidOf = [&](entt::entity ent) -> uint64_t {
        const auto* g = reg.try_get<EntityGuid>(ent);
        return g ? g->value : 0ull;
    };
    if (guidOf(parent.GetHandle()) == 0)
        reg.emplace_or_replace<EntityGuid>(parent.GetHandle(), EntityGuid{ 0xA11CE0001ull });
    if (guidOf(child.GetHandle()) == 0)
        reg.emplace_or_replace<EntityGuid>(child.GetHandle(), EntityGuid{ 0xA11CE0002ull });
    const uint64_t savedParentGuid = guidOf(parent.GetHandle());
    const uint64_t savedChildGuid  = guidOf(child.GetHandle());

    Step(ctx, "★子を先に、次に親を削除キューへ入れる（この順番が壊れていた）");
    ed->ClearSelection();
    ed->pendingDeletions.clear();
    ed->pendingDeletions.push_back(child.GetHandle());
    ed->pendingDeletions.push_back(parent.GetHandle());
    ctx->Yield(6);
    if (reg.valid(parent.GetHandle()) || reg.valid(child.GetHandle()))
    { IM_ERRORF("削除が反映されていない"); return; }

    Step(ctx, "Undo して親子関係が戻るか見る");
    ed->pendingUndo = true;
    ctx->Yield(8);

    Entity p2 = scene->FindEntity("__uitest_delp");
    Entity c2 = scene->FindEntity("__uitest_delc");
    if (!p2.IsValid() || !c2.IsValid())
    { IM_ERRORF("Undo で復元されていない（親=%d 子=%d）", p2.IsValid(), c2.IsValid()); }
    else
    {
        const entt::entity restoredParent = reg.get<Transform>(c2.GetHandle()).parent;
        if (restoredParent != p2.GetHandle())
            IM_ERRORF("Undo 後、子が親の下に戻っていない（シーンのルートへ外れた）");
    }

    // ★guid も引き継がれているか。落とすと、このエンティティを guid で指していた参照
    //   （Trigger の相手 / Lua の entity プロパティ / NetworkIdentity）が全部ぶら下がりになり、
    //   名前一致へ静かに格下げされる（同名が居ると誤爆する）。
    if (p2.IsValid() && c2.IsValid())
    {
        const auto* gp = reg.try_get<EntityGuid>(p2.GetHandle());
        const auto* gc = reg.try_get<EntityGuid>(c2.GetHandle());
        if (!gp || gp->value == 0 || !gc || gc->value == 0)
            IM_ERRORF("Undo で復元したエンティティが EntityGuid を失っている（親=%llu 子=%llu）",
                      gp ? gp->value : 0ull, gc ? gc->value : 0ull);
        else if (gp->value != savedParentGuid || gc->value != savedChildGuid)
            IM_ERRORF("Undo で復元したエンティティの guid が変わっている");
    }

    // 後片付け
    for (const char* nm : {"__uitest_delc", "__uitest_delp"})
    {
        Entity e = scene->FindEntity(nm);
        if (e.IsValid()) scene->Remove(e);
    }
    ctx->Yield(2);
}

void T_MeshGarbageCollect(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Scene* scene = g_app->GetScene();
    IM_CHECK(scene != nullptr);

    ctx->Yield(70);   // 事前に 1 回回してから基準を取る（前のテストの残骸を落とす）
    const size_t baseline = scene->GetOwnedMeshCount();

    constexpr int kRounds = 3;
    constexpr int kPerRound = 4;
    for (int r = 0; r < kRounds; ++r)
    {
        Step(ctx, "生成→削除 %d 巡目", r + 1);
        std::vector<Entity> made;
        for (int i = 0; i < kPerRound; ++i)
        {
            char nm[64];
            std::snprintf(nm, sizeof(nm), "__gc_%d_%d", r, i);
            made.push_back((i % 2 == 0) ? scene->SpawnBox(nm, {static_cast<f32>(i) * 2.0f, 50.0f, 0.0f})
                                        : scene->SpawnSphere(nm, {static_cast<f32>(i) * 2.0f, 50.0f, 2.0f}));
        }
        ctx->Yield(3);
        for (Entity& e : made) scene->Remove(e);
        ctx->Yield(70);   // 回収は 60 フレームに 1 回
    }

    const size_t after = scene->GetOwnedMeshCount();
    ctx->LogInfo("owned meshes: baseline=%d after %d rounds=%d",
                 static_cast<int>(baseline), kRounds, static_cast<int>(after));
    // 回収が効いていなければ kRounds*kPerRound = 12 個ぶん増えている。
    // ぴったり baseline に戻ることまでは要求しない（共有グローメッシュ等が増える経路がある）。
    if (after > baseline + 2)
        IM_ERRORF("未参照メッシュが回収されていない: %d -> %d（%d 個増えた）",
                  static_cast<int>(baseline), static_cast<int>(after),
                  static_cast<int>(after - baseline));
}

void T_DeepGammaRoundTrip(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Scene* scene = g_app->GetScene();
    IM_CHECK(scene != nullptr);

    PostProcessSettings& pp = scene->GetPostSettings();
    const int savedTone = pp.tonemapper;

    Step(ctx, "トーンマップ「なし(ガンマのみ)」で読み戻す");
    pp.tonemapper = 2;
    ctx->Yield(6);
    const Application::DiagFrameStats gammaOnly = GrabFrame(ctx);

    Step(ctx, "トーンマップ「ACES」で読み戻す");
    pp.tonemapper = 0;
    ctx->Yield(6);
    const Application::DiagFrameStats acesTone = GrabFrame(ctx);

    pp.tonemapper = savedTone;
    ctx->Yield(4);

    IM_CHECK(gammaOnly.valid && acesTone.valid);
    // ACES は肩が寝ているぶん必ず暗くなる。真っ黒な画では差が出ないので前提も確認する。
    if (gammaOnly.meanLuma < 0.01f)
    {
        ctx->LogWarning("画が暗すぎて表示変換の差を判定できない（平均輝度 %.4f）", gammaOnly.meanLuma);
        return;
    }
    ctx->LogInfo("平均輝度: ガンマのみ %.3f / ACES %.3f", gammaOnly.meanLuma, acesTone.meanLuma);
    if (std::fabs(gammaOnly.meanLuma - acesTone.meanLuma) < 0.005f)
        IM_ERRORF("トーンマップを変えても読み戻した絵が変わらない。"
                  "スクリーンショットがシーン設定を無視している（ビューポートと別の絵になる）");
}

// ギズモが「触っていないのに値を書き換える」「NaN を出す」「掴んだ状態のまま固まる」を検査する。
// 親が回転＋非一様スケールを持つ子が一番壊れやすいので、その形で全モードを通す。
void T_DeepGizmo(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    Scene* scene = g_app->GetScene();
    IM_CHECK(scene != nullptr);
    entt::registry& reg = scene->GetRegistry();

    Step(ctx, "親(回転+非一様スケール)と子を作る");
    const entt::entity parent = AddBoxAndGet(ctx, reg);
    const entt::entity child  = AddBoxAndGet(ctx, reg);
    IM_CHECK(parent != entt::null && child != entt::null);
    IM_CHECK(reg.valid(parent) && reg.valid(child));

    {
        Transform& tp = reg.get<Transform>(parent);
        tp.position = {  2.0f, 0.5f, -1.0f };
        tp.rotation = { 23.0f, 41.0f, 17.0f };
        tp.scale    = {  1.7f, 0.6f,  2.3f };

        Transform& tc = reg.get<Transform>(child);
        tc.parent   = parent;
        tc.position = { 1.0f, 2.0f, -0.5f };
    }

    ed->selectedEntity = child;
    ed->selectedEntities.assign(1, child);
    ctx->Yield(6);

    const Transform expected = reg.get<Transform>(child);

    const GizmoMode modes[3]  = { GizmoMode::Translate, GizmoMode::Rotate, GizmoMode::Scale };
    const char*     names[3]  = { "移動", "回転", "スケール" };
    const bool      saved2D   = ed->view2D;
    const bool      savedLoc  = ed->gizmoLocalSpace;
    const GizmoMode savedMode = ed->gizmoMode;

    for (int v2 = 0; v2 < 2; ++v2)
        for (int loc = 0; loc < 2; ++loc)
            for (int m = 0; m < 3; ++m)
            {
                ed->view2D          = (v2 != 0);
                ed->gizmoLocalSpace = (loc != 0);
                ed->gizmoMode       = modes[m];
                ctx->Yield(4);

                if (!reg.valid(child)) { IM_ERRORF("子エンティティが消えた"); break; }
                const Transform& t = reg.get<Transform>(child);

                if (!TransformFinite(t))
                {
                    IM_ERRORF("%s / %s / %s でギズモが Transform を NaN にした",
                              names[m], (loc != 0) ? "ローカル" : "ワールド",
                              (v2 != 0) ? "2Dビュー" : "3Dビュー");
                    break;
                }
                // ドラッグしていないのに値が変わったら、描画のたびに物が動く＝致命的
                if (!TransformEqual(t, expected))
                {
                    IM_ERRORF("%s / %s / %s で、掴んでいないのに Transform が書き換わった",
                              names[m], (loc != 0) ? "ローカル" : "ワールド",
                              (v2 != 0) ? "2Dビュー" : "3Dビュー");
                    break;
                }
                // 掴んだままの状態が残ると、以降クリックでの選択が全部吸われる
                IM_CHECK_NO_RET(!ImGuizmo::IsUsing());
            }

    // ---- マルチ選択（ギズモ大改修で入った仮想ピボット経路）----
    // 2 体以上選ぶと SceneViewPanel は「選択群の中心に置いた平行移動だけの行列」を
    // ImGuizmo へ渡し、返ってきた行列との差分を各選択のワールドへ掛ける（回転/拡縮も群に効く）。
    // 掴んでいない間は、この経路でも 1 ミリも動いてはいけない
    // （毎フレーム差分を掛け続けると選択しただけで物が流れていく）。
    Step(ctx, "マルチ選択（親＋子）で全モードを通す");
    ed->view2D = saved2D;
    if (reg.valid(parent) && reg.valid(child))
    {
        const Transform parentExpected = reg.get<Transform>(parent);
        ed->selectedEntity = parent;
        ed->selectedEntities.assign({ parent, child });
        ctx->Yield(6);

        for (int loc = 0; loc < 2; ++loc)
            for (int m = 0; m < 3; ++m)
            {
                ed->gizmoLocalSpace = (loc != 0);
                ed->gizmoMode       = modes[m];
                ctx->Yield(4);

                if (!reg.valid(parent) || !reg.valid(child))
                {
                    IM_ERRORF("マルチ選択中にエンティティが消えた");
                    break;
                }
                const Transform& tp = reg.get<Transform>(parent);
                const Transform& tc = reg.get<Transform>(child);
                if (!TransformFinite(tp) || !TransformFinite(tc))
                {
                    IM_ERRORF("マルチ選択の %s で Transform が NaN になった", names[m]);
                    break;
                }
                if (!TransformEqual(tp, parentExpected) || !TransformEqual(tc, expected))
                {
                    IM_ERRORF("マルチ選択の %s で、掴んでいないのに Transform が書き換わった"
                              "（仮想ピボットの差分が毎フレーム掛かっている）", names[m]);
                    break;
                }
                IM_CHECK_NO_RET(!ImGuizmo::IsUsing());
            }
    }

    // ---- 常時スナップ（スナップ量が EditorContext へ外出しされた）----
    // snapAlways=true は Ctrl を押していなくてもスナップ値を ImGuizmo へ渡す経路。
    // ここで「掴んでいないのにスナップ位置へ吸われる」と、選択しただけで物が動く。
    Step(ctx, "常時スナップ ON で全モードを通す");
    const bool savedSnapAlways = ed->snapAlways;
    ed->snapAlways     = true;
    ed->selectedEntity = child;
    ed->selectedEntities.assign(1, child);
    ctx->Yield(4);
    for (int m = 0; m < 3; ++m)
    {
        ed->gizmoMode = modes[m];
        ctx->Yield(4);
        if (!reg.valid(child)) break;
        const Transform& t = reg.get<Transform>(child);
        if (!TransformFinite(t) || !TransformEqual(t, expected))
        {
            IM_ERRORF("常時スナップ ON の %s で、掴んでいないのに Transform が書き換わった", names[m]);
            break;
        }
    }
    ed->snapAlways = savedSnapAlways;

    ed->view2D          = saved2D;
    ed->gizmoLocalSpace = savedLoc;
    ed->gizmoMode       = savedMode;
    ctx->Yield(4);

    // 親のワールド行列が有限か（非一様スケール＋回転の合成で崩れていないか）
    if (reg.valid(child))
    {
        DirectX::XMFLOAT4X4 wf;
        DirectX::XMStoreFloat4x4(&wf, ComputeWorldMatrix(reg, child));
        bool finite = true;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (!std::isfinite(wf.m[i][j])) finite = false;
        if (!finite)
            IM_ERRORF("親が回転+非一様スケールのとき、子のワールド行列が NaN になる");
    }

    // 回転ギズモは gizmoLocalSpace を無視して常にワールド軸で出る（SceneViewPanel の実装仕様）。
    // 不具合ではないが「T を押しても回転だけ切り替わらない」と見えるので記録に残す。
    ctx->LogInfo("仕様メモ: 回転ギズモはローカル/ワールド切替を無視して常にワールド軸で表示される");
    ctx->LogInfo("仕様メモ: マルチ選択中はローカル/ワールド切替も無視してワールド固定"
                 "（代表 1 個の姿勢に群全体が引っ張られるのを避けるため）");
}

// ---- コピペ / 複製 / プレハブの機能維持検査 ----

// 「親(Box+Trigger) + 子(Box+コライダー+Luaスクリプト)」というステージ制作の典型構成を作る。
// 子の名前を Trigger の target と Lua の entity プロパティが名前参照する＝複製時のリマップ検査台。
struct SubtreeFixture
{
    entt::entity parent = entt::null;
    entt::entity child  = entt::null;
    std::string  childName;
};

SubtreeFixture MakeColliderSubtree(ImGuiTestContext* ctx, entt::registry& reg)
{
    SubtreeFixture fx;
    fx.parent = AddBoxAndGet(ctx, reg);
    fx.child  = AddBoxAndGet(ctx, reg);
    if (fx.parent == entt::null || fx.child == entt::null) return fx;

    // 親子に一意な名前を付ける。生成直後は両方とも同じ名前になり得て、同名だと
    // 名前参照が元から曖昧（エンジンの名前解決は任意の一致を拾う）＝リマップ検査にならない。
    static int s_diagSeq = 0;
    const std::string suffix = std::to_string(++s_diagSeq);
    reg.get<NameTag>(fx.parent).name = "DiagParent_" + suffix;
    reg.get<NameTag>(fx.child).name  = "DiagChild_" + suffix;

    reg.get<Transform>(fx.child).parent = fx.parent;
    fx.childName = reg.get<NameTag>(fx.child).name;

    // 子: 当たり判定 + Lua スクリプト（entity プロパティで自分の兄弟=子自身を名前参照）
    reg.emplace_or_replace<BoxCollider>(fx.child, BoxCollider{});
    LuaScript ls;
    ls.scriptPath = "scripts/__diag_copy_test.lua";   // データ複製の検査なので実在は不要
    ScriptProp p;
    p.name = "targetRef";
    p.type = ScriptPropType::Entity;
    p.str  = fx.childName;
    ls.props.push_back(std::move(p));
    reg.emplace_or_replace<LuaScript>(fx.child, std::move(ls));

    // 親: Trigger の filter / actions[].target も子を名前参照
    Trigger tr;
    tr.filter = fx.childName;
    TriggerAction act;
    act.target = fx.childName;
    tr.actions.push_back(std::move(act));
    reg.emplace_or_replace<Trigger>(fx.parent, std::move(tr));
    return fx;
}

// 生成済み集合 before に対する「新入りのうち親を持たないルート」を返す
entt::entity FindNewRoot(entt::registry& reg, const std::set<entt::entity>& before)
{
    for (auto e : reg.view<Transform>())
        if (before.find(e) == before.end() && reg.get<Transform>(e).parent == entt::null)
            return e;
    return entt::null;
}

entt::entity FindChildOf(entt::registry& reg, entt::entity parent,
                         const std::set<entt::entity>& before)
{
    for (auto e : reg.view<Transform>())
        if (before.find(e) == before.end() && reg.get<Transform>(e).parent == parent)
            return e;
    return entt::null;
}

// コピペした複製が「見た目だけの箱」になっていないかを実データで検査する。
// 子階層・コライダー・Lua スクリプト(プロパティ込み)・Trigger が全部残り、
// 名前参照は複製後の名前へ付け替わっていて、Undo/Redo で往復できること。
void T_DeepCopyPasteSubtree(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    Scene* scene = g_app->GetScene();
    IM_CHECK(scene != nullptr);
    entt::registry& reg = scene->GetRegistry();

    Step(ctx, "親(Trigger) + 子(コライダー+Lua) を作る");
    SubtreeFixture fx = MakeColliderSubtree(ctx, reg);
    IM_CHECK(fx.parent != entt::null && fx.child != entt::null);

    std::set<entt::entity> before;
    for (auto e : reg.view<Transform>()) before.insert(e);

    Step(ctx, "編集メニューでコピー → 貼り付け");
    ed->selectedEntity = fx.parent;
    ed->selectedEntities.assign(1, fx.parent);
    ctx->Yield(2);
    ctx->SetRef(kWinToolbar);
    ctx->MenuClick("編集/コピー");
    ctx->Yield(2);
    ctx->MenuClick("編集/貼り付け");
    ctx->Yield(8);   // ペーストはフレーム境界の遅延処理

    const entt::entity pastedRoot = FindNewRoot(reg, before);
    IM_CHECK(pastedRoot != entt::null);
    const entt::entity pastedChild = FindChildOf(reg, pastedRoot, before);
    if (pastedChild == entt::null)
    {
        IM_ERRORF("ペーストで子エンティティが複製されていない（サブツリーが失われた）");
        return;
    }

    Step(ctx, "複製先のコンポーネントを検査");
    if (!reg.all_of<BoxCollider>(pastedChild))
        IM_ERRORF("ペーストした子に BoxCollider が引き継がれていない");
    if (!reg.all_of<LuaScript>(pastedChild))
        IM_ERRORF("ペーストした子に Lua スクリプトが引き継がれていない");
    if (!reg.all_of<Trigger>(pastedRoot))
        IM_ERRORF("ペーストした親に Trigger が引き継がれていない");

    // 名前参照のリマップ（複製セット内の参照は新しい名前を指すこと）
    const std::string newChildName = reg.get<NameTag>(pastedChild).name;
    IM_CHECK_NO_RET(newChildName != fx.childName);   // 連番でリネームされている前提
    if (reg.all_of<LuaScript>(pastedChild))
    {
        const auto& ls = reg.get<LuaScript>(pastedChild);
        IM_CHECK_NO_RET(!ls.props.empty());
        if (!ls.props.empty() && ls.props[0].str != newChildName)
            IM_ERRORF("Lua の entity プロパティが複製後の名前へリマップされていない"
                      "（%s のままで参照切れ）", ls.props[0].str.c_str());
    }
    if (reg.all_of<Trigger>(pastedRoot))
    {
        const auto& tr = reg.get<Trigger>(pastedRoot);
        if (tr.filter != newChildName)
            IM_ERRORF("Trigger.filter が複製後の名前へリマップされていない（%s）", tr.filter.c_str());
        if (!tr.actions.empty() && tr.actions[0].target != newChildName)
            IM_ERRORF("Trigger.actions[].target がリマップされていない（%s）",
                      tr.actions[0].target.c_str());
    }

    Step(ctx, "Undo で親子ごと消え、Redo で親子ごと戻るか");
    ed->pendingUndo = true;
    ctx->Yield(6);
    if (reg.valid(pastedRoot) || reg.valid(pastedChild))
        IM_ERRORF("ペーストの Undo でサブツリーが消えていない");
    ed->pendingRedo = true;
    ctx->Yield(8);
    const entt::entity redoRoot = FindNewRoot(reg, before);
    if (redoRoot == entt::null || FindChildOf(reg, redoRoot, before) == entt::null)
        IM_ERRORF("ペーストの Redo で親子が復元されない");
}

// 複製(Ctrl+D 相当)の子孫維持と、親子両方選択時の二重複製防止。
void T_DeepDuplicateSubtree(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    Scene* scene = g_app->GetScene();
    IM_CHECK(scene != nullptr);
    entt::registry& reg = scene->GetRegistry();

    Step(ctx, "親 + 子(コライダー) を作って両方選択");
    SubtreeFixture fx = MakeColliderSubtree(ctx, reg);
    IM_CHECK(fx.parent != entt::null && fx.child != entt::null);

    std::set<entt::entity> before;
    for (auto e : reg.view<Transform>()) before.insert(e);

    // 親子「両方」を選択して複製 → 正しければ新規は 2 体（root+child）だけ。
    // 子が個別にもう 1 回複製されると 3 体になる＝二重複製バグ。
    ed->selectedEntity = fx.parent;
    ed->selectedEntities.assign({ fx.parent, fx.child });
    ctx->Yield(2);
    Step(ctx, "編集メニューで複製");
    ctx->SetRef(kWinToolbar);
    ctx->MenuClick("編集/複製");
    ctx->Yield(8);

    int newCount = 0;
    for (auto e : reg.view<Transform>())
        if (before.find(e) == before.end()) ++newCount;

    if (newCount < 2)
        IM_ERRORF("複製で子が複製されていない（新規 %d 体、期待 2 体）", newCount);
    else if (newCount > 2)
        IM_ERRORF("親子両方選択の複製で子が二重複製された（新規 %d 体、期待 2 体）", newCount);

    const entt::entity dupRoot = FindNewRoot(reg, before);
    IM_CHECK(dupRoot != entt::null);
    const entt::entity dupChild = FindChildOf(reg, dupRoot, before);
    if (dupChild == entt::null || !reg.all_of<BoxCollider>(dupChild))
        IM_ERRORF("複製した子にコライダーが引き継がれていない");
}

// プリミティブと PBR の保存往復。
//
// ★これが無かったせいで 2 件のサイレントなデータ損失が生きていた:
//   - 平面の一辺・球の半径が保存されず、読み込み側のハードコード(50 / 0.5)に化ける。
//     既定シーンの Ground は 20m なので、開いて保存し直すだけで 50m へ広がっていた。
//   - metallic/roughness は Material を持つメッシュ(=モデル由来)でしか保存されず、
//     プリミティブでは dx12_set_pbr が成功を返し絵も変わるのに保存で消えていた。
// どちらも「保存した本人には見えない」形なので、JSON を直接見て固定する。
void T_DeepPrimitiveSaveRoundtrip(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Scene* scene = g_app->GetScene();
    IM_CHECK(scene != nullptr);
    entt::registry& reg = scene->GetRegistry();
    const std::string assetsDir = PathResolver::AssetsDir();

    Step(ctx, "既定と違うサイズの平面 + PBR 上書きを作る");
    const float kSide = 17.5f;   // 既定(50)とも Ground(20)とも違う値
    Entity plane = scene->SpawnPlane("__T_Plane", { 0.0f, -50.0f, 0.0f }, kSide, false);
    const entt::entity pe = plane.GetHandle();
    IM_CHECK(pe != entt::null);
    {
        auto& mr = reg.get<MeshRenderer>(pe);
        mr.overrideMetallic  = 0.91f;
        mr.overrideRoughness = 0.13f;
        IM_CHECK(!mr.meshes.empty() && mr.meshes[0] != nullptr);
        // 前提の確認: プリミティブは Material を持たない(SetMaterial は ModelLoader だけが呼ぶ)。
        // ここが将来変わったら、下の material 検査は別の理由で通ってしまう。
        if (mr.meshes[0]->GetMaterial() != nullptr)
            ctx->LogWarning("プリミティブが Material を持つようになった。この検査の前提を見直すこと");
    }

    Step(ctx, "JSON へ書き出して、消えがちなキーが載っているか見る");
    const std::string js = SceneSerializer::SerializeEntity(*scene, pe, assetsDir);
    if (js.find("\"primitiveSize\"") == std::string::npos)
        IM_ERRORF("平面の一辺が保存されていない(読み込みで既定 50 に化ける)");
    if (js.find("\"material\"") == std::string::npos)
        IM_ERRORF("プリミティブの metallic/roughness が保存されていない(絵は変わるのに保存で消える)");

    Step(ctx, "読み戻して形と値が一致するか");
    const entt::entity pe2 = SceneSerializer::InstantiateEntity(*scene, js, assetsDir);
    if (pe2 == entt::null)
    {
        IM_ERRORF("書き出した JSON を読み戻せない");
        reg.destroy(pe);
        return;
    }
    {
        auto& mr2 = reg.get<MeshRenderer>(pe2);
        IM_CHECK(!mr2.meshes.empty() && mr2.meshes[0] != nullptr);
        const float side2 = mr2.meshes[0]->GetAABBMax().x * 2.0f;
        if (std::fabs(side2 - kSide) > 0.01f)
            IM_ERRORF("平面の一辺が %.2f -> %.2f に化けた", kSide, side2);
        if (std::fabs(mr2.overrideMetallic - 0.91f) > 0.001f
            || std::fabs(mr2.overrideRoughness - 0.13f) > 0.001f)
            IM_ERRORF("PBR が復元されない (metallic=%.3f roughness=%.3f)",
                      mr2.overrideMetallic, mr2.overrideRoughness);
    }

    Step(ctx, "球の半径も同じ経路を通る");
    Entity sph = scene->SpawnSphere("__T_Sphere", { 0.0f, -50.0f, 0.0f }, 2.75f);
    const entt::entity se = sph.GetHandle();
    const std::string sjs = SceneSerializer::SerializeEntity(*scene, se, assetsDir);
    const entt::entity se2 = SceneSerializer::InstantiateEntity(*scene, sjs, assetsDir);
    if (se2 != entt::null)
    {
        auto& mr3 = reg.get<MeshRenderer>(se2);
        if (!mr3.meshes.empty() && mr3.meshes[0])
        {
            // 半径は AABB から復元するので、頂点分割ぶんの誤差を許す
            const float r2 = mr3.meshes[0]->GetAABBMax().x;
            if (std::fabs(r2 - 2.75f) > 0.05f)
                IM_ERRORF("球の半径が 2.75 -> %.3f に化けた", r2);
        }
        reg.destroy(se2);
    }
    else
    {
        IM_ERRORF("球の JSON を読み戻せない");
    }

    // 後始末（検査用エンティティをシーンに残さない）
    reg.destroy(se);
    reg.destroy(pe2);
    reg.destroy(pe);
    ctx->Yield(2);
}

// 透明（アルファクリップ / アルファブレンド）の上書きが保存往復するか。
//
// ★ここが守る不変条件は 2 つある:
//   ① 上書きした値（alphaMode / alphaCutoff / opacity）が JSON に載り、読み戻して一致する。
//   ② **何も上書きしていないエンティティは material の透明キーを 1 つも書かない**。
//      書いてしまうと「既存シーンを開いて保存し直しただけで全エンティティの JSON が膨らむ」＝
//      差分が読めなくなる。既定値が「継承」であることの担保でもある。
void T_DeepAlphaSaveRoundtrip(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    Scene* scene = g_app->GetScene();
    IM_CHECK(scene != nullptr);
    entt::registry& reg = scene->GetRegistry();
    const std::string assetsDir = PathResolver::AssetsDir();

    Step(ctx, "上書きしていない板は透明キーを 1 つも書かない");
    Entity plain = scene->SpawnPlane("__T_AlphaPlain", { 0.0f, -50.0f, 0.0f }, 1.0f, false);
    const entt::entity plainE = plain.GetHandle();
    IM_CHECK(plainE != entt::null);
    {
        const std::string js = SceneSerializer::SerializeEntity(*scene, plainE, assetsDir);
        if (js.find("alphaMode") != std::string::npos
            || js.find("alphaCutoff") != std::string::npos
            || js.find("\"opacity\"") != std::string::npos)
            IM_ERRORF("上書きしていないのに透明キーが書かれている（既存シーンの差分が膨らむ）");
    }

    Step(ctx, "MASK + cutoff + opacity を上書きして書き出す");
    Entity plane = scene->SpawnPlane("__T_AlphaPlane", { 0.0f, -50.0f, 0.0f }, 1.0f, false);
    const entt::entity pe = plane.GetHandle();
    IM_CHECK(pe != entt::null);
    {
        auto& mr = reg.get<MeshRenderer>(pe);
        mr.alphaModeOverride   = 1;        // Mask
        mr.alphaCutoffOverride = 0.33f;
        mr.opacity             = 0.5f;
    }
    const std::string js = SceneSerializer::SerializeEntity(*scene, pe, assetsDir);
    if (js.find("\"alphaMode\"") == std::string::npos)
        IM_ERRORF("alphaMode が保存されていない（絵は変わるのに保存で消える）");
    if (js.find("\"alphaCutoff\"") == std::string::npos)
        IM_ERRORF("alphaCutoff が保存されていない");
    if (js.find("\"opacity\"") == std::string::npos)
        IM_ERRORF("opacity が保存されていない");

    Step(ctx, "読み戻して値が一致するか");
    const entt::entity pe2 = SceneSerializer::InstantiateEntity(*scene, js, assetsDir);
    if (pe2 == entt::null)
    {
        IM_ERRORF("透明を書いた JSON を読み戻せない");
        reg.destroy(pe);
        reg.destroy(plainE);
        return;
    }
    {
        const auto& mr2 = reg.get<MeshRenderer>(pe2);
        if (mr2.alphaModeOverride != 1
            || std::fabs(mr2.alphaCutoffOverride - 0.33f) > 0.001f
            || std::fabs(mr2.opacity - 0.5f) > 0.001f)
            IM_ERRORF("透明が復元されない (mode=%d cutoff=%.3f opacity=%.3f)",
                      mr2.alphaModeOverride, mr2.alphaCutoffOverride, mr2.opacity);
    }

    Step(ctx, "実効値の合成規則（上書き > マテリアル）");
    {
        // マテリアルが Blend でも、エンティティ側で Opaque を指定したら Opaque になること。
        Material mat;
        mat.alphaMode      = AlphaMode::Blend;
        mat.alphaCutoff    = 0.9f;
        mat.baseColorAlpha = 0.25f;
        const AlphaParams inherit = ResolveAlphaParams(&mat, -1, -1.0f, 1.0f);
        if (inherit.mode != AlphaMode::Blend || std::fabs(inherit.cutoff - 0.9f) > 0.001f
            || std::fabs(inherit.opacity - 0.25f) > 0.001f)
            IM_ERRORF("継承（-1）でマテリアルの値が出てこない");
        const AlphaParams forced = ResolveAlphaParams(&mat, 0, 0.1f, 0.5f);
        if (forced.mode != AlphaMode::Opaque || std::fabs(forced.cutoff - 0.1f) > 0.001f
            || std::fabs(forced.opacity - 0.125f) > 0.001f)
            IM_ERRORF("上書きがマテリアルより優先されていない");
    }

    Step(ctx, "インスペクターのマテリアル節が透明を描いても落ちない");
    if (EditorContext* ed = Ed())
    {
        const entt::entity prevSel = ed->selectedEntity;
        ed->Select(pe);
        ctx->Yield(3);                 // 選択の反映 → Inspector の再描画まで進める
        ctx->SetRef(kWinInspector);
        ctx->ItemOpenAll("", 2);       // マテリアル節を開いて透明の行まで描かせる
        ctx->Yield(3);
        ed->Select(prevSel);
        ctx->Yield(2);
    }

    // 後始末（検査用エンティティをシーンに残さない）
    reg.destroy(pe2);
    reg.destroy(pe);
    reg.destroy(plainE);
    ctx->Yield(2);
}

// プレハブの往復: プレハブ化 → .prefab がディスクに出る → 元がインスタンス化(PrefabLink) →
// 配置した新インスタンスにも子・コライダー・リンクが揃っているか。
void T_DeepPrefabRoundtrip(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    Scene* scene = g_app->GetScene();
    IM_CHECK(scene != nullptr);
    entt::registry& reg = scene->GetRegistry();
    namespace fs = std::filesystem;

    const fs::path prefabDir = fs::path(PathResolver::AssetsDir()) / "prefabs";
    std::set<std::string> filesBefore;
    std::error_code ec;
    if (fs::exists(prefabDir, ec))
        for (const auto& f : fs::directory_iterator(prefabDir, ec))
            filesBefore.insert(f.path().string());

    Step(ctx, "親 + 子(コライダー+Lua) を作ってプレハブ化");
    SubtreeFixture fx = MakeColliderSubtree(ctx, reg);
    IM_CHECK(fx.parent != entt::null && fx.child != entt::null);
    ed->pendingCreatePrefab = fx.parent;
    ctx->Yield(6);

    // ディスクに .prefab が出たか（後始末のため新規ファイルを控える）
    std::string createdFile;
    if (fs::exists(prefabDir, ec))
        for (const auto& f : fs::directory_iterator(prefabDir, ec))
            if (filesBefore.find(f.path().string()) == filesBefore.end()
                && f.path().extension() == ".prefab")
                createdFile = f.path().string();
    if (createdFile.empty())
    {
        IM_ERRORF("プレハブ化しても assets/prefabs に .prefab が保存されない");
        return;
    }
    ctx->LogInfo("プレハブ保存: %s", createdFile.c_str());

    // 元エンティティがインスタンスに格上げされたか（Unity 同等挙動）
    if (!reg.all_of<PrefabLink>(fx.parent))
        IM_ERRORF("プレハブ化した元エンティティに PrefabLink が付かない（Apply/Revert 不能）");

    Step(ctx, "保存した .prefab をシーンへ配置");
    std::set<entt::entity> before;
    for (auto e : reg.view<Transform>()) before.insert(e);
    PendingSpawnRequest req;
    req.modelPath = createdFile;
    req.position  = { 5.0f, 0.0f, 0.0f };
    ed->pendingSpawns.push_back(req);
    ctx->Yield(8);

    const entt::entity instRoot = FindNewRoot(reg, before);
    if (instRoot == entt::null)
    {
        IM_ERRORF("プレハブを配置してもインスタンスが生成されない");
    }
    else
    {
        if (!reg.all_of<PrefabLink>(instRoot))
            IM_ERRORF("配置したインスタンスに PrefabLink が無い（リンク切れ）");
        const Transform& t = reg.get<Transform>(instRoot);
        if (std::fabs(t.position.x - 5.0f) > 0.001f)
            IM_ERRORF("プレハブ配置位置が指定と違う（x=%.2f、期待 5.0）", t.position.x);

        const entt::entity instChild = FindChildOf(reg, instRoot, before);
        if (instChild == entt::null)
            IM_ERRORF("プレハブ配置で子エンティティが展開されない");
        else
        {
            if (!reg.all_of<BoxCollider>(instChild))
                IM_ERRORF("プレハブ配置した子にコライダーが無い");
            if (!reg.all_of<LuaScript>(instChild))
                IM_ERRORF("プレハブ配置した子に Lua スクリプトが無い");
        }
    }

    // 後始末: テストが作った .prefab を消す（シーンはハーネスが復元するがディスクは残るため）
    fs::remove(createdFile, ec);
    if (ec) ctx->LogWarning("テスト用 .prefab の削除に失敗: %s", createdFile.c_str());
}

// ---- 第2波: 操作性の基盤 ----

// ツール窓レジストリ（表示 / ツール / 窓▾ の共通の表）が一貫していること。
// ★以前は 3 つのメニューが別々の一覧を持ち、UIエディタが「窓▾」にしか無い・「すべて閉じる」が窓を
//   取りこぼす、が起きていた。表が 1 つなら「全窓が同じ操作で開閉でき、全部閉じられる」ことを保証できる。
void T_ToolRegistry(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    const cmd::Env env{g_app ? g_app->GetScene() : nullptr, PathResolver::AssetsDir()};

    Step(ctx, "レジストリの整合（id / 開閉フラグの重複無し・ドック先の窓名）");
    std::set<std::string> ids;
    std::vector<bool EditorContext::*> flags;   // メンバポインタは < で比べられないので set にしない
    std::string bad;
    for (const tools::Desc& d : tools::kAll)
    {
        if (!d.id || !d.id[0] || !d.title || !d.title[0] || !d.icon || !d.category || !d.flag)
            bad += std::string("\n  ・空の項目: ") + (d.id ? d.id : "(null)");
        if (!ids.insert(d.id).second)   bad += std::string("\n  ・id 重複: ") + d.id;
        if (std::find(flags.begin(), flags.end(), d.flag) != flags.end()) bad += std::string("\n  ・フラグ重複: ") + d.id;
        flags.push_back(d.flag);
        if (d.slot == tools::DockSlot::RightTab && (!d.imguiName || !d.imguiName[0]))
            bad += std::string("\n  ・右タブ窓なのに窓名が無い: ") + d.id;
    }
    if (!bad.empty()) IM_ERRORF("ツール窓レジストリに不整合があります:%s", bad.c_str());

    // 診断パネル自身は検査中に閉じない（開閉を試すのは別の窓だけ）
    const bool wasDiag = ed->showEngineDiagnostics;

    Step(ctx, "全窓を window.<id> コマンドで開閉（トグル）");
    for (const tools::Desc& d : tools::kAll)
    {
        if (d.flag == &EditorContext::showEngineDiagnostics) continue;
        const bool start = ed->*(d.flag);
        cmd::Execute(*ed, env, std::string("window.") + d.id);
        if (ed->*(d.flag) == start) bad += std::string("\n  ・開閉できない: ") + d.id;
        cmd::Execute(*ed, env, std::string("window.") + d.id);
        if (ed->*(d.flag) != start) bad += std::string("\n  ・元に戻らない: ") + d.id;
    }
    if (!bad.empty()) IM_ERRORF("window.<id> コマンドで開閉できない窓があります:%s", bad.c_str());

    Step(ctx, "全部開いてから「すべて閉じる」で 1 つも残らない");
    for (const tools::Desc& d : tools::kAll) ed->*(d.flag) = true;
    cmd::Execute(*ed, env, "view.closeTools");
    for (const tools::Desc& d : tools::kAll)
        if (ed->*(d.flag)) bad += std::string("\n  ・閉じ残し: ") + d.id;
    ed->showEngineDiagnostics = wasDiag;
    if (!bad.empty()) IM_ERRORF("「すべて閉じる」が取りこぼしています:%s", bad.c_str());
    ctx->Yield(4);
}

// ショートカット（コマンド表）が実際に効くこと。
// Esc=選択解除 / F2=改名 / Shift+F2=照らし込み / F5・Shift+F5=Play・停止 / Ctrl+K・Ctrl+P=パレット。
// ★「メニューに書いてあるのに効かない」の再発を実機で確かめる（表の単体テストは tests/editor_ux_test.cpp）。
void T_Shortcuts(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr && g_app != nullptr);

    Step(ctx, "Box を作って選択");
    AddEntity(ctx, "Box");
    SelectLastEntity(ctx);
    IM_CHECK(ed->HasSelection());

    Step(ctx, "Esc で選択解除");
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(4);
    IM_CHECK(!ed->HasSelection());

    Step(ctx, "F2 で名前変更（ヒエラルキーにインライン入力が出る）");
    SelectLastEntity(ctx);
    IM_CHECK(ed->HasSelection());
    ctx->KeyPress(ImGuiKey_F2);
    ctx->Yield(6);
    ctx->SetRef(kWinHierarchy);
    IM_CHECK(ctx->ItemExists("**/##Rename"));
    ctx->KeyPress(ImGuiKey_Escape);   // 改名をキャンセル（名前欄の Esc は改名だけを閉じる）
    ctx->Yield(4);
    IM_CHECK(!ctx->ItemExists("**/##Rename"));

    Step(ctx, "Shift+F2 で編集用の照らし込み（F2 は改名に譲った）");
    const f32 fill0 = ed->viewportFill;
    ctx->KeyPress(ImGuiKey_F2 | ImGuiMod_Shift);
    ctx->Yield(3);
    IM_CHECK((ed->viewportFill > 0.0f) != (fill0 > 0.0f));
    ctx->KeyPress(ImGuiKey_F2 | ImGuiMod_Shift);
    ctx->Yield(3);
    IM_CHECK((ed->viewportFill > 0.0f) == (fill0 > 0.0f));

    Step(ctx, "Ctrl+K でコマンドパレットを開く → 検索して Enter で実行 → 閉じる");
    const bool lightingWas = ed->showLighting;
    ed->showLighting = false;
    ctx->KeyPress(ImGuiKey_K | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(ed->paletteOpen);
    ctx->KeyChars("lighting");
    ctx->Yield(6);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(6);
    IM_CHECK(!ed->paletteOpen);
    IM_CHECK(ed->showLighting);           // 「ライティング」窓が開いた
    ed->showLighting = lightingWas;
    ctx->Yield(4);

    Step(ctx, "Ctrl+P でクイックオープン → Esc で閉じる");
    ctx->KeyPress(ImGuiKey_P | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(ed->paletteOpen);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(6);
    IM_CHECK(!ed->paletteOpen);

    Step(ctx, "F5 で Play → Shift+F5 で停止");
    ctx->KeyPress(ImGuiKey_F5);
    int frames = 0;
    while (g_app->GetEngineMode() != Application::EngineMode::Playing && frames < 300) { ctx->Yield(); ++frames; }
    IM_CHECK(g_app->GetEngineMode() == Application::EngineMode::Playing);
    ctx->Yield(10);
    ctx->KeyPress(ImGuiKey_F5 | ImGuiMod_Shift);
    frames = 0;
    while (g_app->GetEngineMode() != Application::EngineMode::Editor && frames < 300) { ctx->Yield(); ++frames; }
    IM_CHECK(g_app->GetEngineMode() == Application::EngineMode::Editor);
    ctx->Yield(6);

    // 後片付け
    for (auto e : ed->selectedEntities) ed->pendingDeletions.push_back(e);
    ed->ClearSelection();
    ctx->Yield(6);
}

// トースト: 積む / 同じ本文は 1 枚に畳む / クリックで閉じる。
void T_ToastFlow(ImGuiTestContext* ctx)
{
    Step(ctx, "トーストを積む（同じ本文は畳む）");
    ui::ToastClearAll();
    const uint32_t id1 = ui::ToastInfo("テスト通知");
    ui::ToastInfo("テスト通知");
    ctx->Yield(4);
    IM_CHECK_EQ(static_cast<int>(ui::ToastLiveCount()), 1);
    ui::ToastError("エラーの通知");
    ctx->Yield(4);
    IM_CHECK_EQ(static_cast<int>(ui::ToastLiveCount()), 2);

    Step(ctx, "クリックで閉じる");
    char ref[48];
    std::snprintf(ref, sizeof(ref), "//##Toast%u", id1);
    ctx->SetRef(ref);
    IM_CHECK(ctx->ItemExists("##dismiss"));
    ctx->ItemClick("##dismiss");
    ctx->Yield(4);
    IM_CHECK_EQ(static_cast<int>(ui::ToastLiveCount()), 1);

    ui::ToastClearAll();
    ctx->Yield(2);
}

// ヒエラルキーでの親替えでワールド位置が保たれる（フィルタ中の D&D も効く）。
// ★以前は parent だけ差し替えていたため、ローカル値に新しい親の変換が掛かり直って見た目が飛んだ。
void T_HierarchyReparent(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);
    auto& reg = scene->GetRegistry();

    Step(ctx, "親（Empty）と子（Box）を作って、名前を付け、親を回転・拡大しておく");
    AddEntity(ctx, "Empty");
    SelectLastEntity(ctx);
    const entt::entity parent = ed->selectedEntity;
    AddEntity(ctx, "Box");
    SelectLastEntity(ctx);
    const entt::entity child = ed->selectedEntity;
    IM_CHECK(reg.valid(parent) && reg.valid(child) && parent != child);
    reg.get<NameTag>(parent).name = "ReparentTestParent";
    reg.get<NameTag>(child).name  = "ReparentTestChild";
    {
        auto& pt = reg.get<Transform>(parent);
        pt.position = {10.0f, 0.0f, 4.0f}; pt.rotation = {0.0f, 90.0f, 0.0f}; pt.scale = {2.0f, 2.0f, 2.0f};
        auto& ct = reg.get<Transform>(child);
        ct.position = {3.0f, 1.0f, 0.0f};
    }
    ctx->Yield(3);
    DirectX::XMFLOAT3 worldBefore{};
    DirectX::XMStoreFloat3(&worldBefore, ComputeWorldMatrix(reg, child).r[3]);

    Step(ctx, "名前フィルタで 2 行に絞る（フィルタ中も D&D が効く）");
    ctx->SetRef(kWinHierarchy);
    ctx->ItemInputValue("##HierFilter", "ReparentTest");
    ctx->Yield(6);

    ImGuiID rowChild = 0, rowParent = 0;
    {
        ctx->SetRef(kWinHierarchy);
        ImGuiTestItemList items;
        ctx->GatherItems(&items, "", 3);
        for (int i = 0; i < items.GetSize(); ++i)
        {
            const ImGuiTestItemInfo* it = items[i];
            if (it == nullptr || it->ID == 0) continue;
            if (std::strncmp(it->DebugLabel, "ReparentTestChild", 17) == 0)  rowChild = it->ID;
            if (std::strncmp(it->DebugLabel, "ReparentTestParent", 18) == 0) rowParent = it->ID;
        }
    }
    IM_CHECK(rowChild != 0 && rowParent != 0);

    Step(ctx, "子の行を親の行へドラッグ&ドロップ");
    ctx->ItemDragAndDrop(rowChild, rowParent);
    ctx->Yield(6);
    IM_CHECK(reg.valid(child));
    IM_CHECK(reg.get<Transform>(child).parent == parent);

    DirectX::XMFLOAT3 worldAfter{};
    DirectX::XMStoreFloat3(&worldAfter, ComputeWorldMatrix(reg, child).r[3]);
    ctx->LogInfo("world before (%.2f,%.2f,%.2f) after (%.2f,%.2f,%.2f)",
                 worldBefore.x, worldBefore.y, worldBefore.z, worldAfter.x, worldAfter.y, worldAfter.z);
    IM_CHECK_LT(std::fabs(worldAfter.x - worldBefore.x), 0.01f);   // ワールド位置が飛んでいない
    IM_CHECK_LT(std::fabs(worldAfter.y - worldBefore.y), 0.01f);
    IM_CHECK_LT(std::fabs(worldAfter.z - worldBefore.z), 0.01f);
    IM_CHECK_GT(std::fabs(reg.get<Transform>(child).position.x - 3.0f), 0.5f);   // ローカル値は逆算し直された

    Step(ctx, "Undo で親子もローカル値も元に戻る");
    ed->pendingUndo = true;
    ctx->Yield(8);
    IM_CHECK(reg.get<Transform>(child).parent == entt::null);
    IM_CHECK_LT(std::fabs(reg.get<Transform>(child).position.x - 3.0f), 0.01f);

    Step(ctx, "後片付け（フィルタを消して 2 体を削除）");
    ctx->SetRef(kWinHierarchy);
    ctx->ItemInputValue("##HierFilter", "");
    ed->pendingDeletions.push_back(child);
    ed->pendingDeletions.push_back(parent);
    ed->ClearSelection();
    ctx->Yield(8);
}

// ===== [H] ヒエラルキーの大量エンティティ計測（フェーズ 1b）=====
// registry へ直接 10 万体（1000 グループ x 99 子）を作り、HierarchyPanel::Render の CPU ms を測る。
// 閉じた状態（ルート 1000 行）/ 全展開（10 万行をクリッパで間引く）/ 検索（名前一致 1 件）/ 選択あり の 4 通り。
// 終了時に作った物を全部消す（他のテストへ持ち越さない）。
void T_HierarchyPerf(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);
    auto& reg = scene->GetRegistry();

    constexpr int kGroups = 1000, kKids = 99;
    Step(ctx, "10 万体を registry へ直接作る（%d グループ x %d 子）", kGroups, kKids);
    std::vector<entt::entity> made;
    made.reserve(static_cast<size_t>(kGroups) * (kKids + 1));
    char nameBuf[48];
    for (int g = 0; g < kGroups; ++g)
    {
        const entt::entity root = reg.create();
        std::snprintf(nameBuf, sizeof(nameBuf), "PerfGroup_%04d", g);
        reg.emplace<NameTag>(root, NameTag{nameBuf});
        reg.emplace<Transform>(root);
        made.push_back(root);
        for (int k = 0; k < kKids; ++k)
        {
            const entt::entity c = reg.create();
            std::snprintf(nameBuf, sizeof(nameBuf), "PerfObj_%04d_%02d", g, k);
            reg.emplace<NameTag>(c, NameTag{nameBuf});
            Transform t; t.parent = root;
            reg.emplace<Transform>(c, t);
            made.push_back(c);
        }
    }
    ed->ClearSelection();

    auto measure = [&](const char* label, int frames)
    {
        ctx->Yield(4);   // 索引の作り直しなどの立ち上がりを捨てる
        double sum = 0.0, mx = 0.0;
        for (int f = 0; f < frames; ++f)
        {
            ctx->Yield(1);
            const double v = static_cast<double>(ed->hierRenderMs);
            sum += v; mx = (std::max)(mx, v);
        }
        ctx->LogInfo("[perf] hierarchy Render (%s): avg %.3f ms / max %.3f ms (%d frames, %zu entities)",
                     label, sum / frames, mx, frames, made.size());
    };

    // 作った直後の 1 フレーム目 = 索引 / 表示行を最初に作る（構造が変わった時の 1 回ぶん）
    ctx->Yield(1);
    ctx->LogInfo("[perf] hierarchy Render (最初の 1 フレーム = 索引の構築): %.3f ms (%zu entities)", static_cast<double>(ed->hierRenderMs), made.size());
    measure("閉じた状態", 20);

    Step(ctx, "すべて展開");
    ctx->SetRef(kWinHierarchy);
    ctx->ItemClick("expandAll/##ib");
    measure("全展開 10 万行", 20);

    Step(ctx, "1 体選択した状態");
    ed->Select(made[kKids + 5]);
    measure("全展開+選択", 10);

    Step(ctx, "検索で 1 件に絞る");
    ctx->SetRef(kWinHierarchy);
    ctx->ItemInputValue("##HierFilter", "PerfObj_0500_07");
    measure("検索 1 件", 10);
    ctx->ItemInputValue("##HierFilter", "PerfObj_05");
    measure("検索 約 1000 件", 10);
    ctx->ItemInputValue("##HierFilter", "");
    ctx->Yield(4);

    Step(ctx, "後片付け（作った 10 万体を全部削除）");
    ed->ClearSelection();
    for (entt::entity e : made) if (reg.valid(e)) reg.destroy(e);
    ctx->SetRef(kWinHierarchy);
    ctx->ItemClick("collapseAll/##ib");
    ctx->Yield(6);
}

// ===== [H] ヒエラルキー: 目と鍵 / 型チップと検索 / 並べ替えとフォルダ（フェーズ 1b）=====
// 行の項目 ID を名前で探す（ヒエラルキーはクリッパで可視行しか作らない。見えていなければ 0）。
ImGuiID HierRowId(ImGuiTestContext* ctx, const char* name)
{
    ctx->SetRef(kWinHierarchy);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, "", 3);
    const size_t n = std::strlen(name);
    for (int i = 0; i < items.GetSize(); ++i)
    {
        const ImGuiTestItemInfo* it = items[i];
        if (it != nullptr && it->ID != 0 && std::strncmp(it->DebugLabel, name, n) == 0) return it->ID;
    }
    return 0;
}

// 行の右端の目 / 鍵ボタンの ID（PushID(entity) の下の "##eye" / "##lock"）
ImGuiID HierFlagId(entt::entity e, const char* which)
{
    ImGuiWindow* w = ImGui::FindWindowByName(kWinHierarchyName);
    if (w == nullptr) return 0;
    const int ei = static_cast<int>(static_cast<uint32_t>(e));
    const ImGuiID base = ImHashData(&ei, sizeof(int), w->ID);
    return ImHashStr(which, 0, base);
}

entt::entity FindByName(entt::registry& reg, const char* name)
{
    for (auto [e, n] : reg.view<NameTag>().each())
        if (n.name == name) return e;
    return entt::null;
}

bool InDrawList(EditorContext* ed, entt::entity e)
{
    if (ed == nullptr || ed->drawItems == nullptr) return false;
    for (const DrawItem& it : *ed->drawItems) if (it.e == e) return true;
    return false;
}

// 行にホバーして目 / 鍵のボタンを出し、クリックする（alt=true で Alt+クリック）。
bool HierClickFlag(ImGuiTestContext* ctx, entt::entity e, const char* rowName, const char* which, bool alt)
{
    const ImGuiID row = HierRowId(ctx, rowName);
    if (row == 0) { IM_ERRORF("ヒエラルキーに行が見つかりません: %s", rowName); return false; }
    ctx->MouseMove(row);
    ctx->Yield(3);
    const ImGuiID id = HierFlagId(e, which);
    if (!ctx->ItemExists(id)) { IM_ERRORF("%s のボタンが出ていません（ホバーしても現れない）: %s", which, rowName); return false; }
    ctx->MouseMove(id);
    if (alt) ctx->KeyDown(ImGuiMod_Alt);
    ctx->MouseClick(0);
    if (alt) ctx->KeyUp(ImGuiMod_Alt);
    ctx->Yield(4);
    return true;
}

void T_HierarchyEyeLock(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);
    auto& reg = scene->GetRegistry();

    Step(ctx, "Box を 2 つ作って名前を付ける");
    AddEntity(ctx, "Box"); SelectLastEntity(ctx);
    entt::entity a = ed->selectedEntity;
    AddEntity(ctx, "Box"); SelectLastEntity(ctx);
    entt::entity b = ed->selectedEntity;
    IM_CHECK(reg.valid(a) && reg.valid(b) && a != b);
    reg.get<NameTag>(a).name = "EyeTestA";
    reg.get<NameTag>(b).name = "EyeTestB";
    ed->ClearSelection();
    ctx->Yield(4);
    IM_CHECK(!eflags::AnyOf<EditorHidden>(reg));
    IM_CHECK(InDrawList(ed, a) && InDrawList(ed, b));

    Step(ctx, "B の目を押す → 描画リストから消える（データは EditorHidden）");
    IM_CHECK(HierClickFlag(ctx, b, "EyeTestB", "##eye", false));
    IM_CHECK(eflags::Self<EditorHidden>(reg, b) && !eflags::Self<EditorHidden>(reg, a));
    ctx->Yield(3);
    IM_CHECK(!InDrawList(ed, b));
    IM_CHECK(InDrawList(ed, a));

    Step(ctx, "Undo で戻る（1 操作 1 エントリ）");
    ed->pendingUndo = true;
    ctx->Yield(6);
    IM_CHECK(!eflags::AnyOf<EditorHidden>(reg));
    IM_CHECK(InDrawList(ed, b));
    ed->pendingRedo = true;
    ctx->Yield(6);
    IM_CHECK(eflags::Self<EditorHidden>(reg, b));
    ed->pendingUndo = true;
    ctx->Yield(6);
    IM_CHECK(!eflags::AnyOf<EditorHidden>(reg));

    Step(ctx, "Alt+クリック = A だけ表示（他は全部隠れる）→ もう一度で全部表示");
    IM_CHECK(HierClickFlag(ctx, a, "EyeTestA", "##eye", true));
    ctx->Yield(3);
    IM_CHECK(!eflags::IsHidden(reg, a));
    IM_CHECK(eflags::IsHidden(reg, b));
    IM_CHECK(!InDrawList(ed, b));
    IM_CHECK(InDrawList(ed, a));
    IM_CHECK(HierClickFlag(ctx, a, "EyeTestA", "##eye", true));
    ctx->Yield(3);
    IM_CHECK(!eflags::AnyOf<EditorHidden>(reg));
    IM_CHECK(InDrawList(ed, b));
    ed->ClearSelection();

    Step(ctx, "Play 中は隠していても描かれる（エディタ専用）→ Stop で隠したまま戻る");
    reg.emplace_or_replace<EditorHidden>(a);
    ctx->Yield(3);
    IM_CHECK(!InDrawList(ed, a));
    g_app->RequestMode(Application::EngineMode::Playing);
    for (int f = 0; f < 240 && g_app->GetEngineMode() != Application::EngineMode::Playing; ++f) ctx->Yield();
    if (g_app->GetEngineMode() == Application::EngineMode::Playing)
    {
        ctx->Yield(20);
        const entt::entity ap = FindByName(scene->GetRegistry(), "EyeTestA");
        IM_CHECK(ap != entt::null);
        IM_CHECK(InDrawList(ed, ap));   // Play では見える
        g_app->RequestMode(Application::EngineMode::Editor);
        for (int f = 0; f < 240 && g_app->GetEngineMode() != Application::EngineMode::Editor; ++f) ctx->Yield();
        ctx->Yield(10);
    }
    else ctx->LogWarning("Play に入れませんでした（アクティブなカメラが無い？）。Play 中の確認は省略");
    {
        auto& r2 = g_app->GetScene()->GetRegistry();
        a = FindByName(r2, "EyeTestA"); b = FindByName(r2, "EyeTestB");
        IM_CHECK(a != entt::null && b != entt::null);
        IM_CHECK(eflags::Self<EditorHidden>(r2, a));   // Play → Stop のスナップショット往復でフラグが残る
        IM_CHECK(!InDrawList(ed, a));
        r2.remove<EditorHidden>(a);
        ctx->Yield(3);
        IM_CHECK(InDrawList(ed, a));
    }
    auto& regNow = g_app->GetScene()->GetRegistry();

    Step(ctx, "鍵: ロックした物はビューポートで選べない（奥は素通り）/ 鍵を外すと選べる");
    ed->ClearSelection();
    ctx->Yield(3);
    // 画面上で A に当たる点を探す（作成位置はカメラの前なのでビューポートのどこかに写る）
    ImVec2 hit(0.0f, 0.0f);
    bool found = false;
    for (int j = 2; j <= 8 && !found; ++j)
        for (int i = 2; i <= 8 && !found; ++i)
        {
            const ImVec2 p(ed->viewportX + ed->viewportW * (i / 10.0f), ed->viewportY + ed->viewportH * (j / 10.0f));
            ctx->MouseMoveToPos(p);
            ctx->Yield(1);
            ctx->MouseClick(0);
            ctx->Yield(3);
            if (ed->selectedEntity == a || ed->selectedEntity == b) { found = true; hit = p; }
        }
    if (!found) ctx->LogWarning("Box がビューポートに写っていないため、ロックの選択テストは省略");
    else
    {
        const entt::entity picked = ed->selectedEntity;
        ed->ClearSelection();
        regNow.emplace_or_replace<EditorLocked>(picked);
        ctx->Yield(3);
        ctx->MouseMoveToPos(hit);
        ctx->Yield(1);
        ctx->MouseClick(0);
        ctx->Yield(3);
        IM_CHECK(ed->selectedEntity != picked);          // ロック中は選べない
        // ヒエラルキーからは選べる
        ed->Select(picked);
        IM_CHECK(ed->IsSelected(picked));
        ed->ClearSelection();
        regNow.remove<EditorLocked>(picked);
        ctx->Yield(3);
        ctx->MouseMoveToPos(hit);
        ctx->Yield(1);
        ctx->MouseClick(0);
        ctx->Yield(3);
        IM_CHECK(ed->selectedEntity == picked);          // 外すと選べる
    }

    Step(ctx, "鍵ボタンで切り替え（Undo つき）");
    ed->ClearSelection();
    ctx->Yield(3);
    a = FindByName(regNow, "EyeTestA");
    IM_CHECK(HierClickFlag(ctx, a, "EyeTestA", "##lock", false));
    IM_CHECK(eflags::Self<EditorLocked>(regNow, a));
    ed->pendingUndo = true;
    ctx->Yield(6);
    IM_CHECK(!eflags::AnyOf<EditorLocked>(regNow));

    Step(ctx, "後片付け");
    ed->ClearSelection();
    a = FindByName(regNow, "EyeTestA"); b = FindByName(regNow, "EyeTestB");
    for (entt::entity e : {a, b}) if (e != entt::null) ed->pendingDeletions.push_back(e);
    ctx->Yield(8);
    for (auto e : hier::PlanClearAll<EditorHidden>(regNow)) regNow.remove<EditorHidden>(e);
    for (auto e : hier::PlanClearAll<EditorLocked>(regNow)) regNow.remove<EditorLocked>(e);
}

void T_HierarchyFilterChips(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);
    auto& reg = scene->GetRegistry();

    Step(ctx, "型の違う 4 体を registry に作る（タグ付き / ライト / メッシュ / 物理）");
    std::vector<entt::entity> made;
    auto mk = [&](const char* name) {
        const entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{name});
        reg.emplace<Transform>(e);
        made.push_back(e);
        return e;
    };
    { Tag t; t.tags = {"chipenemy"}; reg.emplace<Tag>(mk("ChipTagged"), t); }
    reg.emplace<PointLight>(mk("ChipLamp"));
    reg.emplace<MeshRenderer>(mk("ChipMesh"));
    reg.emplace<BoxCollider>(mk("ChipPhys"));
    ctx->Yield(4);

    auto countChipRows = [&]() {
        ctx->SetRef(kWinHierarchy);
        ImGuiTestItemList items;
        ctx->GatherItems(&items, "", 3);
        int n = 0;
        for (int i = 0; i < items.GetSize(); ++i)
            if (items[i] && items[i]->ID != 0 && std::strncmp(items[i]->DebugLabel, "Chip", 4) == 0) ++n;
        return n;
    };
    IM_CHECK(countChipRows() == 4);

    Step(ctx, "検索: tag:chipenemy → タグを持つ 1 体だけ");
    ctx->SetRef(kWinHierarchy);
    ctx->ItemInputValue("##HierFilter", "tag:chipenemy");
    ctx->Yield(6);
    IM_CHECK(countChipRows() == 1);
    IM_CHECK(HierRowId(ctx, "ChipTagged") != 0);

    Step(ctx, "検索: c:pointlight chip → ライトだけ / t:mesh chip → メッシュだけ");
    ctx->SetRef(kWinHierarchy);
    ctx->ItemInputValue("##HierFilter", "c:pointlight chip");
    ctx->Yield(6);
    IM_CHECK(countChipRows() == 1 && HierRowId(ctx, "ChipLamp") != 0);
    ctx->SetRef(kWinHierarchy);
    ctx->ItemInputValue("##HierFilter", "t:mesh chip");
    ctx->Yield(6);
    IM_CHECK(countChipRows() == 1 && HierRowId(ctx, "ChipMesh") != 0);
    ctx->SetRef(kWinHierarchy);
    ctx->ItemInputValue("##HierFilter", "chipenemy");   // 接頭辞なし = 名前 or タグ or コンポーネント名
    ctx->Yield(6);
    IM_CHECK(countChipRows() == 1 && HierRowId(ctx, "ChipTagged") != 0);

    Step(ctx, "型チップ: 物理 → 名前検索 Chip と組み合わせて 1 体、ライトも足すと OR で 2 体");
    ctx->SetRef(kWinHierarchy);
    ctx->ItemInputValue("##HierFilter", "Chip");
    ctx->Yield(4);
    ctx->SetRef(kWinHierarchy);
    ctx->ItemClick("##hierTypeChips/type_physics/##ib");
    ctx->Yield(6);
    IM_CHECK((ed->hierTypeFilter & hier::kTPhysics) != 0);
    IM_CHECK(countChipRows() == 1 && HierRowId(ctx, "ChipPhys") != 0);
    ctx->SetRef(kWinHierarchy);
    ctx->ItemClick("##hierTypeChips/type_light/##ib");
    ctx->Yield(6);
    IM_CHECK(countChipRows() == 2);
    ctx->SetRef(kWinHierarchy);
    ctx->ItemClick("##hierTypeChips/type_clear/##ib");
    ctx->Yield(6);
    IM_CHECK(ed->hierTypeFilter == 0);
    IM_CHECK(countChipRows() == 4);

    Step(ctx, "フィルタ中も行は通常どおり選べる");
    ctx->SetRef(kWinHierarchy);
    ctx->ItemInputValue("##HierFilter", "ChipMesh");
    ctx->Yield(6);
    const ImGuiID row = HierRowId(ctx, "ChipMesh");
    IM_CHECK(row != 0);
    ctx->MouseMove(row);
    ctx->MouseClick(0);
    ctx->Yield(4);
    IM_CHECK(ed->selectedEntity == FindByName(reg, "ChipMesh"));

    Step(ctx, "後片付け");
    ctx->SetRef(kWinHierarchy);
    ctx->ItemInputValue("##HierFilter", "");
    ed->hierTypeFilter = 0;
    ed->ClearSelection();
    for (entt::entity e : made) if (reg.valid(e)) reg.destroy(e);
    ctx->Yield(6);
}

void T_HierarchyReorder(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);
    auto& reg = scene->GetRegistry();

    Step(ctx, "ルートに 3 体（OrdA / OrdB / OrdC）を作る");
    std::vector<entt::entity> made;
    auto mk = [&](const char* name) {
        const entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{name});
        reg.emplace<Transform>(e);
        made.push_back(e);
        return e;
    };
    const entt::entity ea = mk("OrdA"), eb = mk("OrdB"), ec = mk("OrdC");
    ed->ClearSelection();
    ctx->Yield(5);
    auto pos = [&](entt::entity e) {
        hier::Index idx; idx.Build(reg);
        for (size_t i = 0; i < idx.roots.size(); ++i) if (idx.roots[i] == e) return static_cast<int>(i);
        return -1;
    };
    const int pa0 = pos(ea), pb0 = pos(eb), pc0 = pos(ec);
    IM_CHECK(pa0 >= 0 && pb0 >= 0 && pc0 >= 0);

    // 行の上端 25% へドロップする（掴む → 目的の行の上端へ動かす → 離す）
    auto dragTo = [&](const char* srcName, const char* dstName, float ratio) {
        ctx->SleepNoSkip(0.5f, 0.1f);   // 直前の操作と同じ場所を続けて押すとダブルクリック扱いになるので、間を空ける
        const ImGuiID s = HierRowId(ctx, srcName), d = HierRowId(ctx, dstName);
        IM_CHECK_RETV(s != 0 && d != 0, false);
        const ImGuiTestItemInfo di = ctx->ItemInfo(d);
        ctx->MouseMove(s);
        ctx->MouseDown(0);
        ctx->Yield(2);
        ctx->MouseMoveToPos(ImVec2(di.RectFull.Min.x + di.RectFull.GetWidth() * 0.4f,
                                   di.RectFull.Min.y + di.RectFull.GetHeight() * ratio));
        ctx->Yield(3);
        ctx->MouseUp(0);
        ctx->Yield(5);
        return true;
    };

    Step(ctx, "OrdC を OrdA の上端へドロップ → OrdA の前へ並び替わる（親は変わらない）");
    IM_CHECK(dragTo("OrdC", "OrdA", 0.08f));
    IM_CHECK(reg.get<Transform>(ec).parent == entt::null);
    IM_CHECK(pos(ec) < pos(ea));
    IM_CHECK(pos(ea) < pos(eb));

    Step(ctx, "Undo で元の並びへ");
    ed->pendingUndo = true;
    ctx->Yield(6);
    IM_CHECK(pos(ea) < pos(eb) && pos(eb) < pos(ec));
    IM_CHECK(reg.get<Transform>(ec).siblingOrder == 0 && reg.get<Transform>(ea).siblingOrder == 0);

    Step(ctx, "OrdC を OrdA の中央へドロップ → 子になる");
    IM_CHECK(dragTo("OrdC", "OrdA", 0.5f));
    IM_CHECK(reg.get<Transform>(ec).parent == ea);
    ed->pendingUndo = true;
    ctx->Yield(6);
    IM_CHECK(reg.get<Transform>(ec).parent == entt::null);

    Step(ctx, "選択を「フォルダに入れる」→ フォルダができて子になる / Undo で戻る");
    ed->ClearSelection();
    ed->Select(ea);
    ed->AddToSelection(eb);
    ctx->Yield(3);
    cmd::Execute(*ed, cmd::Env{scene, PathResolver::AssetsDir()}, "edit.newFolder");
    ctx->Yield(6);
    const entt::entity folder = FindByName(reg, "Folder");
    IM_CHECK(folder != entt::null && reg.all_of<EditorFolder>(folder));
    IM_CHECK(reg.get<Transform>(ea).parent == folder && reg.get<Transform>(eb).parent == folder);
    IM_CHECK(reg.get<Transform>(ec).parent == entt::null);
    ed->pendingUndo = true;
    ctx->Yield(6);
    IM_CHECK(FindByName(reg, "Folder") == entt::null);
    IM_CHECK(reg.get<Transform>(ea).parent == entt::null && reg.get<Transform>(eb).parent == entt::null);
    (void)pa0; (void)pb0; (void)pc0;

    Step(ctx, "後片付け");
    ed->ClearSelection();
    ctx->Yield(3);
    for (entt::entity e : made) if (reg.valid(e)) reg.destroy(e);
    ctx->Yield(6);
}

// ===== 表示倍率（DPI）=====
// ★何を守っているか
//   倍率 100% / 150% / 200% のどれでも、主要パネルの矩形を倍率で割った「論理サイズ」が 100% と同じ（±1.5px）で、
//   フォントも線形に拡大される（文字が箱からはみ出ない）こと。倍率を実行中に 1.0 → 1.5 → 2.0 → 1.0 と切り替える
//   （--dpi-scale と同じオーバーライド。OS の設定は触らない）。--background の窓は論理 1920x1080 × 倍率 の物理サイズへ
//   作り直されるので、ドックの比率もそのまま論理サイズに換算できる。--background でない窓（人の作業窓）では
//   窓サイズを勝手に変えられないので、倍率の切替が「落ちない・戻る」ことだけを確かめる。
struct DpiSample
{
    float scale = 1.0f;
    ImVec2 win[6] = {};       // ##Toolbar / ヒエラルキー / インスペクター / アセットブラウザ / ##StatusBar / ビューポート
    float fontPx = 0.0f, frameH = 0.0f, spacingY = 0.0f, scrollbar = 0.0f;
    float textBody = 0.0f, textJp = 0.0f, textMono = 0.0f;   // 同じ文字列の描画幅
    ImVec2 client = {};
};

const char* const kDpiWinNames[5] = {
    "##Toolbar",
    "\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc",                                             // ヒエラルキー
    "\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x9a\xe3\x82\xaf\xe3\x82\xbf\xe3\x83\xbc",                                 // インスペクター
    "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x96\xe3\x83\xa9\xe3\x82\xa6\xe3\x82\xb6",                     // アセットブラウザ
    "##StatusBar",
};

DpiSample DpiMeasure(EditorContext* ed)
{
    DpiSample s;
    s.scale = g_app->GetUiScale();
    for (int i = 0; i < 5; ++i)
        if (ImGuiWindow* w = ImGui::FindWindowByName(kDpiWinNames[i])) s.win[i] = w->Size;
    s.win[5] = ImVec2(ed->viewportW, ed->viewportH);
    const ImGuiStyle& st = ImGui::GetStyle();
    s.fontPx = ImGui::GetFontSize();
    s.frameH = ImGui::GetFrameHeight();
    s.spacingY = st.ItemSpacing.y;
    s.scrollbar = st.ScrollbarSize;
    s.textBody = ImGui::CalcTextSize("Transform Position Hello").x;
    s.textJp = ImGui::CalcTextSize("\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x9a\xe3\x82\xaf\xe3\x82\xbf\xe3\x83\xbc\xe3\x81\xae\xe8\xa1\xa8\xe7\xa4\xba").x;
    ui::PushMono();
    s.textMono = ImGui::CalcTextSize("0.123 -45.678 FPS 120").x;
    ui::PopMono();
    s.client = ImVec2(static_cast<float>(g_app->GetClientWidthPx()), static_cast<float>(g_app->GetClientHeightPx()));
    return s;
}

bool DpiSetAndSettle(ImGuiTestContext* ctx, float scale, bool bg)
{
    g_app->SetUiScaleOverride(scale);
    for (int f = 0; f < 90; ++f)
    {
        ctx->Yield(1);
        const bool sizeOk = !bg || (std::fabs(static_cast<float>(g_app->GetClientWidthPx()) - 1920.0f * scale) < 1.5f
                                    && std::fabs(static_cast<float>(g_app->GetClientHeightPx()) - 1080.0f * scale) < 1.5f);
        if (std::fabs(g_app->GetUiScale() - scale) < 0.001f && sizeOk && f >= 4)
        {
            ctx->Yield(10);   // ドックの寸法・行の位置が落ち着くまで
            return true;
        }
    }
    return false;
}

void T_DpiScaleLayout(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(g_app != nullptr && ed != nullptr);
    const bool bg = g_app->IsBackground();
    const float origOverride = dpi::Override();

    Step(ctx, "倍率 1.0 を基準に採る（bg=%d）", bg ? 1 : 0);
    tools::CloseAll(*ed);   // 右タブの窓が増えると比較が濁るので中核 4 窓の状態へ
    IM_CHECK(DpiSetAndSettle(ctx, 1.0f, bg));
    const DpiSample base = DpiMeasure(ed);
    if (bg)
    {
        IM_CHECK_GT(base.win[1].x, 100.0f);     // 主要パネルが実際に出ている
        IM_CHECK_GT(base.win[2].x, 100.0f);
        IM_CHECK_GT(base.win[3].y, 50.0f);
        IM_CHECK_GT(base.win[0].x, 500.0f);
        IM_CHECK_GT(base.win[5].x, 300.0f);
    }
    ctx->LogInfo("base: font %.1f frame %.1f toolbar %.0fx%.0f hier %.0fx%.0f insp %.0fx%.0f assets %.0fx%.0f status %.0fx%.0f viewport %.0fx%.0f",
                 base.fontPx, base.frameH, base.win[0].x, base.win[0].y, base.win[1].x, base.win[1].y, base.win[2].x,
                 base.win[2].y, base.win[3].x, base.win[3].y, base.win[4].x, base.win[4].y, base.win[5].x, base.win[5].y);

    static const float kScales[] = { 1.5f, 2.0f, 1.0f };
    const char* kNames[] = { "toolbar", "hierarchy", "inspector", "assets", "status", "viewport" };
    for (float sc : kScales)
    {
        Step(ctx, "倍率 %.2f へ切替", sc);
        IM_CHECK(DpiSetAndSettle(ctx, sc, bg));
        const DpiSample s = DpiMeasure(ed);
        IM_CHECK_LT(std::fabs(s.scale - sc), 0.001f);

        // フォント・スタイル: 線形（±1px 物理 = 丸めの範囲）。論理へ直すと 100% と同じ
        IM_CHECK_LT(std::fabs(s.fontPx / sc - base.fontPx), 1.0f / sc + 0.01f);
        IM_CHECK_LT(std::fabs(s.frameH / sc - base.frameH), 1.0f / sc + 0.51f);
        IM_CHECK_LT(std::fabs(s.spacingY / sc - base.spacingY), 1.0f);
        IM_CHECK_LT(std::fabs(s.scrollbar / sc - base.scrollbar), 1.0f);
        // 同じ文字列の描画幅: 論理へ直して ±6%（字送りは各サイズで整数 px に丸められるので 1 字あたり最大 0.5px 動く）
        //   = 文字が箱に対して同じ比率で伸びる（固定 px のままだと 1.5 倍で 33% ずれる）
        IM_CHECK_LT(std::fabs(s.textBody / sc - base.textBody), base.textBody * 0.06f);
        IM_CHECK_LT(std::fabs(s.textJp / sc - base.textJp), base.textJp * 0.06f);
        IM_CHECK_LT(std::fabs(s.textMono / sc - base.textMono), base.textMono * 0.06f);
        ctx->LogInfo("scale %.2f: font %.1f frame %.1f textBody %.1f/%.1f textJp %.1f/%.1f", sc, s.fontPx, s.frameH,
                     s.textBody / sc, base.textBody, s.textJp / sc, base.textJp);

        if (!bg) continue;   // 人の作業窓: 窓サイズが変わらないのでパネルの論理サイズ比較は意味が無い
        IM_CHECK_LT(std::fabs(s.client.x - 1920.0f * sc), 1.5f);
        IM_CHECK_LT(std::fabs(s.client.y - 1080.0f * sc), 1.5f);
        for (int i = 0; i < 6; ++i)
        {
            const float lw = s.win[i].x / sc, lh = s.win[i].y / sc;
            ctx->LogInfo("  %-9s logical %.1fx%.1f (base %.1fx%.1f)", kNames[i], lw, lh, base.win[i].x, base.win[i].y);
            if (std::fabs(lw - base.win[i].x) > 1.5f || std::fabs(lh - base.win[i].y) > 1.5f)
                IM_ERRORF("倍率 %.2f で %s の論理サイズが 100%% と違う: %.1fx%.1f (100%%: %.1fx%.1f)",
                          sc, kNames[i], lw, lh, base.win[i].x, base.win[i].y);
        }
    }

    Step(ctx, "元の倍率設定へ戻す");
    g_app->SetUiScaleOverride(origOverride);
    ctx->Yield(30);
}

// imgui.ini の倍率マーカー（[DpiScale][Main] Scale=）と、倍率が違う ini を読んだ時のドックの換算。
// ★何を守っているか: ini は物理 px で保存されるので、150% で保存した ini を 100% で開くとパネルが 1.5 倍の大きさのまま
//   はみ出す（逆ならスカスカ）。保存時の倍率を ini に書き、読み込み時に (今の倍率 / 保存時の倍率) で窓とドックのサイズを換算する。
//   ここでは今のレイアウトを ini に書き出し → 「半分の倍率で保存された」ことにして読み直し → ドックの SizeRef が 2 倍に
//   換算されること → 元の ini を読み戻すと元のレイアウトに戻ること、を確かめる。
void T_DpiIniRoundTrip(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(g_app != nullptr && ed != nullptr);
    const char* kHierarchy = "\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc";
    tools::CloseAll(*ed);
    ctx->Yield(8);
    ImGuiWindow* hier = ImGui::FindWindowByName(kHierarchy);
    IM_CHECK(hier != nullptr && hier->DockNode != nullptr);
    const ImGuiID nodeId = hier->DockNode->ID;
    const float widthBefore = hier->DockNode->Size.x;

    Step(ctx, "今のレイアウトを ini へ書き出す（倍率マーカーの確認）");
    ImGui::SaveIniSettingsToDisk("dx12_dpi_test_unused.ini");   // 保存経路（WriteAll）を一度通す。ファイルは後で消す
    std::error_code ec;
    std::filesystem::remove("dx12_dpi_test_unused.ini", ec);
    size_t len = 0;
    const char* raw = ImGui::SaveIniSettingsToMemory(&len);
    IM_CHECK(raw != nullptr && len > 0);
    const std::string ini0(raw, len);   // 内部バッファは次の呼び出しで変わるので即コピー
    const size_t hdr = ini0.find("[DpiScale][Main]");
    IM_CHECK(hdr != std::string::npos);
    const size_t eq = ini0.find("Scale=", hdr);
    IM_CHECK(eq != std::string::npos);
    const float saved = static_cast<float>(std::atof(ini0.c_str() + eq + 6));
    ctx->LogInfo("ini scale marker = %.4f (現在の倍率 %.4f)", saved, g_app->GetUiScale());
    IM_CHECK_LT(std::fabs(saved - g_app->GetUiScale()), 0.001f);

    // ドックの SizeRef（ini に書かれた値そのもの）を読む: 換算の期待値の基準
    const size_t dockPos = ini0.find("[Docking][Data]");
    IM_CHECK(dockPos != std::string::npos);
    const float sizeRefNow = hier->DockNode->SizeRef.x;

    Step(ctx, "半分の倍率で保存された ini として読み直す → 2 倍に換算される");
    const size_t valEnd = ini0.find('\n', eq);
    IM_CHECK(valEnd != std::string::npos);
    char half[64];
    std::snprintf(half, sizeof(half), "Scale=%.4f", static_cast<double>(saved * 0.5f));
    const std::string ini1 = ini0.substr(0, eq) + half + ini0.substr(valEnd);
    ImGui::LoadIniSettingsFromMemory(ini1.c_str(), ini1.size());
    ImGuiDockNode* n = ImGui::DockBuilderGetNode(nodeId);
    IM_CHECK(n != nullptr);
    ctx->LogInfo("SizeRef %.1f -> %.1f (期待 %.1f)", sizeRefNow, n->SizeRef.x, sizeRefNow * 2.0f);
    IM_CHECK_LT(std::fabs(n->SizeRef.x - sizeRefNow * 2.0f), 3.0f);

    Step(ctx, "元の ini を読み戻す → 元のレイアウトへ戻る");
    ImGui::LoadIniSettingsFromMemory(ini0.c_str(), ini0.size());
    ctx->Yield(10);
    hier = ImGui::FindWindowByName(kHierarchy);
    IM_CHECK(hier != nullptr && hier->DockNode != nullptr);
    ctx->LogInfo("hierarchy dock width %.1f -> %.1f", widthBefore, hier->DockNode->Size.x);
    IM_CHECK_LT(std::fabs(hier->DockNode->Size.x - widthBefore), 3.0f);
}

// ===================== [I] インスペクタ（フェーズ 1b）=====================
// 上部ヘッダ（名前 / 有効 / タグ / 固定）・Add Component の検索・複数選択の Transform 一括編集・行 / 見出しの右クリック・検索と折りたたみ。
// どのテストも開始時に自分の Box を作り、終わりに消す（後続のテストへ選択・値を持ち越さない）。

namespace inspector1b
{
// Box を作って、新しく増えたエンティティを直接選択する。
// ★ヒエラルキーの「最後の行」を押す SelectLastEntity は行の並び順に依存する（兄弟順・フォルダで変わる）ので使わない。
entt::entity AddAndSelectBox(ImGuiTestContext* ctx, const char* type = "Box")
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    if (!ed || !scene) return entt::null;
    auto& reg = scene->GetRegistry();
    std::set<entt::entity> before;
    for (auto [e, n] : reg.view<NameTag>().each()) before.insert(e);
    AddEntity(ctx, type);
    entt::entity added = entt::null;
    for (auto [e, n] : reg.view<NameTag>().each())
        if (before.count(e) == 0) added = e;
    if (added != entt::null) ed->Select(added);
    ctx->Yield(4);
    return added;
}

bool InDrawList(const EditorContext* ed, entt::entity e)
{
    if (!ed || !ed->drawItems) return false;
    for (const DrawItem& it : *ed->drawItems)
        if (it.e == e) return true;
    return false;
}

void Cleanup(ImGuiTestContext* ctx, std::initializer_list<entt::entity> es)
{
    EditorContext* ed = Ed();
    if (!ed) return;
    for (entt::entity e : es) ed->pendingDeletions.push_back(e);
    ed->selectedEntities.clear();
    ed->selectedEntity = entt::null;
    ctx->Yield(6);
}
} // namespace inspector1b

void T_InspectorHeader(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);
    auto& reg = scene->GetRegistry();

    Step(ctx, "Box を作って選択");
    const entt::entity a = inspector1b::AddAndSelectBox(ctx);
    IM_CHECK(a != entt::null && reg.valid(a) && reg.all_of<MeshRenderer>(a));
    const std::string oldName = reg.get<NameTag>(a).name;
    ctx->Yield(4);

    Step(ctx, "名前欄で改名 → Undo 1 回で戻る");
    ctx->SetRef(kWinInspector);
    ctx->ItemInputValue("##EntName", "InspRenamed");
    ctx->Yield(4);
    IM_CHECK_STR_EQ(reg.get<NameTag>(a).name.c_str(), "InspRenamed");
    ed->pendingUndo = true;
    ctx->Yield(8);
    IM_CHECK_STR_EQ(reg.get<NameTag>(a).name.c_str(), oldName.c_str());

    Step(ctx, "有効チェックを外す → 描画リストから消える");
    IM_CHECK(inspector1b::InDrawList(ed, a));
    ctx->SetRef(kWinInspector);
    ctx->ItemClick("##EntEnabled");
    ctx->Yield(4);
    IM_CHECK(eflags::Self<EntityDisabled>(reg, a));
    IM_CHECK(!inspector1b::InDrawList(ed, a));

    Step(ctx, "Undo で有効へ戻る（描画リストにも戻る）");
    ed->pendingUndo = true;
    ctx->Yield(8);
    IM_CHECK(!eflags::Self<EntityDisabled>(reg, a));
    IM_CHECK(inspector1b::InDrawList(ed, a));

    Step(ctx, "タグを追加（＋ タグ → 入力 → Enter）");
    ctx->SetRef(kWinInspector);
    ctx->ItemClick("##tagadd/##chip");
    ctx->Yield(3);
    ctx->SetRef("//$FOCUSED");
    ctx->ItemInputValue("##tagname", "enemy");
    ctx->Yield(4);
    ctx->SetRef(kWinInspector);
    {
        const Tag* t = reg.try_get<Tag>(a);
        IM_CHECK(t != nullptr && t->tags.size() == 1 && t->tags[0] == "enemy");
    }
    Step(ctx, "タグのチップを押して外す（最後の 1 個 = Tag コンポーネントごと消える）");
    ctx->ItemClick("##tag_enemy/##chip");
    ctx->Yield(4);
    IM_CHECK(!reg.all_of<Tag>(a));
    ed->pendingUndo = true;
    ctx->Yield(8);
    IM_CHECK(reg.all_of<Tag>(a));   // Undo で戻る
    ed->pendingUndo = true;
    ctx->Yield(8);
    IM_CHECK(!reg.all_of<Tag>(a));

    Step(ctx, "固定: 別のエンティティを選んでも表示は固定先のまま");
    ctx->SetRef(kWinInspector);
    ctx->ItemClick("##pinInspector/##ib");
    ctx->Yield(3);
    IM_CHECK(ed->inspectorPinned == a);
    const entt::entity b = inspector1b::AddAndSelectBox(ctx, "Sphere");
    IM_CHECK(b != entt::null && b != a);
    ctx->Yield(4);
    IM_CHECK(ed->inspectorPinned == a);
    IM_CHECK(ed->selectedEntity == b);   // 選択そのものは元のまま（Render の間だけ差し替える）
    ctx->SetRef(kWinInspector);
    {
        const char* shown = ctx->ItemReadAsString("##EntName");
        IM_CHECK_STR_EQ(shown, reg.get<NameTag>(a).name.c_str());   // 名前欄は固定先（a）
    }
    Step(ctx, "固定先を消すと自動で解除される");
    ed->pendingDeletions.push_back(a);
    ctx->Yield(8);
    IM_CHECK(ed->inspectorPinned == entt::null);

    inspector1b::Cleanup(ctx, {b});
}

void T_InspectorAddComponentSearch(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);
    auto& reg = scene->GetRegistry();

    Step(ctx, "Box を作って選択 → コンポーネント追加を開き、検索欄へ打つ（自動フォーカス）");
    const entt::entity a = inspector1b::AddAndSelectBox(ctx);
    IM_CHECK(a != entt::null && !reg.all_of<SphereCollider>(a));
    ctx->SetRef(kWinInspector);
    ctx->ItemClick(kBtnAddComponent);
    ctx->Yield(3);
    ctx->KeyChars("sphere coll");
    ctx->Yield(3);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(5);
    IM_CHECK(reg.all_of<SphereCollider>(a));
    IM_CHECK(insp::SplitList(prefs::GetString("insp.recentComponents")).front() == "SphereCollider");

    Step(ctx, "Undo で外れる");
    ed->pendingUndo = true;
    ctx->Yield(8);
    IM_CHECK(!reg.all_of<SphereCollider>(a));

    Step(ctx, "日本語の別名でも引ける（当たり判定 → コライダー。↓ で 2 番目を選んで Enter）");
    ctx->SetRef(kWinInspector);
    ctx->ItemClick(kBtnAddComponent);
    ctx->Yield(3);
    ctx->KeyChars("\xe5\xbd\x93\xe3\x81\x9f\xe3\x82\x8a\xe5\x88\xa4\xe5\xae\x9a");   // 当たり判定
    ctx->Yield(3);
    ctx->KeyPress(ImGuiKey_DownArrow);
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(5);
    const int colliders = (reg.all_of<BoxCollider>(a) ? 1 : 0) + (reg.all_of<SphereCollider>(a) ? 1 : 0) + (reg.all_of<CapsuleCollider>(a) ? 1 : 0);
    IM_CHECK_EQ(colliders, 1);
    ed->pendingUndo = true;
    ctx->Yield(8);

    Step(ctx, "一致が無い検索では Enter しても何も付かない。Esc で閉じる");
    const size_t before = reg.storage<BoxCollider>().size() + reg.storage<RigidBody>().size();
    ctx->SetRef(kWinInspector);
    ctx->ItemClick(kBtnAddComponent);
    ctx->Yield(3);
    ctx->KeyChars("zzzzqqqq");
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(3);
    IM_CHECK_EQ(reg.storage<BoxCollider>().size() + reg.storage<RigidBody>().size(), before);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(3);

    Step(ctx, "複数選択: 全員へ付く（Undo 1 回で全員から外れる）");
    const entt::entity b = inspector1b::AddAndSelectBox(ctx);
    IM_CHECK(b != entt::null && b != a);
    ed->selectedEntities.assign({a, b});
    ed->selectedEntity = a;
    ctx->Yield(4);
    ctx->SetRef(kWinInspector);
    ctx->ItemClick(kBtnAddComponent);
    ctx->Yield(3);
    ctx->KeyChars("capsule coll");
    ctx->Yield(3);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(5);
    IM_CHECK(reg.all_of<CapsuleCollider>(a) && reg.all_of<CapsuleCollider>(b));
    ed->pendingUndo = true;
    ctx->Yield(8);
    IM_CHECK(!reg.all_of<CapsuleCollider>(a) && !reg.all_of<CapsuleCollider>(b));

    inspector1b::Cleanup(ctx, {a, b});
}

void T_InspectorMultiTransform(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);
    auto& reg = scene->GetRegistry();

    Step(ctx, "Box を 3 個作り、位置を別々にする");
    entt::entity e[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        e[i] = inspector1b::AddAndSelectBox(ctx);
        IM_CHECK(e[i] != entt::null && reg.all_of<Transform>(e[i]));
    }
    for (int i = 0; i < 3; ++i)
    {
        auto& t = reg.get<Transform>(e[i]);
        t.position = {1.0f + 3.0f * i, 2.0f + 3.0f * i, 3.0f + 3.0f * i};   // (1,2,3) (4,5,6) (7,8,9)
    }
    ctx->Yield(3);

    Step(ctx, "3 個まとめて選択 → Transform の位置 Y だけを 42 にする");
    ed->selectedEntities.assign(std::begin(e), std::end(e));
    ed->selectedEntity = e[0];
    ctx->Yield(4);
    ctx->SetRef(kWinInspector);
    // FloatN の各軸は PushID(ラベル) → PushID(int 軸) → "##v"（PropertyGrid.h）
    ctx->ItemInputValue("**/\xe4\xbd\x8d\xe7\xbd\xae Position/$$1/##v", 42.0f);   // 位置 Position
    ctx->Yield(6);
    for (int i = 0; i < 3; ++i)
    {
        const auto& p = reg.get<Transform>(e[i]).position;
        if (std::fabs(p.y - 42.0f) > 0.01f)
        { IM_ERRORF("Transform%d の Y が届いていない（%.2f、期待 42）", i, p.y); }
        // 触っていない X / Z は各自のまま（全体コピーになっていない）
        if (std::fabs(p.x - (1.0f + 3.0f * i)) > 0.01f || std::fabs(p.z - (3.0f + 3.0f * i)) > 0.01f)
        { IM_ERRORF("Transform%d の X / Z まで上書きされている（%.2f, %.2f）", i, p.x, p.z); }
    }

    Step(ctx, "Undo 1 回で 3 個とも戻る");
    ed->pendingUndo = true;
    ctx->Yield(8);
    for (int i = 0; i < 3; ++i)
    {
        const auto& p = reg.get<Transform>(e[i]).position;
        if (std::fabs(p.y - (2.0f + 3.0f * i)) > 0.01f)
        { IM_ERRORF("Undo 1 回で Transform%d が戻っていない（Y=%.2f）", i, p.y); }
    }

    inspector1b::Cleanup(ctx, {e[0], e[1], e[2]});
}

void T_InspectorResetCopy(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);
    auto& reg = scene->GetRegistry();

    Step(ctx, "Box を作って選択し、位置を (5,6,7) にする");
    const entt::entity a = inspector1b::AddAndSelectBox(ctx);
    IM_CHECK(a != entt::null);
    reg.get<Transform>(a).position = {5.0f, 6.0f, 7.0f};
    ctx->Yield(3);

    Step(ctx, "位置の行を右クリック → 既定値へリセット");
    ctx->SetRef(kWinInspector);
    ctx->ItemClick("**/\xe4\xbd\x8d\xe7\xbd\xae Position/$$0/##v", ImGuiMouseButton_Right);
    ctx->Yield(2);
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick("\xe6\x97\xa2\xe5\xae\x9a\xe5\x80\xa4\xe3\x81\xb8\xe3\x83\xaa\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88");   // 既定値へリセット
    ctx->Yield(5);
    {
        const auto& p = reg.get<Transform>(a).position;
        IM_CHECK(p.x == 0.0f && p.y == 0.0f && p.z == 0.0f);
    }
    Step(ctx, "Undo で (5,6,7) に戻る");
    ed->pendingUndo = true;
    ctx->Yield(8);
    {
        const auto& p = reg.get<Transform>(a).position;
        IM_CHECK(p.x == 5.0f && p.y == 6.0f && p.z == 7.0f);
    }

    Step(ctx, "値をコピー → 別の値にしてから 値を貼り付け");
    ctx->SetRef(kWinInspector);
    ctx->ItemClick("**/\xe4\xbd\x8d\xe7\xbd\xae Position/$$0/##v", ImGuiMouseButton_Right);
    ctx->Yield(2);
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick("\xe5\x80\xa4\xe3\x82\x92\xe3\x82\xb3\xe3\x83\x94\xe3\x83\xbc");   // 値をコピー
    ctx->Yield(3);
    IM_CHECK(pg::InternalClipboard().rfind("dx12v:f:", 0) == 0);
    reg.get<Transform>(a).position = {1.0f, 1.0f, 1.0f};
    ctx->Yield(3);
    ctx->SetRef(kWinInspector);
    ctx->ItemClick("**/\xe4\xbd\x8d\xe7\xbd\xae Position/$$0/##v", ImGuiMouseButton_Right);
    ctx->Yield(2);
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick("\xe5\x80\xa4\xe3\x82\x92\xe8\xb2\xbc\xe3\x82\x8a\xe4\xbb\x98\xe3\x81\x91");   // 値を貼り付け
    ctx->Yield(5);
    {
        const auto& p = reg.get<Transform>(a).position;
        IM_CHECK(p.x == 5.0f && p.y == 6.0f && p.z == 7.0f);
    }

    Step(ctx, "コンポーネント見出しの右クリック: Box Collider を既定値へ / コピーして貼り付け");
    reg.emplace<BoxCollider>(a);
    reg.get<BoxCollider>(a).halfExtents = {2.0f, 3.0f, 4.0f};
    ctx->Yield(4);
    ctx->SetRef(kWinInspector);
    ctx->ItemClick("Box Collider", ImGuiMouseButton_Right);
    ctx->Yield(2);
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick("\xe3\x82\xb3\xe3\x83\xb3\xe3\x83\x9d\xe3\x83\xbc\xe3\x83\x8d\xe3\x83\xb3\xe3\x83\x88\xe3\x82\x92\xe3\x82\xb3\xe3\x83\x94\xe3\x83\xbc");   // コンポーネントをコピー
    ctx->Yield(3);
    ctx->SetRef(kWinInspector);
    ctx->ItemClick("Box Collider", ImGuiMouseButton_Right);
    ctx->Yield(2);
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick("\xe6\x97\xa2\xe5\xae\x9a\xe5\x80\xa4\xe3\x81\xb8\xe3\x83\xaa\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88");   // 既定値へリセット
    ctx->Yield(5);
    {
        const auto& h = reg.get<BoxCollider>(a).halfExtents;
        IM_CHECK(h.x == 0.5f && h.y == 0.5f && h.z == 0.5f);
    }
    ctx->SetRef(kWinInspector);
    ctx->ItemClick("Box Collider", ImGuiMouseButton_Right);
    ctx->Yield(2);
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick("\xe5\x80\xa4\xe3\x82\x92\xe8\xb2\xbc\xe3\x82\x8a\xe4\xbb\x98\xe3\x81\x91");   // 値を貼り付け
    ctx->Yield(5);
    {
        const auto& h = reg.get<BoxCollider>(a).halfExtents;
        IM_CHECK(h.x == 2.0f && h.y == 3.0f && h.z == 4.0f);
    }

    inspector1b::Cleanup(ctx, {a});
}

void T_InspectorSearchFold(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);

    Step(ctx, "Box を作って選択");
    const entt::entity a = inspector1b::AddAndSelectBox(ctx);
    IM_CHECK(a != entt::null);
    ctx->Yield(4);

    Step(ctx, "見出しを閉じる / 開く → 折りたたみ状態が prefs に残る");
    ctx->SetRef(kWinInspector);
    ctx->ItemClose("Transform");
    ctx->Yield(3);
    IM_CHECK(prefs::GetString("insp.fold").find("Transform=0") != std::string::npos);
    ctx->ItemOpen("Transform");
    ctx->Yield(3);
    IM_CHECK(prefs::GetString("insp.fold").find("Transform=1") != std::string::npos);

    Step(ctx, "すべて折りたたむ / すべて展開");
    ctx->ItemClick("##foldAllInsp/##ib");
    ctx->Yield(4);
    IM_CHECK(!ctx->ItemIsOpened("Transform"));
    ctx->ItemClick("##unfoldAllInsp/##ib");
    ctx->Yield(4);
    IM_CHECK(ctx->ItemIsOpened("Transform"));

    Step(ctx, "検索 scale → Transform だけ残り、行の無い見出しは隠れる");
    ctx->ItemClick("##InspSearch");
    ctx->KeyChars("scale");
    ctx->Yield(8);   // 1 フレーム目に全部描いて、行の無い見出しを学習 → 次から隠す
    IM_CHECK(ctx->ItemExists("Transform"));
    IM_CHECK(!ctx->ItemExists("MeshRenderer"));

    Step(ctx, "検索を空にすると全部戻る");
    ctx->ItemClick("##InspSearch");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
    ctx->KeyPress(ImGuiKey_Backspace);
    ctx->Yield(6);
    IM_CHECK(ctx->ItemExists("MeshRenderer"));

    Step(ctx, "Ctrl+F で検索欄へフォーカス");
    ctx->WindowFocus(kWinInspector);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_F);
    ctx->Yield(3);
    IM_CHECK(ImGui::GetActiveID() != 0);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);

    inspector1b::Cleanup(ctx, {a});
}

void T_InspectorComponentOrder(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    Scene* scene = g_app ? g_app->GetScene() : nullptr;
    IM_CHECK(ed != nullptr && scene != nullptr);
    auto& reg = scene->GetRegistry();

    Step(ctx, "Box にコライダーを 2 種付ける（既定の並びは Box Collider → Sphere Collider）");
    const entt::entity a = inspector1b::AddAndSelectBox(ctx);
    IM_CHECK(a != entt::null);
    reg.emplace<BoxCollider>(a);
    reg.emplace<SphereCollider>(a);
    ctx->Yield(6);   // 前フレームに描いた節の一覧（上へ / 下へ の可否）が揃うまで
    ctx->SetRef(kWinInspector);
    const float boxY0    = ctx->ItemInfo("Box Collider").RectFull.Min.y;
    const float sphereY0 = ctx->ItemInfo("Sphere Collider").RectFull.Min.y;
    IM_CHECK(boxY0 < sphereY0);

    Step(ctx, "Sphere Collider の見出しメニュー → 上へ");
    ctx->ItemClick("Sphere Collider", ImGuiMouseButton_Right);
    ctx->Yield(2);
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick("上へ");
    ctx->Yield(6);
    ctx->SetRef(kWinInspector);
    const float boxY1    = ctx->ItemInfo("Box Collider").RectFull.Min.y;
    const float sphereY1 = ctx->ItemInfo("Sphere Collider").RectFull.Min.y;
    IM_CHECK(sphereY1 < boxY1);
    IM_CHECK(insp::SplitList(prefs::GetString("insp.order")).size() > 2);   // 順序が保存される

    Step(ctx, "下へ で元の並びに戻す");
    ctx->ItemClick("Sphere Collider", ImGuiMouseButton_Right);
    ctx->Yield(2);
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick("下へ");
    ctx->Yield(6);
    ctx->SetRef(kWinInspector);
    IM_CHECK(ctx->ItemInfo("Box Collider").RectFull.Min.y < ctx->ItemInfo("Sphere Collider").RectFull.Min.y);

    inspector1b::Cleanup(ctx, {a});
}

// ===================== テスト表 =====================

struct DiagReg
{
    const char* category;   // ImGuiTestEngine 上のカテゴリ（ASCII）
    const char* name;       // ImGuiTestEngine 上のテスト名（ASCII）
    const char* group;      // 診断パネルの見出し（日本語）
    const char* display;    // 診断パネルの行名（日本語）
    void (*body)(ImGuiTestContext*);
    bool        deep;       // true=超詳細診断。「すべて検査する」には含めず専用ボタンで走らせる
};

const DiagReg kTests[] = {
    { "basic", "hierarchy_click_all",   "基本操作",           "ヒエラルキーの全項目をクリック",       T_HierarchyClickAll     },
    { "panel", "viewport_bar",          "パネル",             "ビューポートの帯（アスペクト / 隠す / スナップ / ビューキューブ / ブックマーク）", T_ViewportBar },
    { "panel", "viewport_selection",    "パネル",             "選択アウトライン / ビューモード / 表示フラグ / 矩形選択", T_ViewportSelection },
    { "basic", "spawn_all_types",       "基本操作",           "全種類のエンティティを生成",           T_SpawnAllEntityTypes   },
    { "basic", "inspector_open_all",    "基本操作",           "インスペクターの全セクションを開く",   T_InspectorOpenAll      },
    { "basic", "undo_redo",             "基本操作",           "Undo / Redo を往復",                   T_UndoRedoStress        },
    { "basic", "edit_menu_ops",         "基本操作",           "コピー / 貼り付け / 複製 / 削除",      T_EditMenuOps           },
    { "basic", "gizmo_view_modes",      "基本操作",           "ギズモ切替と 2D ビュー",               T_GizmoAndViewModes     },

    { "comp",  "add_all_components",    "コンポーネント",     "全コンポーネントを追加",               T_AddAllComponents      },
    { "comp",  "attach_script",         "コンポーネント",     "スクリプトを追加",                     T_AttachScript          },

    { "panel", "open_all_tool_windows", "パネル",             "すべてのツール窓を開いて描画",         T_OpenAllToolWindows    },
    { "panel", "dock_layout_persist",   "パネル",             "ツール窓を開閉してもビューポート/パネル幅が変わらない", T_DockLayoutSurvivesToolToggle },
    { "panel", "tool_registry",         "パネル",             "ツール窓レジストリ（全窓の開閉 / すべて閉じる）", T_ToolRegistry },
    { "basic", "shortcuts",             "基本操作",           "ショートカット（Esc / F2 / F5 / Ctrl+K / Ctrl+P）", T_Shortcuts },
    { "basic", "toast",                 "基本操作",           "トースト通知（積む / 畳む / クリックで閉じる）", T_ToastFlow },
    { "panel", "hierarchy_reparent",    "パネル",             "親替えでワールド位置が保たれる（フィルタ中も）", T_HierarchyReparent },
    { "panel", "hier_perf_100k",        "パネル",             "ヒエラルキー 10 万体の CPU ms 計測", T_HierarchyPerf },
    { "panel", "hierarchy_eye_lock",    "パネル",             "ヒエラルキーの目（非表示）と鍵（ロック）", T_HierarchyEyeLock },
    { "panel", "hierarchy_filter",      "パネル",             "ヒエラルキーの検索（名前 / コンポーネント / タグ）と型チップ", T_HierarchyFilterChips },
    { "panel", "hierarchy_reorder",     "パネル",             "ヒエラルキーの並べ替え（前 / 子）とフォルダ", T_HierarchyReorder },
    { "panel", "inspector_multi_edit",  "パネル",             "複数選択したライトを一括で編集できる", T_InspectorMultiEdit },
    { "panel", "inspector_header",      "パネル",             "インスペクタ上部（名前 / 有効 / タグ / 固定）", T_InspectorHeader },
    { "panel", "inspector_add_component_search", "パネル",     "コンポーネント追加の検索（別名 / キーボード / 複数選択）", T_InspectorAddComponentSearch },
    { "panel", "inspector_multi_transform", "パネル",         "複数選択の Transform（触った軸だけ全員へ / Undo 1 回）", T_InspectorMultiTransform },
    { "panel", "inspector_reset_copy",  "パネル",             "行 / 見出しの右クリック（既定値へ / コピー / 貼り付け）", T_InspectorResetCopy },
    { "panel", "inspector_search_fold", "パネル",             "インスペクタ内検索と折りたたみの保持", T_InspectorSearchFold },
    { "panel", "inspector_component_order", "パネル",         "コンポーネントの並べ替え（見出しメニューの 上へ / 下へ）", T_InspectorComponentOrder },
    { "play",  "ui_focus_releases",     "再生",               "HUD を押した後もパッド操作が死なない", T_UiFocusReleases },
    { "panel", "console",               "パネル",             "コンソール（フィルタ / Lua 実行）",    T_ConsolePanel          },
    { "panel", "asset_browser",         "パネル",             "アセットブラウザ",                     T_AssetBrowser          },
    { "panel", "asset_perf_10k",        "パネル",             "アセットブラウザ: 1 万ファイルの Render() CPU ms（perf/big があるときだけ）", T_AssetPerf10k },
    { "panel", "asset_browser_ops",     "パネル",             "アセットブラウザ: 作成 / 名前変更 / 複製 / 移動 / 削除 / フィルタ / 検索 / リスト", T_AssetBrowserOps },
    { "panel", "asset_os_drop",         "パネル",             "アセットブラウザ: OS からのファイルドロップで取り込み（WM_DROPFILES）", T_AssetOsDrop },
    { "panel", "new_floating_panels",   "パネル",             "ライティング / 地形ツールの開閉",       T_NewFloatingPanels     },
    { "panel", "settings_windows_pg",   "パネル",             "設定窓（ポスト / SSAO / SSR / フォグ / Skybox）が pg:: 行で値を書く", T_SettingsWindowsPg },
    { "panel", "layout_reset",          "パネル",             "ドックレイアウトのリセット",           T_LayoutReset           },
    { "panel", "workspace_switch",      "パネル",             "ワークスペース 6 種の切替（開く窓 / ビューポート / 最後の状態）", T_WorkspaceSwitch },
    { "panel", "layout_save_restore",   "パネル",             "名前つきレイアウトの保存 / 復元 / 削除 / 壊れた保存", T_LayoutSaveRestore },
    { "panel", "bottom_dock_slot",      "パネル",             "下部ドックのスロット（登録タブ / 最大化 / ダブルクリック）", T_BottomDockSlot },
    { "panel", "tool_dock_slots",       "パネル",             "ツール窓の配置先（右タブ / 右分割 / 下部 / フローティング）", T_ToolDockSlots },
    { "panel", "dpi_scale_layout",      "パネル",             "表示倍率 1.0 / 1.5 / 2.0 で論理レイアウトが変わらない", T_DpiScaleLayout },
    { "panel", "dpi_ini_roundtrip",     "パネル",             "imgui.ini の倍率マーカーと、倍率違いの ini の換算", T_DpiIniRoundTrip },
    { "panel", "mesh_gc",               "パネル",             "未参照メッシュの回収（リーク）",       T_MeshGarbageCollect    },
    { "panel", "delete_undo_parent",    "パネル",             "親子を消して Undo で親子が戻る",       T_DeleteUndoKeepsParent },
    { "panel", "prefab_geo_propagate",  "パネル",             "プレハブ適用で形状が配られる",         T_PrefabGeometryPropagate },

    { "uied",  "ui_spawn_all",          "UI エディタ",        "UI 要素を全種類配置",                  T_UiEditorSpawnAll      },
    { "uied",  "ui_view_ops",           "UI エディタ",        "ズーム / グリッド / 極小リサイズ",     T_UiEditorViewOps       },

    { "vfx",   "vfx_all_kinds",         "パーティクルエディタ", "見た目 / 合成 / 向きを全通り",       T_VfxEditorAllKinds     },
    { "vfx",   "vfx_apply_spawn",       "パーティクルエディタ", "選択へ適用 / 新規配置",              T_VfxApplyAndSpawn      },

    { "mat",   "material_editor",       "マテリアル",         "マテリアルエディタ（新規 / プレビュー）", T_MaterialEditor     },
    { "mat",   "material_library",      "マテリアル",         "マテリアルライブラリを開く",           T_MaterialLibrary       },

    { "play",  "play_stop_cycle",       "再生",               "Play → Stop を 2 往復",                T_PlayStopCycle         },
    { "play",  "physics_parent_world",  "再生",               "親の下の剛体が描画位置で当たる",       T_PhysicsParentedBodyWorldSpace },

    { "build", "build_game",            "ビルド",             "ゲームをビルドして成果物を確認",       T_BuildGame             },

    { "stress","rapid_selection",       "ストレス",           "選択を高速に切り替える",               T_RapidSelection        },
    { "stress","spam_add_delete",       "ストレス",           "生成と削除を連打",                     T_SpamAddDelete         },

    // ---- 超詳細診断（deep=true。「すべて検査する」には含まれない）----
    { "deep",  "deep_shaders",          "超詳細: シェーダー", "全 .cso の存在 / 破損 / 鮮度",         T_DeepShaders,        true },
    { "deep",  "deep_textures",         "超詳細: テクスチャ", "全画像の読み込みと色空間",             T_DeepTextures,       true },
    { "deep",  "deep_models",           "超詳細: モデル",     "全モデルの読み込みと形状",             T_DeepModels,         true },
    { "deep",  "deep_gamma_config",     "超詳細: ガンマ",     "表示パイプラインのフォーマット構成",   T_DeepGammaConfig,    true },
    { "deep",  "deep_gamma_roundtrip",  "超詳細: ガンマ",     "スクショの表示変換がシーン設定に追従", T_DeepGammaRoundTrip, true },
    { "deep",  "deep_scene_assets",     "超詳細: アセット",   "シーンのアセットが描画対象になっているか", T_DeepSceneAssets, true },
    { "deep",  "deep_lighting",         "超詳細: ライティング", "灯数上限 / 影スロット / 消灯 / IBL",  T_DeepLighting,       true },
    { "deep",  "deep_terrain",          "超詳細: 地形",       ".hf の整合 / コライダー / 共有の罠",   T_DeepTerrain,        true },
    { "deep",  "deep_picking",          "超詳細: ピッキング", "三角形精密判定が破綻する条件",         T_DeepPicking,        true },
    { "deep",  "deep_instancing",       "超詳細: 性能",       "インスタンシング適格率と不適格理由",   T_DeepInstancing,     true },
    { "deep",  "deep_scripts",          "超詳細: スクリプト", "全 .lua の構文スキャン",               T_DeepScripts,        true },
    { "deep",  "deep_render_proof",     "超詳細: 描画",       "実際に絵が出ているかピクセルで確認",   T_DeepRenderProof,    true },
    { "deep",  "deep_gizmo",            "超詳細: ギズモ",     "全モード×親子×2D で誤作動しないか",    T_DeepGizmo,          true },
    { "deep",  "deep_copy_paste",       "超詳細: コピペ複製", "コピペで子・コライダー・Lua・参照が残るか", T_DeepCopyPasteSubtree, true },
    { "deep",  "deep_duplicate",        "超詳細: コピペ複製", "複製の子孫維持と二重複製防止",         T_DeepDuplicateSubtree, true },
    { "deep",  "deep_prefab_roundtrip", "超詳細: プレハブ",   "プレハブ化→配置→リンク→構成維持",      T_DeepPrefabRoundtrip,  true },
    { "deep",  "deep_primitive_save",   "超詳細: 保存",       "プリミティブの寸法と PBR が保存往復するか", T_DeepPrimitiveSaveRoundtrip, true },
    { "deep",  "deep_alpha_save",       "超詳細: 保存",       "透明(アルファ)の上書きが保存往復するか", T_DeepAlphaSaveRoundtrip, true },

    // ★最後に置く: プロジェクトを閉じて開き直すので、シーンの中身（前のテストが作った物）は入れ替わる。
    { "launcher","launcher_screen",     "ランチャー",         "タブ切替 / 新規フォームの検証 / テンプレ選択 / 検索",  T_LauncherScreen        },
};

const DiagReg* FindReg(const ImGuiTest* test)
{
    return test ? static_cast<const DiagReg*>(test->UserData) : nullptr;
}

const char* DisplayName(const ImGuiTest* test)
{
    const DiagReg* reg = FindReg(test);
    return reg ? reg->display : (test ? test->Name : "?");
}

} // namespace

// ===================== UiTestHarness =====================

void UiTestHarness::SetOnlyList(const std::string& names)
{
    g_onlyTests.clear();
    size_t pos = 0;
    while (pos <= names.size())
    {
        size_t end = names.find_first_of(",;", pos);
        if (end == std::string::npos) end = names.size();
        std::string tok = names.substr(pos, end - pos);
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '"')) tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '"')) tok.pop_back();
        if (!tok.empty()) g_onlyTests.insert(tok);
        pos = end + 1;
    }
}

void UiTestHarness::SetSkipList(const std::string& names)
{
    g_skipTests.clear();
    size_t pos = 0;
    while (pos <= names.size())
    {
        size_t end = names.find_first_of(",;", pos);
        if (end == std::string::npos) end = names.size();
        std::string tok = names.substr(pos, end - pos);
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '"')) tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '"')) tok.pop_back();
        if (!tok.empty()) g_skipTests.insert(tok);
        pos = end + 1;
    }
}

void UiTestHarness::Initialize(Application* app, bool runAllAndExit, int speedMode, bool deepOnly)
{
    m_app           = app;
    m_runAllAndExit = runAllAndExit;
    m_deepOnly      = deepOnly;
    g_app           = app;

    m_engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& io = ImGuiTestEngine_GetIO(m_engine);
    io.ConfigRunSpeed = (speedMode == 2) ? ImGuiTestRunSpeed_Cinematic
                      : (speedMode == 1) ? ImGuiTestRunSpeed_Normal
                                         : ImGuiTestRunSpeed_Fast;
    io.ConfigVerboseLevel        = ImGuiTestVerboseLevel_Info;
    io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    io.ConfigLogToTTY            = true;   // 失敗内容を標準出力へ（バッチ実行の解析用）
    io.ConfigLogToDebugger       = true;
    if (runAllAndExit)
    {
        // 実行結果(失敗テスト名+理由)を JUnit XML で残す。バッチ/CI から読む用
        io.ExportResultsFilename = "ui_test_results.xml";
        io.ExportResultsFormat   = ImGuiTestEngineExportFormat_JUnitXml;
    }
    io.ConfigSavedSettings       = false;   // テストで .ini を汚さない
    io.ConfigStopOnError         = false;   // 1件落ちても残りを走らせる（網羅優先）
    io.ConfigBreakOnError        = false;   // 配布版でデバッグブレークしない
    io.ConfigNoThrottle          = true;
    // ハング検出。フリーズしたテストは自動で打ち切って「失敗」として残す。
    // ビルド検査は 1 フレームが数十秒になりうるので長めに取る。
    io.ConfigWatchdogWarning  = 30.0f;
    io.ConfigWatchdogKillTest = 180.0f;

    RegisterTests();
    ImGuiTestEngine_Start(m_engine, ImGui::GetCurrentContext());

    Logger::Info("UI テストエンジンを起動しました (run-all={})", runAllAndExit ? "yes" : "no");
}

void UiTestHarness::RefreshSummary()
{
    ImGuiTestEngineResultSummary summary{};
    ImGuiTestEngine_GetResultSummary(m_engine, &summary);
    m_lastTested  = summary.CountTested;
    m_lastSuccess = summary.CountSuccess;
}

void UiTestHarness::QueueTests(const char* filterCategory, bool failedOnly, int deepSel)
{
    ImVector<ImGuiTest*> tests;
    ImGuiTestEngine_GetTestList(m_engine, &tests);

    m_queuedTotal = 0;
    for (int i = 0; i < tests.Size; ++i)
    {
        ImGuiTest* test = tests[i];
        if (test == nullptr) continue;
        if (g_skipTests.count(test->Name) != 0) continue;   // --ui-tests-skip
        if (!g_onlyTests.empty() && g_onlyTests.count(test->Name) == 0) continue;   // --ui-tests-only
        if (filterCategory != nullptr && std::strcmp(test->Category, filterCategory) != 0) continue;
        if (failedOnly && test->Output.Status != ImGuiTestStatus_Error) continue;
        // deepSel: -1=問わない / 0=通常の検査だけ / 1=超詳細だけ
        if (deepSel >= 0)
        {
            const DiagReg* reg = FindReg(test);
            const bool isDeep = (reg != nullptr && reg->deep);
            if (isDeep != (deepSel == 1)) continue;
        }

        g_testNotes.erase(test->Name);
        ImGuiTestEngine_QueueTest(m_engine, test, ImGuiTestRunFlags_None);
        ++m_queuedTotal;
    }
    BeginRun();
}

void UiTestHarness::BeginRun()
{
    if (m_queuedTotal == 0)
    {
        m_statusText = "実行する検査項目がありません。";
        return;
    }

    // 検査はシーンを実際に書き換える。ユーザーの作業を壊さないよう退避しておき、
    // 完了時に自動で読み戻す（PostRender の完了検知側で復元）。
    if (m_app && m_sceneSnapshot.empty())
    {
        // 開いていたシーンのパスも覚えておく。復元ロードでこれが退避ファイルに
        // 書き換わってしまい、タイトルバーが ".diagnostics_snapshot" になる＋
        // そのまま上書き保存されかねないため、復元後に必ず戻す。
        if (const EditorContext* ed = m_app->GetEditorContext())
            m_savedScenePath = ed->currentScenePath;
        m_sceneSnapshot = m_app->SaveSceneSnapshot();
    }

    m_running    = true;
    m_statusText = "検査を実行しています...";
    guard::TestRunActive().store(true);   // テスト中は OS への副作用（エクスプローラー等）を実行しない
    CrashHandler::Breadcrumb("エンジン診断: 検査を開始");
    Logger::Info("エンジン診断: {} 件の検査を開始しました", m_queuedTotal);
}

void UiTestHarness::PostRender()
{
    if (!m_engine) return;
    ImGuiTestEngine_PostSwap(m_engine);

    // 診断パネルから走らせたテストの完了検知（--ui-tests-run-all でない通常起動時）
    if (m_running && ImGuiTestEngine_IsTestQueueEmpty(m_engine))
    {
        m_running = false;
        guard::TestRunActive().store(false);
        RefreshSummary();
        const int failed = m_lastTested - m_lastSuccess;
        m_statusText = (failed == 0)
            ? "問題は見つかりませんでした。クラッシュ・フリーズは起きていません。"
            : "問題が " + std::to_string(failed) + " 件見つかりました。下の赤い項目を確認してください。";
        CrashHandler::Breadcrumb("エンジン診断: 検査が完了");
        Logger::Info("エンジン診断: {}/{} 成功", m_lastSuccess, m_lastTested);

        // 検査で書き換えたシーンを元に戻す（ロードはフレーム境界なので、ここでは要求だけ）
        if (m_app && !m_sceneSnapshot.empty())
        {
            m_app->RequestSceneRestore(m_sceneSnapshot);
            m_restorePending = true;
        }
    }

    // 復元が実際に消化されてからスナップショットを消す。
    // 要求と同時に消すと、フレーム境界のロードが「ファイルが開けません」で失敗して
    // 検査で汚れたシーンがそのまま残る（実際にそうなっていた）。
    if (m_restorePending && m_app)
    {
        EditorContext* ed = m_app->GetEditorContext();
        if (ed != nullptr && ed->pendingLoadPath.empty())
        {
            ed->currentScenePath = m_savedScenePath;   // タイトル/保存先を元のシーンへ戻す
            std::error_code ec;
            std::filesystem::remove(m_sceneSnapshot, ec);
            m_sceneSnapshot.clear();
            m_savedScenePath.clear();
            m_restorePending = false;
        }
    }

    if (!m_runAllAndExit) return;

    // 起動直後の 1 フレーム目でキューに積むと、まだパネルが出ておらず全部失敗する。
    // プロジェクトのロードが終わってエディタ UI（ヒエラルキー）が出るまで待つ。
    // ランチャーのまま進まない場合に備えて 1800 フレームで打ち切る。
    static int warmup = 0;
    if (!m_started)
    {
        ++warmup;
        if (warmup < 60) return;
        if (warmup < 1800 && ImGui::FindWindowByName(kWinHierarchyName) == nullptr) return;
        // ★打ち切り後もヒエラルキーが無い＝エディタが出ていない（引数なし起動は
        //   前回プロジェクトを復元せずランチャーで止まる。Application.cpp:1173）。
        //   ここでテストを流すと 25 件が「パネルが見つからない」で落ち、本物の UI 不具合に
        //   見える。実際それで小一時間溶かした。原因を名指しして走らせずに終わる。
        if (ImGui::FindWindowByName(kWinHierarchyName) == nullptr)
        {
            Logger::Error("UI テストを実行できません: エディタが開いていません"
                          "（ランチャー画面のまま）。--project <プロジェクトのフォルダ> を付けて"
                          "起動してください。例: DX12Engine.exe --ui-tests-run-all --project .");
            m_exitCode  = 2;
            m_wantsExit = true;
            m_started   = true;
            return;
        }
        QueueTests(nullptr, false, m_deepOnly ? 1 : -1);
        m_started = true;
        Logger::Info("UI テスト: {} 件をキューへ投入しました ({})",
                     m_queuedTotal, m_deepOnly ? "超詳細診断のみ" : "全テスト");
        return;
    }

    if (ImGuiTestEngine_IsTestQueueEmpty(m_engine))
    {
        ImGuiTestEngineResultSummary summary{};
        ImGuiTestEngine_GetResultSummary(m_engine, &summary);
        Logger::Info("UI テスト完了: {}/{} 成功", summary.CountSuccess, summary.CountTested);

        // 詳細(失敗テスト名と理由)は ExportResultsFilename の JUnit XML に出力される
        m_exitCode = (summary.CountSuccess == summary.CountTested) ? 0 : 1;
        m_wantsExit = true;
    }
}

std::string UiTestHarness::BuildReport() const
{
    ImVector<ImGuiTest*> tests;
    ImGuiTestEngine_GetTestList(m_engine, &tests);

    std::string report = "==== Uno Engine エンジン診断 結果 ====\n";
    report += "エンジン版: v" + std::string(kEngineVersion) + "\n";
    report += "結果: " + std::to_string(m_lastSuccess) + " / " + std::to_string(m_lastTested) + " 成功\n\n";

    for (int i = 0; i < tests.Size; ++i)
    {
        ImGuiTest* test = tests[i];
        if (test == nullptr) continue;
        const DiagReg* reg = FindReg(test);

        const char* st = (test->Output.Status == ImGuiTestStatus_Success) ? "[ OK ]"
                       : (test->Output.Status == ImGuiTestStatus_Error)   ? "[失敗]"
                                                                          : "[未実行]";
        report += std::string(st) + " " + (reg ? reg->group : test->Category)
                + " / " + DisplayName(test) + "\n";

        if (test->Output.Status == ImGuiTestStatus_Error)
        {
            ImGuiTextBuffer buf;
            test->Output.Log.ExtractLinesForVerboseLevels(
                ImGuiTestVerboseLevel_Error, ImGuiTestVerboseLevel_Error, &buf);
            if (buf.size() > 0)
                report += std::string("    理由: ") + buf.c_str() + "\n";
        }

        auto note = g_testNotes.find(test->Name);
        if (note != g_testNotes.end() && !note->second.empty())
            report += "    実行中のエラーログ:\n" + note->second;
    }

    report += "\n(この内容をそのまま開発者へ貼り付けてください。"
              "クラッシュした場合は dx12_crash.log / dx12_engine.log も添えてください)\n";
    return report;
}

void UiTestHarness::DrawDiagnosticsPanel(bool* show, bool* hoveredOut)
{
    if (!m_engine) return;

    // 上級者向け: ImGuiTestEngine 純正の詳細窓（個別実行・ログ・キャプチャ）
    if (m_showEngineUi)
        ImGuiTestEngine_ShowTestEngineWindows(m_engine, &m_showEngineUi);

    if (show == nullptr || !*show) return;

    // 検査中にこの窓が動かない・消えないようにするための 3 点:
    //
    // ① NoDocking … 「レイアウトをリセット」やツール窓の開閉でドックが組み直されると、
    //    ドック可能な窓は右下タブ群へ吸い込まれて別タブの裏に隠れる（＝消えたように見える）。
    // ② NoInputs(検査中のみ) … ImGuiTestEngine は「クリックしたい位置を覆っている窓」を
    //    _MakeAimingSpaceOverPos() で脇へどかす。これが「検査の途中で診断 UI が勝手に動く」
    //    の正体。NoInputs にすると当たり判定から外れて“覆っている”と見なされなくなるので、
    //    どかされず・かつ下のパネルのクリックも邪魔しない。検査中はボタンが全て無効なので
    //    操作できなくても困らない。
    // ③ 位置の固定 … ②で動かされなくなるが、レイアウト再構築など別経路の移動もまとめて封じる。
    //    （※ 最前面へ固定してはいけない。常に対象より手前になって毎回どかされる）
    ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoDocking;
    if (m_running)
    {
        panelFlags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav;
        if (m_panelPosX != 0.0f || m_panelPosY != 0.0f)
            ImGui::SetNextWindowPos(ImVec2(m_panelPosX, m_panelPosY), ImGuiCond_Always);
    }
    ImGui::SetNextWindowSize(theme::Px(680.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("エンジン診断", show, panelFlags))
    {
        ImGui::End();
        return;
    }

    // この窓は3Dビューポートの上に浮かぶ。ホバー中は背後のシーンカメラがホイールに
    // 反応しないよう呼び出し側へ伝える（他のツール窓と同じ2値ラッチ）。
    // 結果一覧を BeginChild でスクロールするので RootAndChildWindows で拾う。
    if (hoveredOut && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                                             | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem
                                             | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        *hoveredOut = true;

    // 検査していない間の位置を覚えておく（検査中はこの位置に固定する）
    if (!m_running)
    {
        const ImVec2 pos = ImGui::GetWindowPos();
        m_panelPosX = pos.x;
        m_panelPosY = pos.y;
    }

    ImVector<ImGuiTest*> tests;
    ImGuiTestEngine_GetTestList(m_engine, &tests);

    // ---- 集計 ----
    int total = tests.Size, ok = 0, ng = 0, notRun = 0;
    const char* runningName = nullptr;
    for (int i = 0; i < tests.Size; ++i)
    {
        ImGuiTest* t = tests[i];
        if (t == nullptr) continue;
        switch (t->Output.Status)
        {
        case ImGuiTestStatus_Success: ++ok; break;
        case ImGuiTestStatus_Error:   ++ng; break;
        case ImGuiTestStatus_Running: runningName = DisplayName(t); break;
        default:                      ++notRun; break;
        }
    }

    // ---- 説明 ----
    ImGui::TextWrapped(
        "エディタを自動で操作して、クラッシュ・フリーズ・エラーが起きないか検査します。"
        "不具合が出たときはこれを実行し、[結果をコピー] の内容を開発者へ送ってください。");
    ImGui::Spacing();
    ImGui::TextDisabled("・検査中はマウスとキーボードが自動で動きます。触らずにお待ちください。");
    ImGui::TextDisabled("・シーンは検査前に退避し、完了後に自動で元へ戻します。");
    ImGui::TextDisabled("・[ビルド] の検査だけは実際に書き出すため、時間がかかります。");
    ImGui::TextDisabled("・[超詳細診断] は UI ではなくエンジンの中身（シェーダー / テクスチャ / モデル /"
                        " ガンマ / 描画 / ギズモ / ライティング / 地形 / ピッキング / 性能 / Lua）を検査します。");
    ImGui::Separator();

    // ---- 実行ボタン ----
    ImGui::BeginDisabled(m_running);
    if (ImGui::Button("▶ すべて検査する", theme::Px(180.0f, 36.0f)))
        QueueTests(nullptr, false, 0);
    ImGui::SameLine();
    if (ImGui::Button("🔬 超詳細診断", theme::Px(150.0f, 36.0f)))
        QueueTests(nullptr, false, 1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "エンジンの機能そのものを検査します（UI 操作ではありません）:\n"
            "・全シェーダーが存在して壊れておらず、.hlsl より新しいか\n"
            "・全テクスチャが読めるか / 法線マップが sRGB で保存されていないか\n"
            "・全モデルが読めるか / 頂点が NaN でないか / ボーンが上限を超えていないか\n"
            "・表示パイプラインのフォーマットがガンマ二重適用になっていないか\n"
            "・スクリーンショットの色がビューポートと一致するか\n"
            "・実際に絵が出ているか（ピクセルで確認）\n"
            "・ギズモが全モード×親子×マルチ選択×常時スナップ×2D で誤作動しないか\n"
            "・ライトが上限(点8/スポット8)を超えて無言で切り捨てられていないか\n"
            "・地形の .hf が壊れていないか / 同じハイトマップを複数の地形が共有していないか\n"
            "・三角形の精密ピッキングが効かない形になっていないか\n"
            "・自動インスタンシングが効いているか（不適格の理由をランキング表示）\n"
            "・全 .lua が構文として閉じているか\n\n"
            "アセット数が多いと数分かかります。");
    ImGui::SameLine();
    ImGui::BeginDisabled(ng == 0);
    if (ImGui::Button("失敗だけ再検査", theme::Px(140.0f, 36.0f)))
        QueueTests(nullptr, true);
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("結果をコピー", theme::Px(130.0f, 36.0f)))
    {
        ImGui::SetClipboardText(BuildReport().c_str());
        m_statusText = "結果をクリップボードにコピーしました。";
    }

    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Checkbox("成功も表示", &m_showPassedRows);
    ImGui::Checkbox("詳細ウィンドウ", &m_showEngineUi);
    ImGui::EndGroup();

    // ---- 進捗 ----
    if (m_running)
    {
        const int done = (m_queuedTotal > 0) ? (ok + ng) : 0;
        const float frac = (m_queuedTotal > 0)
            ? static_cast<float>(done) / static_cast<float>(m_queuedTotal) : 0.0f;
        char label[128];
        std::snprintf(label, sizeof(label), "%d / %d", done, m_queuedTotal);
        ImGui::Spacing();
        ImGui::ProgressBar(frac, ImVec2(-1, 0), label);
        if (runningName)
            ImGui::TextDisabled("検査中: %s", runningName);
    }

    // ---- 結果バナー ----
    if (!m_statusText.empty())
    {
        ImGui::Spacing();
        const bool bad = (ng > 0);
        ImGui::TextColored(bad ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.45f, 0.9f, 0.5f, 1.0f),
                           "%s", m_statusText.c_str());
    }

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1.0f), "OK %d", ok);
    ImGui::SameLine();
    ImGui::TextColored(ng > 0 ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                       "  失敗 %d", ng);
    ImGui::SameLine();
    ImGui::TextDisabled("  未実行 %d  /  全 %d", notRun, total);

    ImGui::Separator();

    // ---- グループごとの一覧 ----
    // 子ウィンドウは親の NoInputs を継承しないので、検査中は明示的に外す
    // （でないとこのリストが下のパネルのクリック判定を奪い、検査が失敗する）。
    ImGui::BeginChild("##diaglist", ImVec2(0, 0), ImGuiChildFlags_None,
                      m_running ? ImGuiWindowFlags_NoInputs : ImGuiWindowFlags_None);
    const char* lastGroup = nullptr;
    bool groupOpen = true;
    for (int i = 0; i < tests.Size; ++i)
    {
        ImGuiTest* test = tests[i];
        if (test == nullptr) continue;
        const DiagReg* reg = FindReg(test);
        const char* group  = reg ? reg->group : test->Category;

        if (lastGroup == nullptr || std::strcmp(lastGroup, group) != 0)
        {
            lastGroup = group;
            ImGui::Spacing();
            groupOpen = ImGui::CollapsingHeader(group, ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::SameLine(ImGui::GetContentRegionMax().x - theme::Px(130.0f));
            ImGui::PushID(group);
            ImGui::BeginDisabled(m_running);
            if (ImGui::SmallButton("ここだけ検査"))
                QueueTests(test->Category, false);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (!groupOpen) continue;

        const bool failed = (test->Output.Status == ImGuiTestStatus_Error);
        if (!failed && !m_showPassedRows && test->Output.Status == ImGuiTestStatus_Success)
            continue;

        ImGui::PushID(test->Name);

        // 状態バッジ → 項目名。バッジ幅がまちまちなので、名前の開始 X を固定して縦に揃える。
        const float kNameColumnX = theme::Px(96.0f);
        ImGui::Indent(theme::Px(8.0f));
        switch (test->Output.Status)
        {
        case ImGuiTestStatus_Success:
            ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1.0f), "OK");
            break;
        case ImGuiTestStatus_Error:
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "失敗");
            break;
        case ImGuiTestStatus_Running:
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "実行中");
            break;
        case ImGuiTestStatus_Queued:
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1.0f), "待機中");
            break;
        default:
            ImGui::TextDisabled("未実行");
            break;
        }
        ImGui::Unindent(theme::Px(8.0f));
        ImGui::SameLine(kNameColumnX);
        ImGui::TextUnformatted(DisplayName(test));

        // 所要時間
        if (test->Output.EndTime > test->Output.StartTime)
        {
            const double sec = static_cast<double>(test->Output.EndTime - test->Output.StartTime) / 1000000.0;
            ImGui::SameLine();
            ImGui::TextDisabled("(%.1f 秒)", sec);
        }

        // 失敗理由（エラーレベルのログ行だけ抜き出して赤で出す）
        if (failed)
        {
            ImGuiTextBuffer buf;
            test->Output.Log.ExtractLinesForVerboseLevels(
                ImGuiTestVerboseLevel_Error, ImGuiTestVerboseLevel_Error, &buf);
            if (buf.size() > 0)
            {
                ImGui::Indent(theme::Px(24.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.5f, 1.0f));
                ImGui::TextWrapped("%s", buf.c_str());
                ImGui::PopStyleColor();
                ImGui::Unindent(theme::Px(24.0f));
            }
        }

        // 実行中に出たエラーログ（落ちてはいないが静かに壊れている、を拾う）
        auto note = g_testNotes.find(test->Name);
        if (note != g_testNotes.end() && !note->second.empty())
        {
            ImGui::Indent(theme::Px(24.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
            ImGui::TextWrapped("実行中のエラーログ:\n%s", note->second.c_str());
            ImGui::PopStyleColor();
            ImGui::Unindent(theme::Px(24.0f));
        }

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::End();
}

void UiTestHarness::Shutdown()
{
    if (!m_engine) return;
    ImGuiTestEngine_Stop(m_engine);
    // DestroyContext は ImGui::DestroyContext() の後に呼ぶ必要があるが、
    // 本エンジンでは ImGuiManager::Shutdown より先にここが呼ばれるため、
    // エンジン破棄はプロセス終了に任せる（リークしても即終了なので実害なし）。
    m_engine = nullptr;
    g_app    = nullptr;
}

void UiTestHarness::RegisterTests()
{
    for (const DiagReg& reg : kTests)
    {
        ImGuiTest* t = IM_REGISTER_TEST(m_engine, reg.category, reg.name);
        t->UserData = const_cast<DiagReg*>(&reg);
        t->TestFunc = [](ImGuiTestContext* ctx)
        {
            const DiagReg* r = FindReg(ctx->Test);
            if (r == nullptr) return;

            char head[192];
            std::snprintf(head, sizeof(head), "診断開始: %s / %s", r->group, r->display);
            CrashHandler::Breadcrumb(head);
            LogWatchBegin();

            r->body(ctx);

            // クラッシュしなくてもエラーログが出ていれば残す（パネルに黄色で出る）
            const std::string errors = LogWatchEnd();
            if (!errors.empty())
            {
                g_testNotes[r->name] = errors;
                ctx->LogWarning("実行中にエラーログが出ました (%d 文字)", static_cast<int>(errors.size()));
            }
            CrashHandler::Breadcrumb("診断完了");
        };
    }
}

} // namespace dx12e
