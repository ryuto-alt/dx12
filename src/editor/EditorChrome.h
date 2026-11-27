#pragma once

// 計器のクロム（W1-A7 / GG-4）。Graphite Gauge のときだけ描く。旧テーマの描画は 1 px も変えない。
//   ・タイトルバー下端の主尺（細 8px / 主 40px）と、その上のフォーカス・キャリパー（黄の予算 01）
//   ・タイトルバーのロゴの矩形（LogoRectScreen: W2 の起動画面→タイトルバーの引き渡し BA-7 の目標）
//   ・Windows 11 のダークタイトルバー（ImGuiManager が variant の確定 / 切替で呼ぶ。実体は Window.cpp）
// 純ロジックは EditorChromeLogic.h（単体テスト対象）。

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

namespace dx12e
{

class EditorChrome
{
public:
    // Graphite（GgDeco::caliper が ON）なら true。
    static bool Active();

    // タイトルバーのロゴ（16 論理 px）の矩形。スクリーン座標の物理 px。
    //   ToolbarPanel が描いたフレームの値を返す。まだ描かれていなければ既定位置（メインビューポート左上の 8px 内側、縦中央）。
    static ImRect LogoRectScreen();
    static void   NoteLogoRect(ImVec2 mn, ImVec2 mx);
    // ロゴの一辺（物理 px）。Graphite は 16 論理 px、旧テーマは従来の 20 論理 px。
    static float  LogoSizePx();

    // タイトルバー（メニューバー）の矩形 bar に、主尺とフォーカス・キャリパーを描く。Active() でなければ何もしない。
    static void DrawTitleBar(const ImRect& bar, ImDrawList* dl);

    // キャリパーをかけるパネルの横範囲（物理 px）。フォーカス中のパネルが決まらないときは直前の値を保つ。
    static bool FocusedPanelSpan(float& x0, float& x1, bool& focusInWindow);

    // 「主尺のみ」の設定（キャリパーを出さない）。既定は GgDeco::caliper。
    static void SetCaliperEnabled(bool on);
    static bool CaliperEnabled();
};

} // namespace dx12e
