#pragma once

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

// ---- 共有ヘルパ（定義は UiTestHarness.cpp）----
Application*   App();        // テスト中の Application（無ければ nullptr）
EditorContext* Editor();     // App()->GetEditorContext()（無ければ nullptr）
// テストの手順をクラッシュレポートとログへ残す（UiTestHarness.cpp の Step と同じ）
void LogStep(ImGuiTestContext* ctx, const char* fmt, ...);

// ---- 対象ウィンドウ（先頭の "//" は絶対参照。UiTestHarness.cpp の kWin* と同じ値）----
inline constexpr const char* kWinHierarchy = "//ヒエラルキー";
inline constexpr const char* kWinInspector = "//インスペクター";
inline constexpr const char* kWinConsole   = "//コンソール";
inline constexpr const char* kWinAssets    = "//アセットブラウザ";
inline constexpr const char* kWinToolbar   = "//##Toolbar";

// ---- 各レーンのテスト本体の宣言（void(ImGuiTestContext*)）----
// ==== lane-A:decl begin ====
// ==== lane-A:decl end ====
// ==== lane-B:decl begin ====
// ==== lane-B:decl end ====
// ==== lane-C:decl begin ====
// ==== lane-C:decl end ====
// ==== lane-D:decl begin ====
// ==== lane-D:decl end ====

} // namespace uitest
} // namespace dx12e
