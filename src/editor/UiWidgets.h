#pragma once

// ===== エディタ共通の見た目部品（ui:: 名前空間）=====
// PropertyGrid.h の pg:: はここの部品を内部で呼ぶ（pg:: の API は互換のまま、見た目だけ差し替わる）。
// pg:: に乗らない生 ImGui フォーム（ポスト系の窓など）を後続波で移行するときの受け皿でもある:
//     ImGui::SliderFloat(...)  →  ui::SliderFloat(...)
//     ImGui::Checkbox(...)     →  ui::Checkbox(...)
//     ImGui::Combo(...)        →  ui::Combo(...)
//
// ★ID の流儀は ImGui と同じ（第 1 引数の "##v" 等がそのまま ID になる）。pg:: は PushID(label) + "##v" で
//   UI 自動テストの参照名になっているので、ここの部品はその ID を変えない。
// ★色・寸法は EditorTheme.h のトークンだけを引く。ここに 16 進数を書かない。
// 実装は UiWidgets.cpp（imgui_internal.h を使うのでヘッダに晒さない）。

#include "editor/EditorTheme.h"
#include "editor/EditorIcons.h"

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

#include <cstddef>

namespace dx12e::ui
{

// ---- DPI スケール（実体は EditorTheme.h の theme::Px 系。ui:: からも引けるようにしてある）----
// ImGui へ渡す寸法・描画座標のオフセットは「論理 px（100% 表示の px）」で書き、Px() を通す。
using theme::Px;
using theme::PxF;
using theme::Scale;
using theme::ToLogical;

// ---- フォント切替（ペアで使う）。サイズは今のまま、字体だけ変える ----
void PushBold();   // 見出し / 選択名（Semibold 相当）
void PopBold();
void PushMono();   // 数値（等幅）。ExtraSizeScale で本文と字の大きさを揃えてある
void PopMono();

// ---- アイコン ----
// 現在のカーソルにアイコン 1 個を色付きで置く（SameLine でラベルと並べられる。幅 ≒ 1em）。
void Icon(const char* glyph, const ImVec4& color);
// 指定の中心へアイコンを描く（DrawList 直。グリフの実際の外形で中心合わせするので縦横ずれない）。
// px<=0 なら標準サイズ。
void DrawIconCentered(ImDrawList* dl, const char* glyph, ImVec2 center, ImU32 col, float px = 0.0f);
// フラットなアイコンのみのボタン。押されたら true。tooltip はホバーで出る。
// active=true のときは薄いアクセント面 + 下辺のアクセントライン。tint を渡すとアイコン色を固定する。
bool IconButton(const char* id, const char* glyph, const char* tooltip = nullptr,
                bool active = false, const ImVec4* tint = nullptr,
                float sizePx = 0.0f, float iconPx = 0.0f,
                const ImVec4* activeFace = nullptr);   // activeFace: active の面/下線の色（既定はアクセント）
// アイコン + 右に小さい chevron の「▾」付きボタン（クリックでメニューを開く用途）。
bool IconDropdownButton(const char* id, const char* glyph, const char* tooltip = nullptr,
                        bool active = false, float sizePx = 0.0f);

// ---- ボタン ----
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));   // 主要アクション（アクセント塗り）
bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0));    // 危険（赤枠・赤文字）

// ---- 入力（見た目は共通: 凹み入力欄 + 1px 枠。ホバーで明るく、フォーカス/ドラッグ中はアクセント枠）----
bool DragFloat(const char* id, float* v, float speed = 1.0f, float mn = 0.0f, float mx = 0.0f,
               const char* fmt = "%.3f", ImGuiSliderFlags flags = 0);
bool DragInt(const char* id, int* v, float speed = 1.0f, int mn = 0, int mx = 0, const char* fmt = "%d",
             ImGuiSliderFlags flags = 0);
// スライダー: 溝 + アクセント 40% の塗りバー + 等幅の値（中央）。つまみは細い縦線（文字に重ならない）。
bool SliderFloat(const char* id, float* v, float mn, float mx, const char* fmt = "%.3f",
                 ImGuiSliderFlags flags = 0);
bool SliderInt(const char* id, int* v, int mn, int mx, const char* fmt = "%d",
               ImGuiSliderFlags flags = 0);
// チェックボックス: 16px・角丸 2 の箱。ON はアクセント塗りに白い ✓。label の "##" 以降は非表示（ID 専用）。
bool Checkbox(const char* label, bool* v);
// コンボ: 幅いっぱい・右端に小さい chevron。
// BeginCombo/EndCombo の置き換え（右端に小さい chevron。preview が表示文字）。true のとき項目を並べて EndCombo。
bool BeginCombo(const char* id, const char* preview, ImGuiComboFlags flags = 0);
inline void EndCombo() { ImGui::EndCombo(); }
bool Combo(const char* id, int* idx, const char* const items[], int count, int popupMaxItems = -1);
// ImGui::Combo のゼロ区切り文字列版（項目を NUL で区切って末尾を NUL 2 つで終える形式）。
bool Combo(const char* id, int* idx, const char* itemsSeparatedByZeros, int popupMaxItems = -1);
bool InputText(const char* id, char* buf, size_t size, ImGuiInputTextFlags flags = 0);
bool InputTextWithHint(const char* id, const char* hint, char* buf, size_t size,
                       ImGuiInputTextFlags flags = 0);
