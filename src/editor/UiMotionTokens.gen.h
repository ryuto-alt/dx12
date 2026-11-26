// ★自動生成: tools/theme/gen_gg_tokens.py（入力 assets/editor/theme/gg/tokens.json、SHA256 = fa050cd1d8a7e6f9a6bf3848fa572463214995df7cca83fc7f87567ef024024d）。手で直さない。
// Graphite Gauge のモーショントークン（tokens.motion）。曲線と時間の数値はここが正で、C++ に数値を書かない（ui::anim が読む）。
#pragma once

namespace dx12e::ui::motion_tokens
{

// cubic-bezier(x1, y1, x2, y2)（CSS と同じ。t は 0..1）
struct CubicBezier { float x1, y1, x2, y2; };
enum class Curve : int { Standard, Exit, Linear, Expo, Spring, Stepped };

inline constexpr CubicBezier kStandard{ 0.2f, 0.8f, 0.2f, 1.0f };
inline constexpr CubicBezier kExit{ 0.4f, 0.0f, 1.0f, 1.0f };
inline constexpr CubicBezier kExpo{ 0.16f, 1.0f, 0.3f, 1.0f };
inline constexpr CubicBezier kLinear{ 0.0f, 0.0f, 1.0f, 1.0f };   // 値だけ残す（UI では使わない）
inline constexpr float kSpringOmega = 14.0f;   // 臨界減衰の角周波数（起動画面の指針 / Tween の行き先変更）
inline constexpr float kSteppedPx = 12.0f;     // 1 段の幅
inline constexpr int   kSteppedSteps = 12;
// 残りがこの px 未満になったら終点へスナップする（standard の note）
inline constexpr float kSnapPx = 0.5f;

// ---- 時間の段（ms）----
inline constexpr float kD60 = 60.0f;
inline constexpr float kD90 = 90.0f;
inline constexpr float kD120 = 120.0f;
inline constexpr float kD160 = 160.0f;
inline constexpr float kD200 = 200.0f;
inline constexpr float kD240 = 240.0f;

struct Dur { float ms; Curve curve; };
// ---- 用途ごとの時間と曲線 ----
inline constexpr Dur kPress{ 60.0f, Curve::Standard };   // 押下の面 bg4
inline constexpr Dur kTooltipOut{ 60.0f, Curve::Exit };   // ツールチップの消去
inline constexpr Dur kHover{ 90.0f, Curve::Standard };   // ホバーの面 bg3
inline constexpr Dur kVernierFade{ 90.0f, Curve::Standard };   // 数値ドラッグのバーニア
inline constexpr Dur kSliderJump{ 90.0f, Curve::Standard };   // スライダーのクリック移動
inline constexpr Dur kRelease{ 120.0f, Curve::Standard };   // 離す・ホバー解除で戻る
inline constexpr Dur kSelectionSlide{ 120.0f, Curve::Standard };   // 選択行の左指針
inline constexpr Dur kUnfocusDim{ 120.0f, Curve::Standard };   // フォーカス外で accentDim へ
inline constexpr Dur kToolUnderline{ 160.0f, Curve::Standard };   // アクティブツールの下線
inline constexpr Dur kTomboDraw{ 160.0f, Curve::Standard };   // 3D 選択トンボの描き込み（腕 0→10）
inline constexpr Dur kTabOverlineSlide{ 160.0f, Curve::Standard };   // タブ上線（x と幅）
inline constexpr Dur kCaliperSlide{ 160.0f, Curve::Standard };   // フォーカス・キャリパーの顎
inline constexpr Dur kPaletteOut{ 90.0f, Curve::Exit };   // パレットと暗幕を閉じる
inline constexpr Dur kTooltipIn{ 90.0f, Curve::Standard };   // ツールチップ（器ごと）＋2px
inline constexpr Dur kPopupIn{ 90.0f, Curve::Standard };   // メニュー（器ごと）＋2px
inline constexpr Dur kPaletteIn{ 120.0f, Curve::Standard };   // パレット本体＋4px
inline constexpr Dur kScrimIn{ 120.0f, Curve::Standard };   // 暗幕 0→34%
inline constexpr Dur kToastOut{ 160.0f, Curve::Exit };   // トースト退場＋4px
inline constexpr Dur kToastReflow{ 160.0f, Curve::Standard };   // 残りのトーストを詰める
inline constexpr Dur kToastIn{ 200.0f, Curve::Standard };   // トースト＋8px
inline constexpr Dur kDropTargetIn{ 90.0f, Curve::Standard };   // 受け先・ドッキング候補の枠
inline constexpr Dur kDragGhostIn{ 90.0f, Curve::Standard };   // ドラッグのゴースト
inline constexpr Dur kChevronRotate{ 90.0f, Curve::Standard };   // ▸ の 90° 回転
inline constexpr Dur kTreeGuideDraw{ 120.0f, Curve::Standard };   // ツリーのガイド線を描き下ろす
inline constexpr Dur kDropSettle{ 160.0f, Curve::Standard };   // ゴーストが自分の行へ収まる
inline constexpr Dur kDragReturn{ 160.0f, Curve::Standard };   // 不可・取り消しで元へ戻る
inline constexpr Dur kProgressStep{ 60.0f, Curve::Stepped };   // 進捗・生存ティックの 1 刻み
inline constexpr Dur kPlayButton{ 90.0f, Curve::Standard };   // 再生ボタンが黄の面へ
inline constexpr Dur kPauseDash{ 120.0f, Curve::Standard };   // 実線→破線（隙間 0→6px）
inline constexpr Dur kHatchFade{ 160.0f, Curve::Standard };   // 編集パネルの断面ハッチ
inline constexpr Dur kPlayStop{ 200.0f, Curve::Standard };   // 枠が四隅へ縮んで消える
inline constexpr Dur kPlayStart{ 240.0f, Curve::Standard };   // 枠が四隅から辺に沿って伸びて 3px の閉じ枠に
inline constexpr Dur kProgressDoneFade{ 240.0f, Curve::Exit };   // 完了・変更の印が消える
inline constexpr Dur kCameraFrame{ 240.0f, Curve::Standard };   // F キーのフレーミング

// ---- 待ち（Reduce Motion でも残す）----
inline constexpr float kTooltipDelayMs = 450.0f;
inline constexpr float kReadoutHoldMs = 600.0f;
inline constexpr float kToastLifeMs = 5000.0f;

// Reduce Motion: 動きの時間を 0 にする。待ち・stepped の刻み・状態の手がかり・音は残す。
inline constexpr float kReduceMotionDurationMs = 0.0f;

} // namespace dx12e::ui::motion_tokens
