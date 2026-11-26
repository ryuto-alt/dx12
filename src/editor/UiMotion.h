#pragma once

// ===== ui::anim — エディタ UI の動き（Graphite Gauge の motion トークンで動く。W1-A4 / GG-3）=====
//
//   float v = ui::anim::Animate(id, hovered ? 1.f : 0.f, ui::anim::dur::kHover);
//
// ・曲線と時間の数値は UiMotionTokens.gen.h（tokens.motion）が正。ここには数値を書かない。
// ・時刻は ui::clock（--ui-deterministic ではフレーム番号 × 1/60 秒）。実時間に依存しない。
// ・OutBack（オーバーシュート）は作らない。Ease は単調で [0,1] の外へ出ない。
// ・Instant() が true の間（決定論 / --background / 仮想入力 / Reduce Motion）は、行き先をそのまま返す。
//   Reduce Motion でも Stepped（進捗・生存ティックの刻み）と待ち（ツールチップの遅延など）は残す。
// ・id ごとの状態は一定フレーム参照されなければ捨てる（無限に増えない）。
//
// ・スレッド: 状態の表は UI スレッド（ImGui のコンテキストを持つスレッド）専用で、ロックは無い。
//   ワーカースレッドから Animate* を呼ばない（進捗などはワーカーが値だけ渡し、UI スレッドで Animate する）。
//
// 実装は UiMotion.cpp（Gui ライブラリ。テストは Gui だけにリンクして検査できる）。

#include "editor/UiMotionTokens.gen.h"

#include <imgui.h>

