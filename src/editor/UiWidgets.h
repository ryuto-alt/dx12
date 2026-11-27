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
#include <cstdint>

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

// アイコン + ラベル + 右に chevron の「▾」付きボタン（ツールバーのドロップダウン。幅はラベルに合わせて自動）。
// label が空ならアイコンだけの IconDropdownButton と同じ見た目。押されたら true（ポップアップを開く用）。
// active = 面 + 下辺のネオンライン。value を渡すと等幅の値（"0.25 m" 等）をラベルの右に淡く添える。
bool LabelDropdownButton(const char* id, const char* glyph, const char* label, const char* tooltip = nullptr,
                         bool active = false, float sizePx = 0.0f, const char* value = nullptr);
// 選択 / 非選択の小さなチップ（スナップ量のプリセット等）。押されたら true。selected = アクセントの面 + 縁。
// widthPx <= 0 でラベル幅に合わせる。
bool Chip(const char* id, const char* label, bool selected, float widthPx = 0.0f);

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
//   既定（Variant::Default）= ネオン・エッジ。B / C は別テーマ（将来のテーマ切替の下地）で、差は Deco 表と
//   各関数の Variant 分岐だけ。呼び出し側（各パネル）は Variant を見ない。
//   後続（ノードグラフ・タイムライン）が使う部品: Ease / Glow / RowFace / CardSelected / SelectionBar /
//   FloatingCard / PaintChrome / EdgeLine（光の線）/ Halo（点の発光）。
// ★色は EditorTheme.h のトークン（ThemeVariants.h の Deco 方針）だけを引く。寸法は Px()。
// ===========================================================================
namespace deco
{
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
// Graphite の黄の指針（選択行の左バー・選択中ツールの下線）。端を整数 px へ丸める（スライド中も 2px 幅の線が滲まない = GG モーション則 01）。
// 名前を Needle で始めるのは A3 の UiTokenLint が黄の線として数える入口を、ここへ一本化するため（本体は数えない）。
void NeedleBar(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col);
// 主尺（タイトルバー下端の目盛り。細 8px・高さ 3 / 主 40px・高さ 6。W1-A7）。bar = タイトルバーの矩形（物理 px）。下辺に沿って描く。
// Graphite のときだけ呼ぶこと（色は gg::GgChrome の tickMinor / tickMajor）。
void ScaleRuler(ImDrawList* dl, ImVec2 mn, ImVec2 mx);
// フォーカス・キャリパー（黄の予算 01。顎 2x8 + 2px の線）。[x0, x1] = フォーカス中パネルの全幅、yBottom = タイトルバーの下端（物理 px）。
// 線は主尺の主目盛りの上に乗る。focused=false は NeedleDim。呼び出し箇所は EditorChrome.cpp の 1 か所だけ（tests/editor_chrome_test が固定）。
void Caliper(ImDrawList* dl, float x0, float x1, float yBottom, bool focused);

// 浮遊するカード（トースト等）。影 + 面 + 縁。Default は従来のトーストと同じ描画。
// face = 面の色（alpha 込み）、alpha = 全体のフェード。
void FloatingCard(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding, const ImVec4& face, float alpha);

// 光の線（中央が明るく両端へ消える。水平: a→b が左右 / 垂直: 上下）。alpha 0..1。ツールバーの下端・タイムラインのプレイヘッド等。
void EdgeLine(ImDrawList* dl, ImVec2 a, ImVec2 b, const ImVec4& col, float alpha);

// 点のハロー（プレイヘッド・キー・ノードのピンの発光）。center を中心に radius まで 2 乗で減衰する光。strength 0..1（案の glow 係数が掛かる）。
void Halo(ImDrawList* dl, ImVec2 center, float radius, const ImVec4& col, float strength);

// 全パネルの描画後（ImGuiManager::EndFrame）に 1 回呼ぶ。パネル上端のヘアライン / フォーカス中パネルのエッジ /
// 選択タブの光 / ポップアップ・メニュー・パレットの影 / ステータスバーの線など「窓の外側から足す」装飾。Default は何もしない。
void PaintChrome();
} // namespace deco

// ===========================================================================
// ゲージ型の進捗・生存ティック（W1-A5 / RM-A3。Graphite Gauge §4.18 / §4.19）
// ---------------------------------------------------------------------------
// ★進捗は連続に伸ばさず 12px（progressStep）刻みで、1 刻みを 60ms で送る（決定論 / background では即時）。
// ★指針は t1（パネル内）。黄になるのはステータスバーの主ジョブ 1 本（ProgressRole::StatusJob）だけで、
//   呼び出しは StatusBar（A7）の 1 か所に限る（tests/text_fit_test が StatusJob の使用箇所を固定している）。
// ★スピナーは時間では回さない（決定 D18）。出来事 1 件で 30° 刻む。出来事が無い間は止まっている。
// 全部の旧テーマでも同じ絵（トークンは Bg3 / TextDim / Text / Bad だけ）。既存の ImGui::ProgressBar は置き換えない。
// ===========================================================================
enum class ProgressState : uint8_t { Running, Paused, Done, Failed };
enum class ProgressRole  : uint8_t { Panel, StatusJob };

// 主尺と副尺のある進捗バー。溝 6px（StatusJob は細型 4px）+ 12px 刻みの塗り + 先端の指針。id は状態のキー（窓の中で一意）。
// size.x <= 0 で残り幅いっぱい。項目の高さは指針の高さ（12px。細型は 10px）。
void ProgressBar(const char* id, float t01, ImVec2 size = ImVec2(0, 0),
                 ProgressState state = ProgressState::Running, ProgressRole role = ProgressRole::Panel);
// 細い輪と数値（％）。label があれば右に添える。刻みは 15°（24 刻み）。
void ProgressGauge(const char* id, float t01, const char* label = nullptr, float pxLogical = 36.0f);

struct GaugeSpinnerInfo
{
    int    laps = 0;              // 12 刻み（= 1 周）を超えた回数。「×n」の n
    double sinceEventSec = 0;     // 最後に出来事の数が変わってからの秒
    double elapsedSec = 0;        // この id を使い始めてからの秒
    bool   stalled = false;       // 10 秒以上進んでいない（「応答待ち 10 s」）
};
// 生存ティックの輪だけを DrawList へ描く（center = 中心、物理 px）。eventCount = 出来事の累計。
// pxLogical は 14 / 20（輪 + 指針）、32 / 56（12 目盛り + 指針 + 軸）。それ以外は近い方へ丸める。
// elapsedSec / sinceEventSec: ジョブ側の経過秒・最後の出来事からの秒（>= 0 で採用）。負なら状態の寿命から出す
// （パネルを開いた時点から数え直すので、ジョブの途中で開く・タブを往復する呼び出しでは必ずジョブ側の値を渡すこと）。
GaugeSpinnerInfo DrawGaugeSpinner(ImDrawList* dl, ImVec2 center, ImGuiID id, uint32_t eventCount, float pxLogical = 20.0f,
                                  double elapsedSec = -1.0, double sinceEventSec = -1.0);
// レイアウトに置く形。showElapsed なら右に「×n」と経過秒（10 秒進まなければ「応答待ち 10 s」）を添える。
void GaugeSpinner(const char* id, uint32_t eventCount, float pxLogical = 20.0f, bool showElapsed = false,
                  double elapsedSec = -1.0, double sinceEventSec = -1.0);

} // namespace dx12e::ui
