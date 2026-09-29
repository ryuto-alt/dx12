#pragma once

// ===== エディタ UI テーマ（Unreal Editor 5 風・ダーク）=====
// エディタ全体（ImGui スタイル + 各パネルのアイコン tint / アクセント / 面の階調）で
// 共有する単一ソース。色・寸法・フォントのハンドルはここだけから引く。
// ヘッダオンリー（リンク依存なし）。ImGuiManager.cpp が ApplyStyle() で ImGuiStyle へ流し込む。
//
// 設計の要点（詳細は dx12-ui-audit/WAVE1_REPORT.md）:
//   ・面の階調で区切る: Bg0(最深) < Bg1(パネル) < Bg2(ヘッダ/非選択タブ/ポップアップ) < Bg3(ホバー)。
//     入力欄は逆にパネルより暗い「凹み」(InputBg)。線ではなく面で領域を作る。
//   ・アクセント青は「選択の面 / フォーカス枠 / アクティブなトグル」だけに使う（乱用しない）。
//   ・文字の 3 段(Text / TextDim / TextFaint)は各背景で WCAG AA 4.5:1 以上（実測は WAVE1_REPORT）。

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace dx12e::theme
{

// 0xRRGGBB → ImVec4（sRGB のまま。ImGui の頂点カラーに合わせてガンマ変換はしない）
inline ImVec4 Hex(unsigned int rgb, float a = 1.0f)
{
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                  ((rgb >> 8)  & 0xFF) / 255.0f,
                  ( rgb        & 0xFF) / 255.0f,
                  a);
}

// ---- 面（深 → 浅）----
inline const ImVec4 Bg0         = Hex(0x0E0E10);  // 最深部: ビューポート周辺 / タブ井戸 / ステータスバー / ドックの隙間
inline const ImVec4 Bg1         = Hex(0x171719);  // パネル本体
inline const ImVec4 Bg2         = Hex(0x1F1F23);  // ヘッダ帯 / 非選択タブ / ポップアップ / 行ゼブラ
inline const ImVec4 Bg3         = Hex(0x2A2A2F);  // ホバー面 / ボタン地
inline const ImVec4 Bg4         = Hex(0x34343B);  // ボタンのホバー / 押下（Bg3 の一段上）
inline const ImVec4 InputBg     = Hex(0x0C0C0E);  // 入力欄（パネルより暗い凹み）
inline const ImVec4 InputBgHover  = Hex(0x111114);
inline const ImVec4 InputBgActive = Hex(0x141418);
inline const ImVec4 InputBorder      = Hex(0x2E2E34);  // 入力欄の 1px 枠
inline const ImVec4 InputBorderHover = Hex(0x46464E);
inline const ImVec4 Border      = Hex(0x2A2A2F);  // 面の境界（控えめ）
inline const ImVec4 BorderStrong = Hex(0x3A3A42); // ポップアップ / フローティング窓の枠

// ---- アクセント（UE5 の選択青）----
inline const ImVec4 Accent        = Hex(0x2F8CFF);
inline const ImVec4 AccentHover   = Hex(0x57A3FF);
inline const ImVec4 AccentPressed = Hex(0x1F6FD6);
inline const ImVec4 Selection      = Hex(0x2F8CFF, 0.30f);  // 選択行の面
inline const ImVec4 SelectionHover = Hex(0x2F8CFF, 0.16f);
inline const ImVec4 SelectionActive = Hex(0x2F8CFF, 0.42f);

// ---- テキスト（3 段。TextHi/TextMid は旧名の互換）----
inline const ImVec4 Text        = Hex(0xD6D6DB);
inline const ImVec4 TextDim     = Hex(0x9C9CA6);
inline const ImVec4 TextFaint   = Hex(0x90909A);   // 実測: Bg0-Bg3 / 入力欄の上で 4.5:1 以上（旧 #74767f は 3.96）
inline const ImVec4 TextHi      = Text;
inline const ImVec4 TextMid     = Hex(0xBEBEC7);   // ラベル（Text と TextDim の間）

