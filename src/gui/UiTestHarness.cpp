#include "gui/UiTestHarness.h"
#include "core/Application.h"
#include "core/CrashHandler.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/Version.h"
#include "ecs/Components.h"
#include "editor/EditorContext.h"
#include "gui/DeepDiagnostics.h"
#include "scene/Scene.h"
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

// パーティクルエディタは中身を BeginChild で 3 分割している。子ウィンドウの中の項目は
// 親窓からの ID パスでは引けない（子窓の実 ID は "親名/str_id_XXXXXXXX" のハッシュ）ので、
// WindowInfo に子窓まで辿らせて ID を取ってから触る。
const char* kWinVfxAssetList = "//パーティクルエディタ###VfxEditorPanelFloating/##VfxAssetList";
const char* kWinVfxMain      = "//パーティクルエディタ###VfxEditorPanelFloating/##VfxMain";
const char* kWinVfxEmission  = "//パーティクルエディタ###VfxEditorPanelFloating/##VfxMain/##VfxEmission";

const char* kBtnAddEntity    = "✚ エンティティ追加";
const char* kBtnAddComponent = "✚ コンポーネント追加";
const char* kMenuScript      = "スクリプト";

// ===================== 実行中の状態 =====================
// テスト関数はキャプチャ不可のラムダ（関数ポインタ）なので、共有状態はファイル静的に置く。
Application* g_app = nullptr;

// 各テストが実行中に吐いたエラーログ。クラッシュしなくても「静かに壊れている」を拾うため、
// テスト名 → 収集した行、で残して診断パネルに出す（失敗扱いにはしない＝誤検知で赤くしない）。
std::map<std::string, std::string> g_testNotes;
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
        if (std::strstr(item->DebugLabel, "✚") != nullptr) continue;
        return item;
    }
    return nullptr;
}

