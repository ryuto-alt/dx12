#include "gui/UiTestHarness.h"
#include "core/Application.h"
#include "core/CrashHandler.h"
#include "core/Logger.h"
#include "core/Version.h"
#include "editor/EditorContext.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_engine.h>
#include <imgui_te_context.h>
#include <imgui_te_ui.h>
#include <imgui_te_utils.h>
#include <imgui_te_exporters.h>
#pragma warning(pop)

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <vector>

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
    ctx->SetRef(kWinHierarchy);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, "", 3);
    for (int i = 0; i < items.GetSize() && i < 12; ++i)
    {
        const ImGuiTestItemInfo* item = items[i];
        if (item == nullptr || item->ID == 0) continue;
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

void T_PlayStopCycle(ImGuiTestContext* ctx)
{
    IM_CHECK(g_app != nullptr);

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
    }
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
        IM_CHECK_NO_RET(fs::exists(fs::path(buildDir) / "Game.exe"));
        IM_CHECK_NO_RET(fs::exists(fs::path(buildDir) / "game.pak"));
    }

    fs::remove_all(outDir, ec);   // 一時フォルダを掃除（失敗しても無視）
}

// ---- ストレス ----

void T_RapidSelection(ImGuiTestContext* ctx)
{
    Step(ctx, "選択を高速に切り替える");
    ctx->SetRef(kWinHierarchy);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, "", 3);
    IM_CHECK_NO_RET(items.GetSize() > 0);

    for (int pass = 0; pass < 6; ++pass)
    {
        for (int i = 0; i < items.GetSize() && i < 10; ++i)
        {
            const ImGuiTestItemInfo* item = items[i];
            if (item == nullptr || item->ID == 0) continue;
            if (std::strstr(item->DebugLabel, "✚") != nullptr) continue;
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

// ===================== テスト表 =====================

struct DiagReg
{
    const char* category;   // ImGuiTestEngine 上のカテゴリ（ASCII）
    const char* name;       // ImGuiTestEngine 上のテスト名（ASCII）
    const char* group;      // 診断パネルの見出し（日本語）
    const char* display;    // 診断パネルの行名（日本語）
    void (*body)(ImGuiTestContext*);
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
    { "panel", "layout_reset",          "パネル",             "ドックレイアウトのリセット",           T_LayoutReset           },

    { "uied",  "ui_spawn_all",          "UI エディタ",        "UI 要素を全種類配置",                  T_UiEditorSpawnAll      },
    { "uied",  "ui_view_ops",           "UI エディタ",        "ズーム / グリッド / 極小リサイズ",     T_UiEditorViewOps       },

    { "vfx",   "vfx_all_kinds",         "パーティクルエディタ", "見た目 / 合成 / 向きを全通り",       T_VfxEditorAllKinds     },
    { "vfx",   "vfx_apply_spawn",       "パーティクルエディタ", "選択へ適用 / 新規配置",              T_VfxApplyAndSpawn      },

    { "mat",   "material_editor",       "マテリアル",         "マテリアルエディタ（新規 / プレビュー）", T_MaterialEditor     },
    { "mat",   "material_library",      "マテリアル",         "マテリアルライブラリを開く",           T_MaterialLibrary       },

    { "play",  "play_stop_cycle",       "再生",               "Play → Stop を 2 往復",                T_PlayStopCycle         },

    { "build", "build_game",            "ビルド",             "ゲームをビルドして成果物を確認",       T_BuildGame             },

    { "stress","rapid_selection",       "ストレス",           "選択を高速に切り替える",               T_RapidSelection        },
    { "stress","spam_add_delete",       "ストレス",           "生成と削除を連打",                     T_SpamAddDelete         },
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

void UiTestHarness::Initialize(Application* app, bool runAllAndExit, int speedMode)
{
    m_app           = app;
    m_runAllAndExit = runAllAndExit;
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

void UiTestHarness::QueueTests(const char* filterCategory, bool failedOnly)
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
        QueueTests(nullptr, false);
        m_started = true;
        Logger::Info("UI テスト: 全テストをキューへ投入しました");
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

void UiTestHarness::DrawDiagnosticsPanel(bool* show)
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
    ImGui::Separator();

    // ---- 実行ボタン ----
    ImGui::BeginDisabled(m_running);
    if (ImGui::Button("▶ すべて検査する", ImVec2(180, 36)))
        QueueTests(nullptr, false);
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