// ---- ステータス色 ----
inline const ImVec4 Good        = Hex(0x4CBF7A);
inline const ImVec4 Warn        = Hex(0xE5A03C);
inline const ImVec4 Bad         = Hex(0xEE6A62);   // 文字色として Bg1-Bg3 で 4.5:1 以上になるよう少し明るく（初期値 #E5534B は Bg2 で 4.44）

// ---- 種別カラー（アイコングリフの tint。「控えめな色味」= 彩度を落とし、全部同じ青にしない）----
inline const ImVec4 TypeMesh    = Hex(0xA9AFBC);   // 中性（メッシュは数が多いので色を主張しない）
inline const ImVec4 TypeLight   = Hex(0xE7B55A);   // 琥珀
inline const ImVec4 TypeCamera  = Hex(0x5FA8FF);   // 青
inline const ImVec4 TypeAudio   = Hex(0x55C4C4);   // ティール
inline const ImVec4 TypeScript  = Hex(0xA994F0);   // 紫
inline const ImVec4 TypePhysics = Hex(0x6DBE7A);   // 緑
inline const ImVec4 TypeUi      = Hex(0xD97BBB);   // マゼンタ
inline const ImVec4 TypePrefab  = Hex(0x6FA8FF);
inline const ImVec4 TypeEmpty   = Hex(0x8E919C);
inline const ImVec4 TypeFolder  = Hex(0xB4B6C0);   // フォルダ（中性の明るいグレー）
inline const ImVec4 TypeScene   = Hex(0xE58A55);   // シーン = 橙

// ---- 旧名の互換（散在する参照を壊さない。値は新トークンへ寄せてある）----
inline const ImVec4 AppBg       = Bg0;
inline const ImVec4 PanelBg     = Bg1;
inline const ImVec4 Chrome      = Bg1;      // ツールバー / メニュー帯（旧: パネルより少し明るい別色）
inline const ImVec4 GroupBg     = Bg2;
inline const ImVec4 FrameBg     = InputBg;
inline const ImVec4 FrameBgHi   = InputBgHover;
inline const ImVec4 FrameBgActive = InputBgActive;
inline const ImVec4 BorderSoft  = InputBorder;
inline const ImVec4 AccentLight = AccentHover;
inline const ImVec4 AccentDim   = SelectionHover;
inline const ImVec4 AccentDim2  = Selection;

// ---- 寸法トークン ----
namespace size
{
inline constexpr float kFontBase     = 16.0f;   // 本文（ImGui の FontSize。em ≒ 12px。行高 = 16）
inline constexpr float kFontMono     = 14.0f;   // 等幅（数値。本文と視覚的な字の大きさが揃うよう 0.875 倍）
inline constexpr float kIconPx       = 16.0f;   // アイコンフォントの標準サイズ
inline constexpr float kRowH         = 23.0f;   // 1 行の標準高（フレーム高 = FontSize + FramePadding.y*2）
inline constexpr float kHeaderH      = 26.0f;   // コンポーネント見出し / セクション帯
inline constexpr float kToolbarBtn   = 28.0f;   // ツールバーのアイコンボタン
inline constexpr float kTitleBarH    = 32.0f;   // メニューバー兼タイトルバー
inline constexpr float kCheckBox     = 16.0f;   // チェックボックスの箱
} // namespace size