namespace dx12e::ui::anim
{

enum class Curve : unsigned char { Standard, Exit, Expo, Linear, Spring, Stepped };   // OutBack は作らない

// 曲線。t は 0..1 に丸める。戻りは 0..1（単調）。
//   Standard / Exit / Expo / Linear: tokens の cubic-bezier を x→t（ニュートン法＋二分法）で解いて y を返す。
//   Spring: 臨界減衰（ω = kSpringOmega）の階段応答を、t=1 でちょうど 1 になるよう正規化したもの（settle 時間 = 7/ω 秒を 0..1 に写す）。
//   Stepped: kSteppedSteps 段の階段（t=1 で 1）。★トークンの stepped は「12px ごとに d60」の px 基準だが、ここは距離に依らず等分。A5（進捗）で px 基準の刻みに直す。
//   Spring: 行き先変更（retarget）では呼び出し側が指定した曲線で再スタートする（トークンの『Tween の行き先変更』とは別の選択）。
float Ease(Curve c, float t);

// 臨界減衰ばね x(t) = target + (x0 - target)(1 + ωt)e^{-ωt}（t は秒）。ω = motion_tokens::kSpringOmega。
float SpringValue(float x0, float target, float tSec);

// (id) ごとに target へ向かう値。行き先が変わったら「現在値から」durMs・curve で再開する（retarget）。
// 残りが snapEps 未満になったら終点へスナップする（既定 kSnapUnit は 0..1 の値向け。px の値は AnimatePx）。
// Spring は durMs で打ち切らない（臨界減衰が snapEps に着くまで動く。durMs <= 0 のときだけ即時）。
inline constexpr float kSnapUnit  = 0.003f;   // 0..1 の値のスナップ幅（motion_spec に px 以外の定めが無いので、見た目で 1 段階未満になる幅）
inline constexpr float kSnapColor = 0.002f;   // 色の 1 成分のスナップ幅（8bit の 1/255 = 0.0039 の半分）
float Animate(ImGuiID id, float target, float durMs, Curve curve = Curve::Standard, float snapEps = kSnapUnit);
// px の値向け（残り kSnapPx = 0.5px でスナップ）。
float AnimatePx(ImGuiID id, float targetPx, float durMs, Curve curve = Curve::Standard);
ImVec4 AnimateColor(ImGuiID id, ImVec4 target, float durMs, Curve curve = Curve::Standard);

// 動きを使わないとき true（決定論 / --background / 仮想入力 / Reduce Motion）。
bool Instant();

// ツール群のアクティブ下線をホスト（ToolbarPanel など）が 1 本でスライドさせて描くとき true にする。
// true の間、ToolButtonFace は Graphite でボタンごとの下線を描かない。
void SetToolUnderlineHosted(bool on);
bool ToolUnderlineHosted();

// ---- Instant の入力（呼び出し元から設定する）----
void SetBackground(bool on);          // --background（Application の m_bgOptions.Active() から設定。--headless は別経路で未接続）
void SetVirtualInput(bool on);        // 仮想入力モード
// Reduce Motion の判定（SPI_GETCLIENTAREAANIMATION が FALSE のとき true）。テストでは上書きできる。
bool ReduceMotion();
void SetReduceMotionOverride(int v);  // -1 = OS に従う / 0 = 常に OFF / 1 = 常に ON

// 診断・テスト用: いま保持している id 状態の数、全消去。
int  StateCount();
void Reset();

// 用途ごとの時間（ms）と曲線。UiMotionTokens.gen.h の値を参照するだけ。
namespace dur
{
struct D { float ms; Curve curve; };
constexpr Curve Map(motion_tokens::Curve c)
{
    switch (c)
    {
    case motion_tokens::Curve::Standard: return Curve::Standard;
    case motion_tokens::Curve::Exit:     return Curve::Exit;
    case motion_tokens::Curve::Expo:     return Curve::Expo;
    case motion_tokens::Curve::Linear:   return Curve::Linear;
    case motion_tokens::Curve::Spring:   return Curve::Spring;
    case motion_tokens::Curve::Stepped:  return Curve::Stepped;
    }
    return Curve::Standard;
}
constexpr D From(motion_tokens::Dur d) { return D{ d.ms, Map(d.curve) }; }

inline constexpr D kPress = From(motion_tokens::kPress);
inline constexpr D kHover = From(motion_tokens::kHover);
inline constexpr D kRelease = From(motion_tokens::kRelease);
inline constexpr D kSelectionSlide = From(motion_tokens::kSelectionSlide);
inline constexpr D kUnfocusDim = From(motion_tokens::kUnfocusDim);
inline constexpr D kToolUnderline = From(motion_tokens::kToolUnderline);
inline constexpr D kTabOverlineSlide = From(motion_tokens::kTabOverlineSlide);
inline constexpr D kCaliperSlide = From(motion_tokens::kCaliperSlide);
inline constexpr D kTooltipIn = From(motion_tokens::kTooltipIn);
inline constexpr D kTooltipOut = From(motion_tokens::kTooltipOut);
inline constexpr D kPopupIn = From(motion_tokens::kPopupIn);
inline constexpr D kPaletteIn = From(motion_tokens::kPaletteIn);
inline constexpr D kPaletteOut = From(motion_tokens::kPaletteOut);
inline constexpr D kScrimIn = From(motion_tokens::kScrimIn);
inline constexpr D kToastIn = From(motion_tokens::kToastIn);
inline constexpr D kToastOut = From(motion_tokens::kToastOut);
inline constexpr D kProgressStep = From(motion_tokens::kProgressStep);
inline constexpr D kChevronRotate = From(motion_tokens::kChevronRotate);
inline constexpr D kVernierFade = From(motion_tokens::kVernierFade);
inline constexpr D kSliderJump = From(motion_tokens::kSliderJump);
inline constexpr D kTomboDraw = From(motion_tokens::kTomboDraw);
inline constexpr D kToastReflow = From(motion_tokens::kToastReflow);
inline constexpr D kDropTargetIn = From(motion_tokens::kDropTargetIn);
inline constexpr D kDragGhostIn = From(motion_tokens::kDragGhostIn);
inline constexpr D kTreeGuideDraw = From(motion_tokens::kTreeGuideDraw);
inline constexpr D kDropSettle = From(motion_tokens::kDropSettle);
inline constexpr D kDragReturn = From(motion_tokens::kDragReturn);
inline constexpr D kPlayButton = From(motion_tokens::kPlayButton);
inline constexpr D kPauseDash = From(motion_tokens::kPauseDash);
inline constexpr D kHatchFade = From(motion_tokens::kHatchFade);
inline constexpr D kPlayStop = From(motion_tokens::kPlayStop);
inline constexpr D kPlayStart = From(motion_tokens::kPlayStart);
inline constexpr D kProgressDoneFade = From(motion_tokens::kProgressDoneFade);
inline constexpr D kCameraFrame = From(motion_tokens::kCameraFrame);

// 待ち（Reduce Motion でも残す）
inline constexpr float kTooltipDelayMs = motion_tokens::kTooltipDelayMs;
inline constexpr float kReadoutHoldMs   = motion_tokens::kReadoutHoldMs;
inline constexpr float kToastLifeMs    = motion_tokens::kToastLifeMs;
} // namespace dur

// deco::Ease（Graphite）の slot → 時間と曲線。slot 0=hover 1=press 2=focus 3=選択中のツール。rising = 行き先が 0.5 より大きい。
//   スロットの意味を UiWidgets に埋めず、ここで 1 か所に決める（テストできる）。
constexpr dur::D DecoSlotDur(int slot, bool rising)
{
    return slot == 1 ? (rising ? dur::kPress : dur::kRelease)
         : slot == 2 ? dur::kUnfocusDim
         : slot == 3 ? dur::kToolUnderline
                     : (rising ? dur::kHover : dur::kRelease);
}

// dur の D をそのまま渡す短縮形。
inline float Animate(ImGuiID id, float target, dur::D d, float snapEps = kSnapUnit) { return Animate(id, target, d.ms, d.curve, snapEps); }
inline float AnimatePx(ImGuiID id, float targetPx, dur::D d) { return AnimatePx(id, targetPx, d.ms, d.curve); }
inline ImVec4 AnimateColor(ImGuiID id, ImVec4 target, dur::D d) { return AnimateColor(id, target, d.ms, d.curve); }

} // namespace dx12e::ui::anim
