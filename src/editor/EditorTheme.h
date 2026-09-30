#pragma once

// ===== エディタ UI テーマ（Uno 独自「ネオン・エッジ」・ダーク。第 1 波は Unreal Editor 5 風）=====
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

#include "editor/ThemeVariants.h"   // Hex() / Variant / 各案のトークン表（Default = 現行値）

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace dx12e::theme
{

// ---- テーマ・バリアント（アイデンティティ候補。既定 = 現行）----
// 下のトークン（Bg0 等）は【可変】。SetVariant() が案の表（ThemeVariants.h）から書き換える。
// 起動引数 --theme-variant a|b|c（main.cpp）→ ImGuiManager::Initialize の前に SetVariant → ApplyStyle が ImGuiStyle へ流す。
// 実行中の切替は ImGuiManager::SetThemeVariant()（基準スタイルの作り直しまで行う）。
inline Variant g_variant = Variant::Default;
inline Variant CurrentVariant() { return g_variant; }
inline const Spec& CurrentSpec() { return SpecOf(g_variant); }
inline const Deco& CurrentDeco() { return SpecOf(g_variant).deco; }
inline bool IsDefaultVariant() { return g_variant == Variant::Default; }
// 実行中の切替要求（コマンドパレット等から）。ImGuiManager::BeginFrame が NewFrame の前に適用する（-1 = 要求なし）。
inline bool g_variantSwitchEnabled = false;   // --theme-variant を付けて起動した時だけ true（パレットに切替コマンドを出す）
inline int  g_pendingVariant = -1;
inline void RequestVariant(Variant v) { g_pendingVariant = static_cast<int>(v); }
// 窓外装飾（ui::deco::PaintChrome）の呼び出し口。Gui ライブラリは Editor に依存できないので、EditorLayer が設定する
// （g_restoreMainScaleFn と同じ流儀）。ImGuiManager::EndFrame が全パネルの描画後に 1 回呼ぶ。
inline void (*g_paintChromeFn)() = nullptr;

// ---- 面（深 → 浅）----
// ★初期値は Default の表（= 現行値）。SetVariant で案の値に置き換わる。
inline ImVec4 Bg0         = SpecOf(Variant::Default).pal.bg0;   // 最深部: ビューポート周辺 / タブ井戸 / ステータスバー / ドックの隙間
inline ImVec4 Bg1         = SpecOf(Variant::Default).pal.bg1;   // パネル本体
inline ImVec4 Bg2         = SpecOf(Variant::Default).pal.bg2;   // ヘッダ帯 / 非選択タブ / ポップアップ / 行ゼブラ
inline ImVec4 Bg3         = SpecOf(Variant::Default).pal.bg3;   // ホバー面 / ボタン地
inline ImVec4 Bg4         = SpecOf(Variant::Default).pal.bg4;   // ボタンのホバー / 押下（Bg3 の一段上）
inline ImVec4 InputBg     = SpecOf(Variant::Default).pal.inputBg;   // 入力欄（パネルより暗い凹み）
inline ImVec4 InputBgHover  = SpecOf(Variant::Default).pal.inputBgHover;
inline ImVec4 InputBgActive = SpecOf(Variant::Default).pal.inputBgActive;
inline ImVec4 InputBorder      = SpecOf(Variant::Default).pal.inputBorder;       // 入力欄の 1px 枠
inline ImVec4 InputBorderHover = SpecOf(Variant::Default).pal.inputBorderHover;
inline ImVec4 Border      = SpecOf(Variant::Default).pal.border;         // 面の境界（控えめ）
inline ImVec4 BorderStrong = SpecOf(Variant::Default).pal.borderStrong;  // ポップアップ / フローティング窓の枠

// ---- アクセント（既定 = UE5 の選択青。案ごとに色相が違う）----
inline ImVec4 Accent        = SpecOf(Variant::Default).pal.accent;
inline ImVec4 AccentHover   = SpecOf(Variant::Default).pal.accentHover;
inline ImVec4 AccentPressed = SpecOf(Variant::Default).pal.accentPressed;
inline ImVec4 OnAccent      = SpecOf(Variant::Default).pal.onAccent;   // アクセント塗りの上の文字 / ✓
inline ImVec4 Selection      = SpecOf(Variant::Default).pal.selection;        // 選択行の面
inline ImVec4 SelectionHover = SpecOf(Variant::Default).pal.selectionHover;
inline ImVec4 SelectionActive = SpecOf(Variant::Default).pal.selectionActive;

// ---- テキスト（3 段。TextHi/TextMid は旧名の互換）----
inline ImVec4 Text        = SpecOf(Variant::Default).pal.text;
inline ImVec4 TextDim     = SpecOf(Variant::Default).pal.textDim;
inline ImVec4 TextFaint   = SpecOf(Variant::Default).pal.textFaint;   // 実測: Bg0-Bg3 / 入力欄の上で 4.5:1 以上（旧 #74767f は 3.96）
inline ImVec4 TextHi      = Text;
inline ImVec4 TextMid     = SpecOf(Variant::Default).pal.textMid;     // ラベル（Text と TextDim の間）

// ---- ステータス色 ----
inline ImVec4 Good        = SpecOf(Variant::Default).pal.good;
inline ImVec4 Warn        = SpecOf(Variant::Default).pal.warn;
inline ImVec4 Bad         = SpecOf(Variant::Default).pal.bad;         // 文字色として Bg1-Bg3 で 4.5:1 以上になるよう少し明るく（初期値 #E5534B は Bg2 で 4.44）

// ---- 種別カラー（アイコングリフの tint。「控えめな色味」= 彩度を落とし、全部同じ青にしない）----
inline ImVec4 TypeMesh    = SpecOf(Variant::Default).pal.typeMesh;      // 中性（メッシュは数が多いので色を主張しない）
inline ImVec4 TypeLight   = SpecOf(Variant::Default).pal.typeLight;     // 琥珀
inline ImVec4 TypeCamera  = SpecOf(Variant::Default).pal.typeCamera;    // 青
inline ImVec4 TypeAudio   = SpecOf(Variant::Default).pal.typeAudio;     // ティール
inline ImVec4 TypeScript  = SpecOf(Variant::Default).pal.typeScript;    // 紫
inline ImVec4 TypePhysics = SpecOf(Variant::Default).pal.typePhysics;   // 緑
inline ImVec4 TypeUi      = SpecOf(Variant::Default).pal.typeUi;        // マゼンタ
inline ImVec4 TypePrefab  = SpecOf(Variant::Default).pal.typePrefab;
inline ImVec4 TypeEmpty   = SpecOf(Variant::Default).pal.typeEmpty;
inline ImVec4 TypeFolder  = SpecOf(Variant::Default).pal.typeFolder;    // フォルダ（中性の明るいグレー）
inline ImVec4 TypeScene   = SpecOf(Variant::Default).pal.typeScene;     // シーン = 橙

// ---- 旧名の互換（散在する参照を壊さない。値は新トークンへ寄せてある。SetVariant で同じく更新する）----
inline ImVec4 AppBg       = Bg0;
inline ImVec4 PanelBg     = Bg1;
inline ImVec4 Chrome      = Bg1;      // ツールバー / メニュー帯（旧: パネルより少し明るい別色）
inline ImVec4 GroupBg     = Bg2;
inline ImVec4 FrameBg     = InputBg;
inline ImVec4 FrameBgHi   = InputBgHover;
inline ImVec4 FrameBgActive = InputBgActive;
inline ImVec4 BorderSoft  = InputBorder;
inline ImVec4 AccentLight = AccentHover;
inline ImVec4 AccentDim   = SelectionHover;
inline ImVec4 AccentDim2  = Selection;

// 案を切り替える（トークンを書き換える）。ImGuiStyle への反映は ApplyStyle（と ImGuiManager::SetThemeVariant）が行う。
inline void SetVariant(Variant v)
{
    g_variant = v;
    const Palette& p = SpecOf(v).pal;
    Bg0 = p.bg0; Bg1 = p.bg1; Bg2 = p.bg2; Bg3 = p.bg3; Bg4 = p.bg4;
    InputBg = p.inputBg; InputBgHover = p.inputBgHover; InputBgActive = p.inputBgActive;
    InputBorder = p.inputBorder; InputBorderHover = p.inputBorderHover;
    Border = p.border; BorderStrong = p.borderStrong;
    Accent = p.accent; AccentHover = p.accentHover; AccentPressed = p.accentPressed; OnAccent = p.onAccent;
    Selection = p.selection; SelectionHover = p.selectionHover; SelectionActive = p.selectionActive;
    Text = p.text; TextDim = p.textDim; TextFaint = p.textFaint; TextHi = p.text; TextMid = p.textMid;
    Good = p.good; Warn = p.warn; Bad = p.bad;
    TypeMesh = p.typeMesh; TypeLight = p.typeLight; TypeCamera = p.typeCamera; TypeAudio = p.typeAudio;
    TypeScript = p.typeScript; TypePhysics = p.typePhysics; TypeUi = p.typeUi; TypePrefab = p.typePrefab;
    TypeEmpty = p.typeEmpty; TypeFolder = p.typeFolder; TypeScene = p.typeScene;
    AppBg = p.bg0; PanelBg = p.bg1; Chrome = p.bg1; GroupBg = p.bg2;
    FrameBg = p.inputBg; FrameBgHi = p.inputBgHover; FrameBgActive = p.inputBgActive;
    BorderSoft = p.inputBorder; AccentLight = p.accentHover; AccentDim = p.selectionHover; AccentDim2 = p.selection;
}

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
// 現在のバリアント（g_variant）の Metrics / Palette を流す。Default は第 1 波の値そのもの（UI テストと単体テストが監視）。
inline void ApplyStyle(ImGuiStyle& style)
{
    const Spec& sp = CurrentSpec();
    const Palette& P = sp.pal;
    const Metrics& M = sp.m;
    // --- 形状: UE5 は角ばり気味。ウィンドウは 0、入力欄は 2、ポップアップだけ 4（案ごとに Metrics で変わる）---
    style.WindowRounding          = M.windowRounding;
    style.ChildRounding           = M.childRounding;
    style.FrameRounding           = M.frameRounding;
    style.GrabRounding            = M.grabRounding;
    style.PopupRounding           = M.popupRounding;
    style.TabRounding             = M.tabRounding;
    style.ScrollbarRounding       = M.scrollbarRounding;
    // --- 余白: 行高 23（16 + 3.5*2）。Details は密（行間 4）。★案では動かさない（レイアウト・UI テストが共通）---
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
    style.WindowBorderSize        = M.windowBorder;
    style.ChildBorderSize         = M.childBorder;
    style.PopupBorderSize         = M.popupBorder;
    style.FrameBorderSize         = M.frameBorder;
    style.TabBorderSize           = 0.0f;
    style.TabBarBorderSize        = M.tabBarBorder;    // タブバーの下線
    style.TabBarOverlineSize      = M.tabOverline;     // 選択タブ上端のアクセントライン
    style.DockingSeparatorSize    = M.dockSeparator;   // パネル間の隙間（Bg0 が見える）
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
    c[ImGuiCol_Text]                 = P.text;
    c[ImGuiCol_TextDisabled]         = P.textFaint;
    c[ImGuiCol_TextSelectedBg]       = P.textSelectedBg;
    c[ImGuiCol_TextLink]             = P.accentHover;
    // ベース背景（ポップアップは Bg2 を不透明で: ビューポート上でも同化しない。B だけ半透明）
    c[ImGuiCol_WindowBg]             = P.windowBg;
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = P.popupBg;
    c[ImGuiCol_Border]               = P.inputBorder;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    // タイトル（フローティング窓）/ ドックのタブ井戸 / メニューバー
    c[ImGuiCol_TitleBg]              = P.bg0;
    c[ImGuiCol_TitleBgActive]        = P.bg0;
    c[ImGuiCol_TitleBgCollapsed]     = P.bg0;
    c[ImGuiCol_MenuBarBg]            = P.bg0;
    // フレーム（入力欄・スライダー溝: パネルより暗い凹み）
    c[ImGuiCol_FrameBg]              = P.inputBg;
    c[ImGuiCol_FrameBgHovered]       = P.inputBgHover;
    c[ImGuiCol_FrameBgActive]        = P.inputBgActive;
    // ボタン（フラット。通常 Bg3、ホバーで一段明るく）
    c[ImGuiCol_Button]               = P.bg3;
    c[ImGuiCol_ButtonHovered]        = P.bg4;
    c[ImGuiCol_ButtonActive]         = P.btnActive;
    // ヘッダ（選択行 = アクセント 30%。ホバー = Bg3）
    c[ImGuiCol_Header]               = P.selection;
    c[ImGuiCol_HeaderHovered]        = P.bg3;
    c[ImGuiCol_HeaderActive]         = P.selectionActive;
    // タブ（非選択 = Bg2 / 選択 = パネル色で立ち上がり上端アクセント）
    c[ImGuiCol_Tab]                       = P.bg2;
    c[ImGuiCol_TabHovered]                = P.bg3;
    c[ImGuiCol_TabSelected]               = P.bg1;
    c[ImGuiCol_TabSelectedOverline]       = P.tabOverline;
    c[ImGuiCol_TabDimmed]                 = P.bg2;
    c[ImGuiCol_TabDimmedSelected]         = P.bg1;
    c[ImGuiCol_TabDimmedSelectedOverline] = P.tabOverlineDim;
    // スクロール（トラック透明、細い丸グラブ）
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = P.scrollGrab;
    c[ImGuiCol_ScrollbarGrabHovered] = P.scrollGrabHover;
    c[ImGuiCol_ScrollbarGrabActive]  = P.scrollGrabActive;
    // スライダー / チェック（ui:: の自前描画が主役。素の ImGui 描画に落ちたときの保険）
    c[ImGuiCol_SliderGrab]           = P.accentHover;
    c[ImGuiCol_SliderGrabActive]     = P.text;
    c[ImGuiCol_CheckMark]            = P.onAccent;
    // セパレータ（ドッキング分割バーもこの色。ホバー/ドラッグでアクセント）
    c[ImGuiCol_Separator]            = P.separator;
    c[ImGuiCol_SeparatorHovered]     = P.separatorHover;
    c[ImGuiCol_SeparatorActive]      = P.accent;
    // リサイズグリップ（通常は不可視）
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = P.resizeGripHover;
    c[ImGuiCol_ResizeGripActive]     = P.accent;
    // ドッキング
    c[ImGuiCol_DockingPreview]       = P.dockingPreview;
    c[ImGuiCol_DockingEmptyBg]       = P.bg0;
    // テーブル
    c[ImGuiCol_TableHeaderBg]        = P.bg2;
    c[ImGuiCol_TableBorderStrong]    = P.border;
    c[ImGuiCol_TableBorderLight]     = P.tableBorderLight;
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = P.tableRowAlt;
    // ナビ / ドラッグ&ドロップ / モーダル
    c[ImGuiCol_NavCursor]            = P.accent;
    c[ImGuiCol_DragDropTarget]       = P.accent;
    c[ImGuiCol_NavWindowingHighlight] = Hex(0xFFFFFF, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = Hex(0x000000, 0.45f);
    c[ImGuiCol_ModalWindowDimBg]     = Hex(0x000000, 0.55f);
    c[ImGuiCol_TreeLines]            = P.treeLines;
    c[ImGuiCol_PlotLines]            = P.textDim;
    c[ImGuiCol_PlotLinesHovered]     = P.accentHover;
    c[ImGuiCol_PlotHistogram]        = P.accent;
    c[ImGuiCol_PlotHistogramHovered] = P.accentHover;
}

} // namespace dx12e::theme