// ===========================================================================
// DPI スケール（Windows の表示倍率）
// ---------------------------------------------------------------------------
// ★エディタのピクセル値は「論理 px（= 100% 表示での px）」で書く。ImGui の座標は物理 px なので、
//   ImGui へ渡す寸法・描画座標のオフセットは必ず Px() を通す。
//     ImVec2(28, 28)                →  theme::Px(28, 28)          （ui:: 名前空間からも ui::Px で引ける）
//     ImGui::SetNextItemWidth(120)   →  ImGui::SetNextItemWidth(ui::Px(120))
//     dl->AddLine(a, b, col, 1.5f)   →  dl->AddLine(a, b, col, ui::Px(1.5f))
//   Px() は倍率 100% のとき恒等（小数もそのまま）。それ以外は整数 px へ丸める（にじみ防止）。
//   ★スケールしないもの: 色・アルファ・0..1 の割合・UV・ImVec2(0,0)・ImVec2(-1,0) / -FLT_MIN の「残り全部」記号・
//     ワールド座標・3D ビューポートの描画解像度（物理 px のまま）・GetFontSize()/GetFrameHeight() 等
//     ImGui が既にスケールして返す値・ImGui 内部でスケール済みのスタイル値（ItemSpacing 等）。
//   倍率は ImGuiManager が（OS の表示倍率 or --dpi-scale から）決め、ビューポートごとに切り替える。
// ===========================================================================
inline float g_scale = 1.0f;                                     // 現在の UI 倍率（1.0 = 100%）
inline float Scale() { return g_scale; }
inline void  SetScale(float s) { g_scale = (s > 0.05f) ? s : 1.0f; }
// 論理 px → 物理 px。倍率 1.0 のときは恒等（100% の見た目を 1px も変えない）。
inline float Px(float logical)
{
    if (g_scale == 1.0f) return logical;
    return std::floor(logical * g_scale + 0.5f);
}
inline ImVec2 Px(float x, float y) { return ImVec2(Px(x), Px(y)); }
inline ImVec2 Px(const ImVec2& v)  { return Px(v.x, v.y); }
// 丸めない版（線の太さ・半径など、小数のまま比例させたい値）。
inline float PxF(float logical) { return logical * g_scale; }
// 物理 px → 論理 px（テスト・ログ・ini 用）。
inline float ToLogical(float physical) { return physical / g_scale; }

// multi-viewport で別倍率のモニターに引き出した窓を描くと、その窓の Begin から End まで theme::Scale() が切り替わる
// （ImGuiManager の Platform_OnChangedViewport フック）。メイン窓の座標計算（DockSpace / ステータスバー / トースト）に入る前に
// これを呼んで、倍率をメインビューポートのものへ戻す。全ビューポートが同じ倍率なら何もしない。
inline void (*g_restoreMainScaleFn)() = nullptr;   // ImGuiManager が設定
inline void RestoreMainScale() { if (g_restoreMainScaleFn) g_restoreMainScaleFn(); }

// ---- ImGuiStyle への倍率適用 ----
// 基準スタイル（100% 相当）を 1 回だけ保持し、倍率が変わるたびに【基準から】作り直す（累積させない・往復で元に戻る）。
// 純ロジック部（ScaleStyleFrom）は imgui のコンテキスト不要で単体テストできる。
inline ImGuiStyle& BaseStyle() { static ImGuiStyle s; return s; }
inline bool&       BaseStyleValid() { static bool v = false; return v; }

