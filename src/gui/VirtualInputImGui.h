#pragma once

// ===========================================================================
// 仮想入力モード: ImGui への流し込み・仮想カーソルの描画・ウィンドウ探索
// ---------------------------------------------------------------------------
// 純粋ロジック（キュー / 座標変換 / キー名）は input/VirtualInput.h。ここは ImGui に触る部分だけ。
// GPU にも Win32 にも触らない（ImGui コンテキストさえあれば動く）＝ヘッドレスでユニットテストできる。
// ===========================================================================

#include "input/VirtualInput.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <functional>
#include <string>
#include <vector>

namespace dx12e::vinput_gui
{

// VK コード → ImGuiKey（修飾キーは別扱い）。対応なしは ImGuiKey_None。
ImGuiKey VkToImGuiKey(int vk);

struct ApplyContext
{
    ImVec2  viewportPos{0.0f, 0.0f};    // GetMainViewport()->Pos（マルチビューポート有効時はスクリーン座標）
    ImVec2  displaySize{0.0f, 0.0f};    // クライアント領域のサイズ（クランプ用）
    ImGuiID mainViewportId = 0;         // 「マウスは常にメインビューポート上」と ImGui に伝える
    // キー押下を InputSystem（VK ベースの入力）へも配るためのフック。null 可。
    std::function<void(int vk, bool down)> keySink;
};

// 1 フレームぶんのイベントを ImGuiIO のイベントキューへ積む（ImGui::NewFrame の直前に呼ぶ）。
// 空フレームでも「ホバー中のビューポート」だけは毎回伝える。
void ApplyFrame(ImGuiIO& io, const vinput::Frame& frame, const ApplyContext& ctx);

// ImGui のイベントキューの現在長 / 切り詰め。
//   Win32 バックエンドの NewFrame は「実マウス位置の取り込み」「修飾キーの解放」等を
//   キューへ積むので、仮想モード中は NewFrame の前後で長さを測って増えた分を捨てる
//   （実入力が仮想入力を上書きしないようにする）。
int  InputQueueSize();
void TruncateInputQueue(int keep);

// 仮想モードの ON/OFF に伴う ImGui 側の設定（OS カーソル形状の変更禁止・フォーカス固定）。
void SetImGuiVirtualMode(bool on);

// 仮想カーソル（矢印 + クリックの波紋）を ForegroundDrawList(メインビューポート)へ描く。
// 仮想モード ON の間だけ呼ぶこと。ImGui::Render の直前。
void DrawVirtualCursor();
// テスト用: 波紋の内部状態を捨てる。
void ResetVirtualCursorState();
// 描画した要素数（テスト用。矢印 + 波紋）。
int  LastCursorPrimCount();

// ---- find: ImGui のウィンドウ / タブの一覧 ----
struct WindowInfo
{
    std::string name;       // ImGui 内部名（"Title###id" の全体）
    std::string title;      // 表示名（"##" 以降を除いたもの）
    float x = 0, y = 0, w = 0, h = 0;        // ImGui 座標（スクリーン座標）
    bool  visible = false;                    // 今フレーム描かれている（Active && !Hidden）
    bool  focused = false;
    bool  hovered = false;
    bool  collapsed = false;
    bool  docked = false;
    bool  dockTabVisible = false;             // ドック内で「選ばれているタブ」か
    bool  hasTabRect = false;                 // docked のとき、タブ見出しの矩形
    float tx = 0, ty = 0, tw = 0, th = 0;
    bool  isPopup = false;
    bool  isChild = false;
};

// 直近のフレームのウィンドウ一覧。includeChildren=false で子ウィンドウ（Child）を除く。
std::vector<WindowInfo> CollectWindows(bool includeChildren);

struct HoverInfo
{
    std::string hoveredWindow;        // マウスが乗っているルートウィンドウ名（無ければ空）
    std::string focusedWindow;
    bool wantCaptureMouse = false;
    bool wantCaptureKeyboard = false;
    bool wantTextInput = false;       // 文字入力欄がアクティブ（text を送れる）
    ImGuiID activeId = 0;
    ImGuiID hoveredId = 0;
};
HoverInfo QueryHoverInfo();

// name / title が label に合うか（大小無視。contains=false は完全一致）。
bool MatchLabel(const std::string& text, const std::string& label, bool contains);


// ---- find 用のアンカー登録（パネルの描画コードから呼ぶ。仮想入力モード OFF の間は即 return）----
// 直前に描いたアイテムの矩形を「名前つき要素」として登録する。クリップされて見えていない要素は登録しない
// （スクロールで隠れた行を AI が狙ってしまうのを防ぐ）。
void AnchorLastItem(const char* kind, const char* label);
// プロパティ行: ラベル文字の矩形 + 値欄の矩形（クリック対象は値欄）。
void AnchorProperty(const char* label, ImVec2 labelMin, ImVec2 labelMax, ImVec2 valueMin, ImVec2 valueMax);
} // namespace dx12e::vinput_gui