// ヒエラルキーに並ぶエンティティ行を集めて、順にクリックする。
// 落ちる/固まるのはたいてい「選択が変わった次のフレームで Inspector が描く」瞬間なので、
// 1 件ごとに数フレーム回して Inspector 側の描画まで到達させる。
void ClickEveryHierarchyItem(ImGuiTestContext* ctx, int maxItems)
{
    ctx->SetRef(kWinHierarchy);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, "", 3);

    int clicked = 0;
    for (int i = 0; i < items.GetSize(); ++i)
    {
        const ImGuiTestItemInfo* item = items[i];
        if (clicked >= maxItems) break;
        if (item == nullptr || item->ID == 0 || item->Window == nullptr) continue;
        if (std::strstr(item->DebugLabel, "✚") != nullptr) continue;

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
        // ラベルに '/' を含むものがあるため、名前ではなくラベル一致 → ID でクリックする
        if (!ClickPopupItemContaining(ctx, comp))
            ctx->KeyPress(ImGuiKey_Escape);
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

void T_OpenAllToolWindows(ImGuiTestContext* ctx)
{
    IM_CHECK(Ed() != nullptr);

    // 「フラグ → 窓」。ツールメニュー経由ではなく直接立てる（メニューにはダイアログ項目が混じるため）
    // optional=true: 環境によって存在しない窓（MCP ブリッジ未起動など）。出なくても失敗にしない。
    // textOnlyOk=true: 中身が案内テキストだけになる状態が正常な窓。GatherItems は ID 付き
    //   アイテムしか拾わない（TextDisabled は ID 無し）ので、空判定から除外する。
    struct Entry { bool EditorContext::* flag; const char* window; const char* label; bool optional; bool textOnlyOk; };
    static const Entry kEntries[] = {
        { &EditorContext::showPostProcess,     "//Post Process",            "Post Process",           false },
        // マスター OFF / 有効エフェクト 0 件なら案内文だけ＝正常（Application.cpp の showPostParams 参照）
        { &EditorContext::showPostParams,      "//Post Process パラメータ", "Post Process パラメータ", false, true },
        { &EditorContext::showSkybox,          "//Skybox / IBL",            "Skybox / IBL",           false },
        { &EditorContext::showSSAO,            "//SSAO",                    "SSAO",                   false },
        { &EditorContext::showScreenSpaceGi,   "//SSR / SSGI",              "SSR / SSGI",             false },
        { &EditorContext::showVolumetricFog,   "//Volumetric Fog",          "Volumetric Fog",         false },
        { &EditorContext::showEngineSettings,  "//エンジン設定",            "エンジン設定",           false },
        { &EditorContext::showSceneFlow,       "//Scene Flow",              "Scene Flow",             false },
        { &EditorContext::showProject,         "//Project",                 "Project",                false },
        { &EditorContext::showVersionControl,  "//Git 変更###Version Control (Git)", "Git 変更",      false },
        { &EditorContext::showMcpBridge,       "//MCP / AI Bridge",         "MCP / AI Bridge",        true  },
        { &EditorContext::showNetworkStatus,   "//Network",                 "Network",                false },
        { &EditorContext::showNetworkSettings, "//Network 設定",            "Network 設定",           false },
        { &EditorContext::showBuildSettings,   kWinBuild,                   "ビルド設定",             false },
        // ※ 診断パネル自身はここに入れない。検査中にユーザーが見ている窓を勝手に開閉・
        //   フォーカス移動させると「検査の途中で UI が消えた」ように見えるため。
        //   パネルの描画自体は検査中ずっと画面に出ている＝毎フレーム検査されている。
    };

    // まとめて開く。1つずつ開閉すると「ツール窓が 0 個」になった瞬間に右下ドックノードが
    // 畳まれ、次の窓が数フレーム 0 サイズ扱い＝「出ていない」と誤判定されるため。
    // 元の開閉状態は最後に戻す（診断パネル自身を閉じてしまわないように）。
    Step(ctx, "ツール窓をすべて開く");
    bool wasOpen[IM_ARRAYSIZE(kEntries)] = {};
    for (int i = 0; i < IM_ARRAYSIZE(kEntries); ++i)
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
        const ImGuiID id = FocusWindow(ctx, e.window);
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

    for (int i = 0; i < IM_ARRAYSIZE(kEntries); ++i)
        Ed()->*(kEntries[i].flag) = wasOpen[i];
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
        Step(ctx, "アセットブラウザの項目をクリック: %s", item->DebugLabel);
        ctx->ItemClick(item->ID);
        ctx->Yield(6);   // 一覧の再構築 + サムネイル生成まで進める
        ++clicked;
    }
    ctx->LogInfo("clicked %d asset browser items", clicked);
    IM_CHECK_NO_RET(clicked > 0);
    ctx->Yield(6);
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

void T_LayoutReset(ImGuiTestContext* ctx)
{
    EditorContext* ed = Ed();
    IM_CHECK(ed != nullptr);
    Step(ctx, "ドックレイアウトをリセット（ドックツリーの再構築）");
    ed->resetLayout = true;
    ctx->Yield(20);
    IM_CHECK_NO_RET(!ed->resetLayout);   // 消費されていない＝リセットが走っていない
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
    for (const char* combo : { "見た目 Kind", "合成 Blend", "向き Orient" })
    {
        if (!ItemExistsIn(ctx, emission, combo))
        {
            ctx->LogWarning("コンボが見つかりません(スキップ): %s", combo);
            continue;
        }
        Step(ctx, "%s を全通り切り替える", combo);
        ctx->SetRef(emission);
        ctx->ComboClickAll(combo);
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
            std::wstring cmd = exePath.wstring();
            std::wstring cwd = fs::path(buildDir).wstring();
            STARTUPINFOW si{};
            si.cb = sizeof(si);
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
            if (std::strstr(item->DebugLabel, "✚") != nullptr) continue;
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
    { "basic", "spawn_all_types",       "基本操作",           "全種類のエンティティを生成",           T_SpawnAllEntityTypes   },
    { "basic", "inspector_open_all",    "基本操作",           "インスペクターの全セクションを開く",   T_InspectorOpenAll      },
    { "basic", "undo_redo",             "基本操作",           "Undo / Redo を往復",                   T_UndoRedoStress        },
    { "basic", "edit_menu_ops",         "基本操作",           "コピー / 貼り付け / 複製 / 削除",      T_EditMenuOps           },
    { "basic", "gizmo_view_modes",      "基本操作",           "ギズモ切替と 2D ビュー",               T_GizmoAndViewModes     },

    { "comp",  "add_all_components",    "コンポーネント",     "全コンポーネントを追加",               T_AddAllComponents      },
    { "comp",  "attach_script",         "コンポーネント",     "スクリプトを追加",                     T_AttachScript          },

    { "panel", "open_all_tool_windows", "パネル",             "すべてのツール窓を開いて描画",         T_OpenAllToolWindows    },
    { "panel", "console",               "パネル",             "コンソール（フィルタ / Lua 実行）",    T_ConsolePanel          },
    { "panel", "asset_browser",         "パネル",             "アセットブラウザ",                     T_AssetBrowser          },
    { "panel", "new_floating_panels",   "パネル",             "ライティング / 地形ツールの開閉",       T_NewFloatingPanels     },
    { "panel", "layout_reset",          "パネル",             "ドックレイアウトのリセット",           T_LayoutReset           },
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

    std::string report = "==== DX12 Engine エンジン診断 結果 ====\n";
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
    ImGui::SetNextWindowSize(ImVec2(680, 620), ImGuiCond_FirstUseEver);
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
    if (ImGui::Button("▶ すべて検査する", ImVec2(180, 36)))
        QueueTests(nullptr, false, 0);
    ImGui::SameLine();
    if (ImGui::Button("🔬 超詳細診断", ImVec2(150, 36)))
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
    if (ImGui::Button("失敗だけ再検査", ImVec2(140, 36)))
        QueueTests(nullptr, true);
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("結果をコピー", ImVec2(130, 36)))
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
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 130.0f);
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
        const float kNameColumnX = 96.0f;
        ImGui::Indent(8.0f);
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
        ImGui::Unindent(8.0f);
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
                ImGui::Indent(24.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.5f, 1.0f));
                ImGui::TextWrapped("%s", buf.c_str());
                ImGui::PopStyleColor();
                ImGui::Unindent(24.0f);
            }
        }

        // 実行中に出たエラーログ（落ちてはいないが静かに壊れている、を拾う）
        auto note = g_testNotes.find(test->Name);
        if (note != g_testNotes.end() && !note->second.empty())
        {
            ImGui::Indent(24.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
            ImGui::TextWrapped("実行中のエラーログ:\n%s", note->second.c_str());
            ImGui::PopStyleColor();
            ImGui::Unindent(24.0f);
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
