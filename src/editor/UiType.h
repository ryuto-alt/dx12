#pragma once

// ===== 書体の役割（W1-A14 / GG-2b）=====
// Graphite Gauge の typography.scale の役割ごとに、書体（ImFont）とサイズを選んで ImGui::PushFont する。
//   ui::type::Push(Role::BodyBold);  ...  ui::type::Pop();
// ・書体のハンドルは theme::g_fonts（ImGuiManager が起動時に埋める。無ければ現在のフォント）。
// ・サイズは論理 px（旧案は本文 16 / 等幅 14、Graphite は 17 / 15。見出しは本文の比で出す）。DPI 倍率は ImGui の FontScaleDpi が掛ける。
// ・数値（Numeric）は必ず等幅。桁が増えても表の列がずれない。
// ・旧書体（--ui-font legacy）でも呼べる（同じ ImFont に差し替わるだけ。サイズは同じ比で出る）。

#include "editor/EditorTheme.h"

#include <imgui.h>

namespace dx12e::ui::type
{

enum class Role
{
    Caption,    // 補助 / ツールチップの説明 / ステータスバー（本文より小さい）
    Body,       // 標準の本文
    BodyBold,   // 選択中の名前 / 強調（SemiBold）
    H2,         // コンポーネント見出し / セクション（SemiBold）
    H1,         // ダイアログ / ようこそ / About の題（SemiBold）
    Mono,       // 数値 / 時刻 / ログ
    Numeric,    // 数値（等幅。Mono と同じ書体・サイズ。桁揃えが要る場所用の別名）
};

// 役割に対応する書体。null のときは現在のフォント（呼び出し側で ImGui::GetFont() を使う）。
inline ImFont* FontOf(Role r)
{
    namespace th = dx12e::theme;
    switch (r)
    {
    case Role::BodyBold:
    case Role::H2:
    case Role::H1:      return th::g_fonts.bold;
    case Role::Mono:
    case Role::Numeric: return th::g_fonts.mono;
    default:            return th::g_fonts.body;
    }
}

// 役割のサイズ（論理 px）。本文 = FontBaseOf()、Caption = 本文 - 2、H2 = 本文 + 1、H1 = 本文 + 6（Graphite の 15 / 17 / 18 / 23 に一致）、等幅 = FontMonoOf()。
inline float SizeOf(Role r)
{
    namespace th = dx12e::theme;
    const float base = th::FontBaseOf();
    switch (r)
    {
    case Role::Caption: return base - 2.0f;
    case Role::H2:      return base + 1.0f;
    case Role::H1:      return base + 6.0f;
    case Role::Mono:
    case Role::Numeric: return th::FontMonoOf();
    default:            return base;
    }
}

// 書体とサイズを積む。Pop() と必ず対にする。サイズは DPI 倍率の前の論理 px（ImGui が倍率を掛ける）。
inline void Push(Role r)
{
    ImFont* f = FontOf(r);
    ImGui::PushFont(f ? f : ImGui::GetFont(), SizeOf(r));
}
inline void Pop() { ImGui::PopFont(); }

// RAII 版。
struct Scope
{
    explicit Scope(Role r) { Push(r); }
    ~Scope() { Pop(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace dx12e::ui::type
