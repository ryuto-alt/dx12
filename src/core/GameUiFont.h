#pragma once

// ===== ゲーム UI（UIText / Lua の ui.text）が使う既定フォント =====
// エディタの UI フォント（Segoe UI + Yu Gothic UI。editor/EditorTheme.h）とは別物。
// ★ゲーム UI の文字の幅・行高は「出荷したゲーム」（pak のフォント / 無ければ Yu Gothic Medium 17px）で決まる。
//   エディタの Play / UI 編集プレビューが別のフォントで測ると、エディタでは収まった文字が出荷版で
//   はみ出す（WYSIWYG が崩れる）ので、エディタでは旧来の Yu Gothic Medium を別フォントとして持ち、
//   ゲーム UI の描画にはこちらを使う。
// null のとき（配布ゲーム = ImGui の既定フォントがそのままゲームのフォント）は ImGui::GetFont() を使うこと。

struct ImFont;

namespace dx12e
{
inline ImFont*& GameUiDefaultFont()
{
    static ImFont* s_font = nullptr;
    return s_font;
}
} // namespace dx12e
