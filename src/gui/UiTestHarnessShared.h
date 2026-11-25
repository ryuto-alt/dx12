#pragma once

#include <string>

#include "editor/EditorIcons.h"   // ICON_PLUS（kBtnAdd*）

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>                // ImGuiID
#pragma warning(pop)

// UI テスト（ImGuiTestEngine）の共有部。
// テストの本体（T_*）は UiTestHarness.cpp の無名名前空間（5,600 行超）に置かれていて、
// そのままでは各レーンが表の外へ関数を足すことになる。そこで、レーンのテストは
// src/gui/uitests/lane_{a,b,c,d}.cpp に置き、ここの共有ヘルパと宣言を通して書く。
//
//   - 各レーンの本体の宣言は、下の自分のレーンのブロック（lane-X:decl）に足す。
//   - 登録は UiTestHarness.cpp の kTests の自分のレーンのブロックに 1 行足す。
//   - 共有ヘルパを足したいときは「依頼」としてメインへ（ここは L1 が所有）。

struct ImGuiTestContext;   // グローバル名前空間（imgui_test_engine 提供）

namespace dx12e
{

class Application;
class EditorContext;

namespace uitest
{

// ---- 共有ヘルパ（定義は UiTestHarness.cpp 末尾の uitest 区画。中身は無名名前空間の同名ヘルパへの転送）----
Application*   App();        // テスト中の Application（無ければ nullptr）
EditorContext* Editor();     // App()->GetEditorContext()（無ければ nullptr）
// テストの手順をクラッシュレポートとログへ残す（UiTestHarness.cpp の Step と同じ）
void LogStep(ImGuiTestContext* ctx, const char* fmt, ...);

// 窓を前面にして ID を返す（0 = 窓が出ていない）。windowRef は "//窓名" 形式（kWin*）
ImGuiID FocusWindow(ImGuiTestContext* ctx, const char* windowRef);
// "//窓名/##子ID" 形式から子ウィンドウ込みで窓 ID を引く
ImGuiID ChildWindowId(ImGuiTestContext* ctx, const char* pathRef);
// 窓の中の項目の有無（ID パスで引く。ラベル走査はしない）
bool ItemExistsIn(ImGuiTestContext* ctx, ImGuiID windowId, const char* path);
// 窓の中の項目をクリック（無ければテスト失敗）
bool ClickPath(ImGuiTestContext* ctx, ImGuiID windowId, const char* path);
// 開いているポップアップの項目のうち、ラベルに needle を含むものをクリック
bool ClickPopupItemContaining(ImGuiTestContext* ctx, const char* needle);
// ヒエラルキーの「エンティティ追加」から type を追加 / 最後の行を選択
void AddEntity(ImGuiTestContext* ctx, const char* type);
void SelectLastEntity(ImGuiTestContext* ctx);
// EditorContext のトグルで窓を開く（前面に出して ID を返す。開けなければテスト失敗）/ 閉じる
ImGuiID OpenToolWindow(ImGuiTestContext* ctx, bool EditorContext::* flag, const char* windowRef);
void CloseToolWindow(ImGuiTestContext* ctx, bool EditorContext::* flag);
// テスト中に出たエラーログの収集（Begin で開始し、End で収集分を返す）
void LogWatchBegin();
std::string LogWatchEnd();

// ---- 対象ウィンドウ・ボタン名（先頭の "//" は絶対参照。値の唯一の定義はここ。UiTestHarness.cpp も using で使う）----
inline constexpr const char* kWinHierarchyName = "ヒエラルキー";   // ImGui::Begin に渡している生の名前
inline constexpr const char* kWinHierarchy   = "//ヒエラルキー";
inline constexpr const char* kWinInspector   = "//インスペクター";
inline constexpr const char* kWinConsole     = "//コンソール";
inline constexpr const char* kWinAssets      = "//アセットブラウザ";
inline constexpr const char* kWinToolbar     = "//##Toolbar";
inline constexpr const char* kWinVfx         = "//パーティクルエディタ###VfxEditorPanelFloating";
inline constexpr const char* kWinUiEditor    = "//UIエディタ###UiEditorPanelFloating";
inline constexpr const char* kWinMaterial    = "//マテリアルエディタ###MaterialEditorFloating";
inline constexpr const char* kWinMatLibrary  = "//マテリアルライブラリ (Poly Haven)###MaterialLibraryFloating";
inline constexpr const char* kWinBuild       = "//ビルド設定";
inline constexpr const char* kWinLighting    = "//ライティング###LightingPanelFloating";
inline constexpr const char* kWinTerrain     = "//地形ツール###TerrainToolFloating";
inline constexpr const char* kWinSculpt      = "//スカルプト（異形）###SculptToolFloating";
inline constexpr const char* kWinNavMesh     = "//ナビメッシュ###NavMeshFloating";
inline constexpr const char* kWinAudioMixer  = "//オーディオミキサー###AudioMixerFloating";
inline constexpr const char* kWinVfxAssetList = "//パーティクルエディタ###VfxEditorPanelFloating/##VfxAssetList";
inline constexpr const char* kWinVfxMain      = "//パーティクルエディタ###VfxEditorPanelFloating/##VfxMain";
inline constexpr const char* kWinVfxEmission  = "//パーティクルエディタ###VfxEditorPanelFloating/##VfxMain/##VfxEmission";
inline constexpr const char* kBtnAddEntity    = ICON_PLUS " エンティティ追加";      // EditorIcons.h（Lucide グリフ + ラベル）
inline constexpr const char* kBtnAddComponent = ICON_PLUS " コンポーネント追加";
inline constexpr const char* kMenuScript      = "スクリプト";

// ---- 各レーンのテスト本体の宣言（void(ImGuiTestContext*)）----
// ==== lane-A:decl begin ====
// ==== lane-A:decl end ====
// ==== lane-B:decl begin ====
void T_SaveAsOverwriteConfirm(ImGuiTestContext* ctx);   // W05-12
void T_NewSceneNameKept(ImGuiTestContext* ctx);         // W05-12
void T_NewSceneCancelKeepsNoPath(ImGuiTestContext* ctx); // W05-12
// ==== lane-B:decl end ====
// ==== lane-C:decl begin ====
void T_CreateEntitySelects(ImGuiTestContext* ctx);   // W05-21
// ==== lane-C:decl end ====
// ==== lane-D:decl begin ====
// ==== lane-D:decl end ====

} // namespace uitest
} // namespace dx12e