// 100% スタイル → scale 倍のスタイル。フォントは FontScaleDpi で拡大される（ダイナミックフォント）。
// 行高 kRowH の整数化: FramePadding.y は「Px(kRowH) - フォント高」から逆算する（分数 px の行境界でのにじみ防止）。
inline ImGuiStyle ScaleStyleFrom(const ImGuiStyle& base, float scale)
{
    ImGuiStyle s = base;
    if (scale == 1.0f) return s;               // 100% は基準そのもの（ImTrunc で 3.5→3 になる等の劣化を避ける）
    auto r  = [&](float v) { return std::floor(v * scale + 0.5f); };
    auto r2 = [&](ImVec2 v) { return ImVec2(r(v.x), r(v.y)); };
    s.WindowPadding = r2(base.WindowPadding);   s.WindowRounding = r(base.WindowRounding);
    s.WindowMinSize = r2(base.WindowMinSize);   s.WindowBorderHoverPadding = r(base.WindowBorderHoverPadding);
    s.ChildRounding = r(base.ChildRounding);    s.PopupRounding = r(base.PopupRounding);
    s.FramePadding = r2(base.FramePadding);     s.FrameRounding = r(base.FrameRounding);
    s.ItemSpacing = r2(base.ItemSpacing);       s.ItemInnerSpacing = r2(base.ItemInnerSpacing);
    s.CellPadding = r2(base.CellPadding);       s.TouchExtraPadding = r2(base.TouchExtraPadding);
    s.IndentSpacing = r(base.IndentSpacing);    s.ColumnsMinSpacing = r(base.ColumnsMinSpacing);
    s.ScrollbarSize = r(base.ScrollbarSize);    s.ScrollbarRounding = r(base.ScrollbarRounding);
    s.ScrollbarPadding = r(base.ScrollbarPadding);
    s.GrabMinSize = r(base.GrabMinSize);        s.GrabRounding = r(base.GrabRounding);
    s.LogSliderDeadzone = r(base.LogSliderDeadzone);
    s.ImageRounding = r(base.ImageRounding);    s.ImageBorderSize = r(base.ImageBorderSize);
    s.TabRounding = r(base.TabRounding);
    s.TabMinWidthBase = r(base.TabMinWidthBase); s.TabMinWidthShrink = r(base.TabMinWidthShrink);
    if (base.TabCloseButtonMinWidthSelected > 0.0f && base.TabCloseButtonMinWidthSelected != FLT_MAX)
        s.TabCloseButtonMinWidthSelected = r(base.TabCloseButtonMinWidthSelected);
    if (base.TabCloseButtonMinWidthUnselected > 0.0f && base.TabCloseButtonMinWidthUnselected != FLT_MAX)
        s.TabCloseButtonMinWidthUnselected = r(base.TabCloseButtonMinWidthUnselected);
    s.TabBarOverlineSize = r(base.TabBarOverlineSize);
    s.TreeLinesRounding = r(base.TreeLinesRounding);
    s.DragDropTargetRounding = r(base.DragDropTargetRounding);
    s.DragDropTargetBorderSize = r(base.DragDropTargetBorderSize);
    s.DragDropTargetPadding = r(base.DragDropTargetPadding);
    s.ColorMarkerSize = r(base.ColorMarkerSize);
    s.SeparatorTextPadding = r2(base.SeparatorTextPadding);
    s.DockingSeparatorSize = r(base.DockingSeparatorSize);
    s.DisplayWindowPadding = r2(base.DisplayWindowPadding);
    s.DisplaySafeAreaPadding = r2(base.DisplaySafeAreaPadding);
    s.MouseCursorScale = r(base.MouseCursorScale);
    // 枠線は 1 未満にならないよう（0 は 0 のまま）丸める。1.0 → 1.25 倍では 1px のまま（太らせない）。
    auto border = [&](float v) { return v > 0.0f ? (std::max)(1.0f, r(v)) : 0.0f; };
    s.WindowBorderSize = border(base.WindowBorderSize);   s.ChildBorderSize = border(base.ChildBorderSize);
    s.PopupBorderSize = border(base.PopupBorderSize);     s.FrameBorderSize = border(base.FrameBorderSize);
    s.TabBorderSize = border(base.TabBorderSize);         s.TabBarBorderSize = border(base.TabBarBorderSize);
    s.SeparatorTextBorderSize = border(base.SeparatorTextBorderSize);
    s.TreeLinesSize = border(base.TreeLinesSize);
    // 行高: フォント高（FontSizeBase × scale を ImGui が整数へ丸める）と kRowH の差分を上下均等に割る。
    {
        const float font = std::floor(base.FontSizeBase * scale + 0.5f);
        const float row  = r(size::kRowH);
        if (base.FramePadding.y * 2.0f + base.FontSizeBase == size::kRowH)     // 基準が kRowH 行のときだけ
            s.FramePadding.y = (std::max)(1.0f, (row - font) * 0.5f);
    }
    s.FontScaleDpi = base.FontScaleDpi * scale;
    s._MainScale = base._MainScale * scale;
    return s;
}

// ---- フォントのハンドル（ImGuiManager が起動時に埋める。null なら既定フォント）----
struct Fonts
{
    ImFont* body = nullptr;   // Segoe UI + Yu Gothic UI + Segoe UI Symbol + Lucide（既定）
    ImFont* bold = nullptr;   // 見出し / タブ / 選択名（Segoe UI Bold + Yu Gothic UI Semibold + Lucide）
    ImFont* mono = nullptr;   // 数値（Cascadia Mono / Consolas + Lucide）
    bool    icons = false;    // Lucide を読み込めたか（false のとき ICON_* は化ける）
};
inline Fonts g_fonts;