// カラー欄: スウォッチ（NoInputs 指定時は枠つき）。
bool ColorEdit3(const char* id, float* col, ImGuiColorEditFlags flags = 0);
bool ColorEdit4(const char* id, float* col, ImGuiColorEditFlags flags = 0);
// 直前に描いた項目を「入力欄の枠」でなぞる（ホバー/フォーカスの色替え）。自前の入力部品で使う。
void OutlineLastItem();

// 虫眼鏡アイコン付き・クリア(✕)付きの検索欄。入力が変わったら true。
bool SearchField(const char* id, char* buf, size_t size, const char* hint);

// ---- 見出し・行 ----
// コンポーネント見出し / セクション帯（高さ 26・Bg2・開閉 chevron・任意のアイコン）。
// 返り値は開いているか。label の "##" 以降は非表示（ID 専用）。
bool SectionHeader(const char* label, const char* iconGlyph = nullptr,
                   const ImVec4* iconTint = nullptr, ImGuiTreeNodeFlags flags = 0);
// 素の ImGui::CollapsingHeader の置き換え（アイコン無しの SectionHeader）。
bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0);
// SectionHeader の直後に呼ぶ: 右端に「⋯」を重ねる。押されたら true（コンテキストメニューを開く合図）。
bool HeaderMenuButton();

// ---- メニュー ----
// 左にアイコン列のあるメニュー項目（ImGui 内部の MenuItemEx の icon 引数を使う）。ラベル文字列は素のままなので
// 項目の ID（= UI 自動テストの参照名）は変わらない。ひとつのメニューの中では全項目にアイコンを付けること
// （無い項目は ICON_BLANK）。付けないと列がずれる。
bool MenuItem(const char* icon, const char* label, const char* shortcut = nullptr,
              bool selected = false, bool enabled = true);
bool MenuItem(const char* icon, const char* label, const char* shortcut, bool* selected, bool enabled = true);

// メニュー / ポップアップの中身を UE 風に広く（行高 ≒ 26）する。BeginMenu / BeginPopup の
// 直前で Push、EndMenu / EndPopup の後で Pop（ペア）。
void PushMenuStyle();
void PopMenuStyle();


// ===========================================================================
// deco:: — テーマ・バリアント（アイデンティティ候補 A/B/C。EditorTheme.h / ThemeVariants.h）の共通装飾ヘルパ
// ---------------------------------------------------------------------------
// ★グロー / 光のヘアライン / シグナルバー / 影 / 状態遷移のイージングは、パネルごとに書かずここに集約する。
//   Variant::Default（既定）では、どの関数も従来と同じ描画・同じ値を返す（＝見た目は 1px も変わらない）。
//   案を 1 つに決めたら、その案の分岐だけ残して他を消せる構造（呼び出し側は Variant を見ない）。
// ★色は EditorTheme.h のトークン（ThemeVariants.h の Deco 方針）だけを引く。寸法は Px()。
// ===========================================================================
namespace deco
{
// 既定（現行）以外の案が有効か。
bool Active();

// 状態の滑らかな遷移: (id, slot) ごとの 0..1 の値が target へ寄っていく（時間 = Deco::easeSec）。
// Default（easeSec == 0）は target をそのまま返す（＝即時・記憶なし）。slot: 0=hover 1=active 2=focus。
float Ease(ImGuiID id, int slot, float target);

// 外側へ淡く広がるハロー（多重の細い枠で疑似ブラー）。strength 0..1（案の glow 係数が掛かる）。
// 窓のクリップを広げて描くので、テーブルのセルの縁でも切れない。
void Glow(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding, const ImVec4& col, float strength);

// 行の面（ヒエラルキー / コマンドパレット / ツリー行）。窓幅いっぱいの矩形 [mn, mx] を渡す。
//   Default = 選択: アクセント 30% + 左 2px アクセントライン / ホバー: Bg3 / 押下: アクセント 42%
//   A = ネオンバー（左ライン + 光のにじみ）/ B = ガラスの丸い面 + 光の縁 / C = 左のシグナルバー + 無彩色の面
void RowFace(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool selected, bool hovered, bool held);

// 選択中カード（アセットブラウザ）の縁取り。Default = アクセント 2px。
void CardSelected(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding);

// 左端の選択バー（ツリー行の選択）。Default = 2px のアクセント。
void SelectionBar(ImDrawList* dl, ImVec2 mn, ImVec2 mx);

// 浮遊するカード（トースト等）。影 + 面 + 縁。Default は従来のトーストと同じ描画。
// face = 面の色（alpha 込み）、alpha = 全体のフェード。
void FloatingCard(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding, const ImVec4& face, float alpha);

// 全パネルの描画後（ImGuiManager::EndFrame）に 1 回呼ぶ。パネル上端のヘアライン / フォーカス中パネルのエッジ /
// 選択タブの光 / ポップアップ・メニュー・パレットの影 / ステータスバーの線など「窓の外側から足す」装飾。Default は何もしない。
void PaintChrome();
} // namespace deco

} // namespace dx12e::ui