// ---- WCAG コントラスト比（レポート / 自己検査用。sRGB → 相対輝度）----
inline float RelativeLuminance(const ImVec4& c)
{
    auto lin = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * lin(c.x) + 0.7152f * lin(c.y) + 0.0722f * lin(c.z);
}
inline float ContrastRatio(const ImVec4& a, const ImVec4& b)
{
    const float la = RelativeLuminance(a), lb = RelativeLuminance(b);
    const float hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05f) / (lo + 0.05f);
}

// 色に不透明度を掛ける
inline ImVec4 WithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }
inline ImVec4 Mul(const ImVec4& c, float k) { return ImVec4(c.x * k, c.y * k, c.z * k, c.w); }

// ---- ImGuiStyle への流し込み（エディタ用。配布 GameRuntime では呼ばない）----
inline void ApplyStyle(ImGuiStyle& style)
{
    // --- 形状: UE5 は角ばり気味。ウィンドウは 0、入力欄は 2、ポップアップだけ 4 ---
    style.WindowRounding          = 0.0f;
    style.ChildRounding           = 2.0f;
    style.FrameRounding           = 2.0f;
    style.GrabRounding            = 2.0f;
    style.PopupRounding           = 4.0f;
    style.TabRounding             = 3.0f;
    style.ScrollbarRounding       = 6.0f;
    // --- 余白: 行高 23（16 + 3.5*2）。Details は密（行間 4）---
    style.WindowPadding           = ImVec2(8.0f, 8.0f);
    style.FramePadding            = ImVec2(7.0f, 3.5f);
    style.CellPadding             = ImVec2(6.0f, 3.0f);
    style.ItemSpacing             = ImVec2(6.0f, 4.0f);
    style.ItemInnerSpacing        = ImVec2(4.0f, 4.0f);
    style.TouchExtraPadding       = ImVec2(0.0f, 0.0f);
    style.IndentSpacing           = 16.0f;
    style.ScrollbarSize           = 10.0f;
    style.GrabMinSize             = 8.0f;
    // --- 枠 ---
    style.WindowBorderSize        = 1.0f;
    style.ChildBorderSize         = 1.0f;
    style.PopupBorderSize         = 1.0f;
    style.FrameBorderSize         = 1.0f;
    style.TabBorderSize           = 0.0f;
    style.TabBarBorderSize        = 1.0f;    // タブバーの下線
    style.TabBarOverlineSize      = 2.0f;    // 選択タブ上端のアクセントライン
    style.DockingSeparatorSize    = 3.0f;    // パネル間の隙間（Bg0 が見える）
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextPadding    = ImVec2(12.0f, 4.0f);
    // --- 配置 ---
    style.WindowTitleAlign        = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;   // ドック左上の大きな ▽ を出さない
    style.ButtonTextAlign         = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign     = ImVec2(0.0f, 0.0f);
    style.DisplayWindowPadding    = ImVec2(19.0f, 19.0f);
    style.DisplaySafeAreaPadding  = ImVec2(0.0f, 0.0f);
    style.HoverDelayShort         = 0.15f;
    style.HoverDelayNormal        = 0.40f;
    style.HoverStationaryDelay    = 0.10f;
    style.TreeLinesSize           = 1.0f;

    ImVec4* c = style.Colors;
    // テキスト
    c[ImGuiCol_Text]                 = Text;
    c[ImGuiCol_TextDisabled]         = TextFaint;
    c[ImGuiCol_TextSelectedBg]       = Hex(0x2F8CFF, 0.35f);
    c[ImGuiCol_TextLink]             = AccentHover;
    // ベース背景（ポップアップは Bg2 を不透明で: ビューポート上でも同化しない）
    c[ImGuiCol_WindowBg]             = Bg1;
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = Bg2;
    c[ImGuiCol_Border]               = InputBorder;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    // タイトル（フローティング窓）/ ドックのタブ井戸 / メニューバー
    c[ImGuiCol_TitleBg]              = Bg0;
    c[ImGuiCol_TitleBgActive]        = Bg0;
    c[ImGuiCol_TitleBgCollapsed]     = Bg0;
    c[ImGuiCol_MenuBarBg]            = Bg0;
    // フレーム（入力欄・スライダー溝: パネルより暗い凹み）
    c[ImGuiCol_FrameBg]              = InputBg;
    c[ImGuiCol_FrameBgHovered]       = InputBgHover;
    c[ImGuiCol_FrameBgActive]        = InputBgActive;
    // ボタン（フラット。通常 Bg3、ホバーで一段明るく）
    c[ImGuiCol_Button]               = Bg3;
    c[ImGuiCol_ButtonHovered]        = Bg4;
    c[ImGuiCol_ButtonActive]         = Hex(0x3E3E46);
    // ヘッダ（選択行 = アクセント 30%。ホバー = Bg3）
    c[ImGuiCol_Header]               = Selection;
    c[ImGuiCol_HeaderHovered]        = Bg3;
    c[ImGuiCol_HeaderActive]         = SelectionActive;
    // タブ（非選択 = Bg2 / 選択 = パネル色で立ち上がり上端アクセント）
    c[ImGuiCol_Tab]                       = Bg2;
    c[ImGuiCol_TabHovered]                = Bg3;
    c[ImGuiCol_TabSelected]               = Bg1;
    c[ImGuiCol_TabSelectedOverline]       = Accent;
    c[ImGuiCol_TabDimmed]                 = Bg2;
    c[ImGuiCol_TabDimmedSelected]         = Bg1;
    c[ImGuiCol_TabDimmedSelectedOverline] = Hex(0x2F8CFF, 0.40f);
    // スクロール（トラック透明、細い丸グラブ）
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = Hex(0x36363D);
    c[ImGuiCol_ScrollbarGrabHovered] = Hex(0x4A4A53);
    c[ImGuiCol_ScrollbarGrabActive]  = Hex(0x5C5C67);
    // スライダー / チェック（ui:: の自前描画が主役。素の ImGui 描画に落ちたときの保険）
    c[ImGuiCol_SliderGrab]           = AccentHover;
    c[ImGuiCol_SliderGrabActive]     = Text;
    c[ImGuiCol_CheckMark]            = Hex(0xFFFFFF);
    // セパレータ（ドッキング分割バーもこの色。ホバー/ドラッグでアクセント）
    c[ImGuiCol_Separator]            = Border;
    c[ImGuiCol_SeparatorHovered]     = Hex(0x2F8CFF, 0.60f);
    c[ImGuiCol_SeparatorActive]      = Accent;
    // リサイズグリップ（通常は不可視）
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = Hex(0x2F8CFF, 0.40f);
    c[ImGuiCol_ResizeGripActive]     = Accent;
    // ドッキング
    c[ImGuiCol_DockingPreview]       = Hex(0x2F8CFF, 0.30f);
    c[ImGuiCol_DockingEmptyBg]       = Bg0;
    // テーブル
    c[ImGuiCol_TableHeaderBg]        = Bg2;
    c[ImGuiCol_TableBorderStrong]    = Border;
    c[ImGuiCol_TableBorderLight]     = Hex(0x222226);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = Hex(0xFFFFFF, 0.02f);
    // ナビ / ドラッグ&ドロップ / モーダル
    c[ImGuiCol_NavCursor]            = Accent;
    c[ImGuiCol_DragDropTarget]       = Accent;
    c[ImGuiCol_NavWindowingHighlight] = Hex(0xFFFFFF, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = Hex(0x000000, 0.45f);
    c[ImGuiCol_ModalWindowDimBg]     = Hex(0x000000, 0.55f);
    c[ImGuiCol_TreeLines]            = Hex(0xFFFFFF, 0.14f);
    c[ImGuiCol_PlotLines]            = TextDim;
    c[ImGuiCol_PlotLinesHovered]     = AccentHover;
    c[ImGuiCol_PlotHistogram]        = Accent;
    c[ImGuiCol_PlotHistogramHovered] = AccentHover;
}

} // namespace dx12e::theme
