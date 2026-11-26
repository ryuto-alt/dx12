#pragma once

// ===========================================================================
// 起動画面 Graphite Gauge（Calibrate）の「動き」の純ロジック（W1-BD2。ヘッダオンリー。Win32 / GPU / 音デバイスに依存しない）
// ---------------------------------------------------------------------------
// 時刻 t から、指針の角度・残像・目盛りの点灯・行き過ぎ・ロック・整列波・ピン・退場を決める 1 本の決定論の関数群。
// 実窓・プレビュー・オフライン同期ダンプ・単体テストが【同じ関数】を通る（描画側は GaugeFrame だけを見る）。
//
// 時刻の正は src/core/SplashCalibrateEvents.h（tools/gen_boot_sound_calibrate.py の生成物。EXEC_W05_W1 §2.6 の
// Calibrate キュー表）。ここに時刻を手で書かない。ここにあるのは「その時刻の間をどう補間するか」だけ。
//
// 3 つの経路（GaugePath）:
//   Fast   … ロードが C23 までに終わる。正規のタイムライン（graphite_1.wav と同じ時計）。EvaluateGauge() が状態を持たずに返す。
//   Slow   … ロードが C23 までに終わらない。状態を持つ GaugeSlowDriver が実進捗をバネ（ω=14、臨界減衰）で追い、
//            完了要求から Calibrate の減速でロックへ送る。ロック以降は Fast と同じ関数（EvaluateAfterLock）。
//   Reduce … Reduce Motion。整列まで終わった静止の姿（alignStep=5、着座、bellT=1。回転と残像なし）。ピンと退場の時計だけ進む。
//            ★要ルーリング: EXEC §2.6 は「ロックの状態（alignStep 0、未着座）から静的に」、PP は「θ = 実進捗」。現実装は整列済みの姿。
//            timelineMs は最後の reveal（5 つ目のベル）の時刻以上に保つ（reveal が動かない）。決まったらここに記録する。
// 「ロック以降はどの経路でも同じ」: Fast / Slow / スキップは、ロックからの経過 ms を EvaluateAfterLock に渡すだけ。
//
// 単位: 時間は ms（double）、角度は度（12 時から時計回り）、長さは論理 px（1600x900 の設計空間）。
// ===========================================================================

#include "core/SplashCalibrateEvents.h"
#include "core/SplashMotion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace dx12e::splash
{

enum class GaugePath : std::uint8_t { Fast, Slow, Reduce };

// 1 フレームぶんの描画パラメータ。ISplashRenderer::Render（BD3）はこれだけを見る。
struct GaugeFrame
{
    float needleDeg = 0.f;          // 指針の角度 0..360。360 に着いたあとは 360 のまま（行き過ぎは overshootPx）
    float blurDeg = 0.f;            // 残像の長さ（度）。直近 1 フレーム（60Hz）に進んだ角度。ロック後と Reduce は 0
    std::uint32_t tickLit = 0;      // 文字盤の目盛りの点灯数 0..kDialTickCount（真上から時計回りに刻む。linear）
    float bellT = 0.f;              // ベゼルの鏡面スイープの進み 0..1（1,648〜1,848。反射の移動として描く）
    float pinT = 0.f;               // 登録のピン（図番 UN-x.y.z の刻印）の進み 0..1（2,148 から 120ms）
    bool locked = false;            // ロック（1,628）以降
    float overshootPx = 0.f;        // 行き過ぎ 0..2px（指針の先端が進行方向＝円周の接線方向へ出る長さ。C23 からロックまでの 85ms）
    std::uint32_t alignStep = 0;    // 整列波の段 0..5（ベルの各音に 1 段ずつ）
    // --- 描画側が欲しい追加分（BD2 で足したもの） ---
    bool needleShown = false;       // 指針が現れた（177ms。0° に静止したまま）
    bool seated = false;            // 着座（1,646。表題欄の状態が「校正中」→「校正済」）
    float tomboT = 0.f;             // 四隅のトンボの進み 0..1（8〜148、linear）
    float fadeOutT = 0.f;           // 窓の退場 0..1（2,148〜2,328 の α フェード。alpha = 1 - fadeOutT）
    float timelineMs = 0.f;         // この絵の正規タイムライン上の時刻。単調（Slow はロックまで lockMs-1e-6 で止まり、ロックで lockMs から再開する）。演出の reveal はこれで決める

    bool operator==(const GaugeFrame&) const = default;
};

// graphite_1.wav の時刻表のビュー（SplashCalibrateEvents.h から作る）。テストで差し替えられるよう値で持つ。
struct CalibrateEvents
{
    static constexpr int kClicks = calibrate::kClickCount;   // 24
    static constexpr int kBells = 5;
    double clickMs[kClicks] = {};
    double clickDeg[kClicks] = {};
    double clickPan[kClicks] = {};
    double lockMs = 0;       // T_lock
    double seatMs = 0;       // ラッチ 2（着座）
    double bellMs[kBells] = {};
    double pingMs = 0;
    double lengthMs = 0;
};

namespace gauge_detail
{
constexpr CalibrateEvents MakeDefaultEvents()
{
    CalibrateEvents e;
    for (int i = 0; i < CalibrateEvents::kClicks; ++i)
    {
        e.clickMs[i] = calibrate::kClickMs[i];
        e.clickDeg[i] = calibrate::kClickAngleDeg[i];
    }
    e.lockMs = calibrate::kLockMs;
    e.lengthMs = calibrate::kLengthMs;
    for (const calibrate::Event& ev : calibrate::kEvents)
    {
        switch (ev.kind)
        {
        case calibrate::EventKind::Click:
            if (ev.index >= 0 && ev.index < CalibrateEvents::kClicks) e.clickPan[ev.index] = ev.pan;
            break;
        case calibrate::EventKind::Latch:
            if (ev.index == 1) e.seatMs = ev.tMs;
            break;
        case calibrate::EventKind::Bell:
            if (ev.index >= 0 && ev.index < CalibrateEvents::kBells) e.bellMs[ev.index] = ev.tMs;
            break;
        case calibrate::EventKind::Ping:
            e.pingMs = ev.tMs;
            break;
        default: break;
        }
    }
    return e;
}
} // namespace gauge_detail

inline const CalibrateEvents& DefaultCalibrateEvents()
{
    static constexpr CalibrateEvents kDefault = gauge_detail::MakeDefaultEvents();
    return kDefault;
}

// ---------------------------------------------------------------- 定数（時刻ではなく「形」の値。時刻は CalibrateEvents）

namespace gauge
{
inline constexpr std::uint32_t kDialTickCount = 60;     // 文字盤の目盛りの数（点灯数 tickLit の上限）
// キュー表（SplashCalibrateEvents.h）から導ける時刻は導く。四隅のトンボ・目盛りの刻み始めは LEAD（風切りの層の開始）、
// 目盛りの刻み終わりは最初のクリック（C00）。
inline constexpr double kTomboStartMs = calibrate::kSweepStartMs;
inline constexpr double kDialStartMs = calibrate::kSweepStartMs;
inline constexpr double kDialEndMs = calibrate::kClickMs[0];
// EXEC §2.6 の Calibrate キュー表の値: トンボ 8〜148ms（140ms）、ベゼルの鏡面スイープ 1,648〜1,848ms（200ms）、窓の α フェード 2,148〜2,328ms（180ms）。
// 暫定値は kPinStampMs（120ms）だけ（キュー表にも GG final の tokens.json にも出どころが無い。BD3 の kGaugeTokens で確定する）。
// 注意: GG の motion.tomboDraw（160ms）は 3D 選択のトンボの値で、ここの起動画面のトンボとは別。
inline constexpr double kTomboDurMs = 140.0;
inline constexpr double kBezelSweepMs = 200.0;
inline constexpr double kPinStampMs = 120.0;
inline constexpr double kFadeOutMs = 180.0;
inline constexpr double kOvershootPx = 2.0;             // 行き過ぎの最大（2px）
inline constexpr double kNeedleRadiusPx = 248.0;        // 指針の半径。行き過ぎは「指針の先端が円周の接線方向（進行方向）へ出る長さ」で、度への換算は OvershootDegOf()
inline constexpr double kBlurWindowMs = 1000.0 / 60.0;  // 残像 = 直近 1 フレームの移動量
inline constexpr double kSlowCapDeg = 345.0;            // 遅い経路の上限（最後の 1 歯はロックのために取っておく）
inline constexpr double kSlowProgressCap = 0.95;        // 遅い経路の目標 = 360 * min(p, 0.95)
inline constexpr double kSlowOmega = 14.0;              // バネの固有角振動数（1/s。臨界減衰）
inline constexpr double kClickMaxRateHz = 30.0;         // 遅い経路の追従中のクリックの上限（GG の ratchet 30 回/秒）
inline constexpr double kLookaheadMs = 50.0;            // 予約は発音の 50ms 以上前に行う
inline constexpr double kMaxFrameMs = 34.0;             // これ以下のフレーム間隔なら予約の先読みが 50ms を割らない
inline constexpr double kDisplayLagMs = kLookaheadMs + kMaxFrameMs;   // 遅い経路は、絵を計算より 84ms 遅らせて出す
inline constexpr double kApproachFullMs = 1275.0;       // 最終接近: 360 度ぶんの所要（残りの角度に比例）
inline constexpr double kApproachMinMs = 300.0;
inline constexpr double kHermiteSlopeLimit = 3.0;       // 単調性の条件（端の傾き <= 3 * 割線）
// 行き過ぎ px を角度（度）へ（先端の円周方向。2px は半径 248 で +0.46 度）。needleDeg には足さない（描画側が使う場合の換算）
inline constexpr double OvershootDegOf(double px) { return px / kNeedleRadiusPx * (180.0 / 3.14159265358979323846); }
} // namespace gauge

// ---------------------------------------------------------------- 単調な区分 3 次エルミート（PCHIP）

struct HermiteSpline
{
    static constexpr int kMaxKnots = 26;
    int n = 0;
    std::array<double, kMaxKnots> t{};
    std::array<double, kMaxKnots> y{};
    std::array<double, kMaxKnots> d{};   // 各結び目の傾き（度/ms）

    void Clear() { n = 0; }
    void Add(double tt, double yy) { t[static_cast<std::size_t>(n)] = tt; y[static_cast<std::size_t>(n)] = yy; ++n; }

    // 内側の傾きは Fritsch-Butland（重み付き調和平均。単調データなら単調）。端の傾きは引数（C1 のため呼び出し側が決める）。
    void Build(double d0, double dn)
    {
        const auto idx = [](int i) { return static_cast<std::size_t>(i); };
        if (n < 2) { if (n == 1) d[0] = 0.0; return; }
        const auto secant = [&](int k) { return (y[idx(k + 1)] - y[idx(k)]) / (t[idx(k + 1)] - t[idx(k)]); };
        d[0] = d0;
        d[idx(n - 1)] = dn;
        for (int k = 1; k < n - 1; ++k)
        {
            const double h0 = t[idx(k)] - t[idx(k - 1)], h1 = t[idx(k + 1)] - t[idx(k)];
            const double s0 = secant(k - 1), s1 = secant(k);
            if (s0 * s1 <= 0.0) { d[idx(k)] = 0.0; continue; }
            const double w1 = 2.0 * h1 + h0, w2 = h1 + 2.0 * h0;
            d[idx(k)] = (w1 + w2) / (w1 / s0 + w2 / s1);
        }
    }

    // 区間の探索（線形。n <= 26）
    int Segment(double x) const
    {
        int k = 0;
        while (k < n - 2 && x >= t[static_cast<std::size_t>(k + 1)]) ++k;
        return k;
    }
    double Eval(double x) const
    {
        if (n == 0) return 0.0;
        if (n == 1 || x <= t[0]) return y[0];
        if (x >= t[static_cast<std::size_t>(n - 1)]) return y[static_cast<std::size_t>(n - 1)];
        const auto k = static_cast<std::size_t>(Segment(x));
        const double h = t[k + 1] - t[k];
        const double s = (x - t[k]) / h;
        const double s2 = s * s, s3 = s2 * s;
        return (2 * s3 - 3 * s2 + 1) * y[k] + (s3 - 2 * s2 + s) * h * d[k] + (-2 * s3 + 3 * s2) * y[k + 1] + (s3 - s2) * h * d[k + 1];
    }
    double Slope(double x) const   // 度/ms
    {
        if (n < 2 || x <= t[0] || x >= t[static_cast<std::size_t>(n - 1)]) return 0.0;
        const auto k = static_cast<std::size_t>(Segment(x));
        const double h = t[k + 1] - t[k];
        const double s = (x - t[k]) / h;
        const double s2 = s * s;
        return ((6 * s2 - 6 * s) * y[k] + (3 * s2 - 4 * s + 1) * h * d[k] + (-6 * s2 + 6 * s) * y[k + 1] + (3 * s2 - 2 * s) * h * d[k + 1]) / h;
    }
};

// ---------------------------------------------------------------- 正規タイムライン（Fast。ロック以降は全経路で共通）

namespace gauge_detail
{
// 指針が 0° に現れる時刻 = 最初のクリック − 最初の間隔（EXEC §2.6: 268 − 91 = 177ms）
inline double NeedleStartMs(const CalibrateEvents& ev) { return 2.0 * ev.clickMs[0] - ev.clickMs[1]; }

// C23 からロックまでの行き過ぎ区間の長さ（85ms）。遅い経路の「最後の歯 + 85ms」もこれ
inline double OvershootSpanMs(const CalibrateEvents& ev) { return ev.lockMs - ev.clickMs[CalibrateEvents::kClicks - 1]; }

// 行き過ぎ（px）。u = C23 からの経過 ms。16 s^2 (1-s)^2: 0 から出て 2px で折り返し、ロックの 1 点で 0 に戻る（傾きも 0 = C1）
inline double OvershootPxAt(double uMs, double spanMs)
{
    if (uMs <= 0.0 || uMs >= spanMs) return 0.0;
    const double s = uMs / spanMs, q = s * (1.0 - s);
    return gauge::kOvershootPx * 16.0 * q * q;
}

// 正規の指針の角度のスプライン: (177ms, 0°) と C00..C23 の点を通る。始点と終点の傾きは 0（現れて静止 → 着座で止まる。ロックの行き過ぎと C1）
inline HermiteSpline MakeCanonicalNeedle(const CalibrateEvents& ev)
{
    HermiteSpline sp;
    sp.Add(NeedleStartMs(ev), 0.0);
    for (int i = 0; i < CalibrateEvents::kClicks; ++i) sp.Add(ev.clickMs[i], ev.clickDeg[i]);
    sp.Build(0.0, 0.0);
    return sp;
}

// 指針以外の要素（時刻だけで決まる）。timelineMs = 正規タイムライン上の時刻。
inline void FillItems(const CalibrateEvents& ev, double t, GaugeFrame& f)
{
    f.timelineMs = static_cast<float>(t);
    f.tomboT = static_cast<float>(Clamp01((t - gauge::kTomboStartMs) / gauge::kTomboDurMs));
    f.tickLit = static_cast<std::uint32_t>(std::floor(gauge::kDialTickCount * Clamp01((t - gauge::kDialStartMs) / (gauge::kDialEndMs - gauge::kDialStartMs)) + 1e-9));
    f.needleShown = t >= NeedleStartMs(ev);
    f.locked = t >= ev.lockMs;
    f.seated = t >= ev.seatMs;
    std::uint32_t step = 0;
    for (double b : ev.bellMs) if (t >= b) ++step;
    f.alignStep = step;
    f.bellT = static_cast<float>(Clamp01((t - ev.bellMs[0]) / gauge::kBezelSweepMs));
    f.pinT = static_cast<float>(EaseOutCubic((t - ev.pingMs) / gauge::kPinStampMs));
    f.fadeOutT = static_cast<float>(Clamp01((t - ev.pingMs) / gauge::kFadeOutMs));
}

inline GaugeFrame CanonicalFrame(const CalibrateEvents& ev, const HermiteSpline& needle, double t)
{
    GaugeFrame f;
    FillItems(ev, t, f);
    const double th = needle.Eval(t);
    f.needleDeg = static_cast<float>(th);
    f.blurDeg = f.locked ? 0.f : static_cast<float>(std::max(0.0, th - needle.Eval(t - gauge::kBlurWindowMs)));
    f.overshootPx = static_cast<float>(OvershootPxAt(t - ev.clickMs[CalibrateEvents::kClicks - 1], OvershootSpanMs(ev)));
    return f;
}
} // namespace gauge_detail

// 退場（ピン・窓のフェード）の開始時刻。ロードが終わる（完了要求を観測する）前に退場が進まないよう、自然な開始時刻（ピンの時刻）と
// 完了要求の観測時刻の遅いほうにする。finishMs が NaN（まだ完了要求を観測していない）なら +inf（開始しない = ピン 0・フェード 0 で待つ）。
// 時刻の基準は呼び出し側で揃える（実時計 ms、またはロックからの経過 ms）。
inline double GaugeExitStartMs(double naturalMs, double finishMs)
{
    if (std::isnan(finishMs)) return std::numeric_limits<double>::infinity();
    return std::max(naturalMs, finishMs);
}

namespace gauge_detail
{
// 退場の時計だけを差し替える。dt = 退場の開始からの経過 ms（負・-inf は「まだ」= ピン 0・フェード 0）
inline void ApplyExitClock(GaugeFrame& f, double dtMs)
{
    f.pinT = dtMs >= 0.0 ? static_cast<float>(EaseOutCubic(dtMs / gauge::kPinStampMs)) : 0.f;
    f.fadeOutT = dtMs >= 0.0 ? static_cast<float>(Clamp01(dtMs / gauge::kFadeOutMs)) : 0.f;
}
} // namespace gauge_detail

// ロックからの経過 ms（>= 0）に対する絵。Fast / Slow / スキップがすべてこれを通る（ロック以降はビット一致）。
// exitStartSinceLockMs: 退場（ピン・フェード）の開始を「ロックからの経過 ms」で指す。NaN（既定）= 自然な時刻（ev.pingMs）。
// +inf = まだ始めない（ロードが終わっていない。ピン 0・フェード 0 で保持）。有限値 = その時刻から始める（GaugeExitStartMs の結果をロック基準にして渡す）。
inline GaugeFrame EvaluateAfterLock(double sinceLockMs, const CalibrateEvents& ev = DefaultCalibrateEvents(), double exitStartSinceLockMs = std::numeric_limits<double>::quiet_NaN())
{
    GaugeFrame f = gauge_detail::CanonicalFrame(ev, gauge_detail::MakeCanonicalNeedle(ev), ev.lockMs + std::max(0.0, sinceLockMs));
    if (!std::isnan(exitStartSinceLockMs)) gauge_detail::ApplyExitClock(f, std::max(0.0, sinceLockMs) - exitStartSinceLockMs);
    return f;
}

// スキップ（キー・クリック）: 絵はその時点でロックのフレームへ飛ぶ。以降の正規タイムライン上の時刻を返す。
// ロック以降（tSkip >= lockMs）のスキップは何もしない（時計を巻き戻さない。整列波・ロック音を再演しない）。
inline double GaugeSkippedTimelineMs(double tNowMs, double tSkipMs, const CalibrateEvents& ev = DefaultCalibrateEvents())
{
    if (tSkipMs >= ev.lockMs) return tNowMs;
    return ev.lockMs + std::max(0.0, tNowMs - tSkipMs);
}

// 状態を持たない評価。Fast: 正規のタイムライン。Reduce: ロックの状態の静止フレーム（ピン・退場だけ時計で進む）。
// Slow: 非対応（状態が要る）。GaugeSlowDriver を使うこと。GaugeFrame{}（全 0）を返す。
// Reduce の exitStartMs: 退場（ピン・フェード）の開始（時計 tMs と同じ基準）。NaN（既定）= ev.pingMs（ロードを待たない従来の動き）、
// +inf = まだ始めない（ロード未完。ピン 0・フェード 0）、有限値 = GaugeExitStartMs(ev.pingMs, 完了要求の時刻)。
inline GaugeFrame EvaluateGauge(GaugePath path, double tMs, float loadProgress01, const CalibrateEvents& ev = DefaultCalibrateEvents(),
                                double exitStartMs = std::numeric_limits<double>::quiet_NaN())
{
    const HermiteSpline needle = gauge_detail::MakeCanonicalNeedle(ev);
    switch (path)
    {
    case GaugePath::Fast:
        return gauge_detail::CanonicalFrame(ev, needle, std::max(0.0, tMs));
    case GaugePath::Reduce:
    {
        // ロックの整列が終わったあとの静止した姿。動き（回転・残像・行き過ぎ・整列波のスイープ）は無い。
        GaugeFrame f = gauge_detail::CanonicalFrame(ev, needle, std::max(ev.bellMs[CalibrateEvents::kBells - 1], std::max(0.0, tMs)));
        f.needleDeg = 360.f; f.blurDeg = 0.f; f.overshootPx = 0.f; f.bellT = 1.f;
        const double t = std::max(0.0, tMs);   // timelineMs は CanonicalFrame の値（最後のベルの時刻以上）のまま。reveal は動かない
        const double exitStart = std::isnan(exitStartMs) ? ev.pingMs : exitStartMs;
        f.pinT = t >= exitStart ? 1.f : 0.f;                       // 動かさない: 刻印は出るだけ
        f.fadeOutT = t >= exitStart ? static_cast<float>(Clamp01((t - exitStart) / gauge::kFadeOutMs)) : 0.f;
        return f;
    }
    case GaugePath::Slow:
    default:
        break;   // 非対応: 状態が要るので GaugeSlowDriver / GaugeFastDriver を使う。全フィールド 0 の GaugeFrame{} を返す（描画しても何も出ない）
    }
    (void)loadProgress01;
    return GaugeFrame{};
}

// ---------------------------------------------------------------- 遅い経路（状態を持つ）

enum class GaugeCueKind : std::uint8_t { Click, LockTail };

// 音の予約。再生時刻 playAtMs は Update に渡した時計と同じ ms。Click は click_NN.wav（パンつき）、LockTail は lock_tail.wav。
struct GaugeCue
{
    double playAtMs = 0;      // 鳴らす時刻（絵の上で指針が歯を跨ぐ時刻と一致する）
    double issuedAtMs = 0;    // 予約した時刻（Update の tMs）。playAtMs - issuedAtMs >= kLookaheadMs（スキップの LockTail を除く）
    GaugeCueKind kind = GaugeCueKind::Click;
    int index = 0;            // Click: 歯の番号 0..23
    float pan = 0.f;          // -1..1
    std::uint32_t id = 0;     // 予約の通し番号（1 から。ドライバごとに単調）。スキップの取り消し（TakeCancelledCueIds）はこの id で指す
};

// 使い方: 起動時に生成し、フレームごとに Update(t, 実進捗, 完了要求) → Frame(t) と TakeCues()。t は生成からの ms。
//   ・Frame(t) は【直近の Update に渡した t】（またはそれ以降。ロック後の全フレームの照会を含む）に対して呼ぶ。
//   ・スレッド: 全体がスプラッシュのスレッド専用（同期なし）。TakeCues / PendingCues も Update と同じスレッドから呼ぶ
//     （BD5 の音のスレッドへは、取り出した GaugeCue を値で渡す）。
//   ・時計は 2 本。実時計（Update の t）と、計算の時計 S（バネ・歯の検出）。S は 1 回の Update で最大 kMaxFrameMs しか進まず、
//     進み残し（ヒッチ）は欠損 D = t - S に積む（D は単調増加。最終接近が始まったら固定）。絵は S - kDisplayLagMs の時点を出し、
//     予約は playAt = 歯の S + kDisplayLagMs + D。こうすると、どんなフレーム間隔でも playAt - issuedAt >= kLookaheadMs（50ms）が
//     成り立つ（歯は S の直近 kMaxFrameMs 以内にしか生まれないため）。ヒッチの間は絵が止まって見え（先へ飛ばない）、音と絵は揃ったまま。
//     絵の時計 p(wall) は「発行済みの予約の (playAt, 歯の S)」を結び目にした折れ線（結び目の外は傾き 1。最終接近の前は S 以下に頭打ち）。
//     ヒッチで D が増えても、発行済みの歯は playAt ちょうどに絵が跨ぐ（音が主時計で、絵が音に合わせる。ヒッチ後の絵は前へ飛び、次の結び目へ緩やかにつながる）。
//   ・フレーム間隔が常に 34ms を超える環境（20〜25fps）: 完了要求の前は S が 1 回 34ms しか進まず、指針は実時間より遅く動き、D は増え続ける
//     （先読みは保たれる。完了要求のあとの最終接近は D を固定するので、音と絵は揃ってロックする）。
//   ・完了要求が来たら残りの歯を Calibrate の減速で送り、最後の歯 + 85ms にロックする。以後は EvaluateAfterLock と同じ。
//   ・指針の上限は 345°（最後の 1 歯はロックのために取っておく）。進捗は単調（減ったら無視）。指針は NeedleStartMs（177ms）まで 0° のまま。
class GaugeSlowDriver
{
public:
    explicit GaugeSlowDriver(const CalibrateEvents& ev = DefaultCalibrateEvents())
        : m_ev(ev) {}

    // 引数 t は実時計 ms。Update の間隔は任意（1 回のヒッチが何 ms でも先読みは保たれる）。
    void Update(double tMs, float loadProgress01, bool finishRequested)
    {
        tMs = std::max(tMs, m_lastWall);
        if (finishRequested && !m_finish) { m_finish = true; m_finishWall = tMs; }   // スキップ後も完了要求は記録する（退場はロードの完了まで待つ）
        if (m_skipped) return;
        m_progress = std::max(m_progress, static_cast<double>(Clamp01(loadProgress01)));
        if (!m_approach)
        {
            const double pNow = PictureTime(tMs);              // 今の絵の時計（新しい結び目をつなぐ起点）
            const std::size_t firstNew = m_knots.size();
            const double dt = tMs - m_lastWall;
            const double adv = std::min(dt, gauge::kMaxFrameMs);
            m_deficit += dt - adv;
            Follow(m_lastT + adv, tMs);
            if (finishRequested) BeginApproach(m_lastT, tMs);
            // 新しい予約があって、今が最後の結び目より後（傾き 1 の外挿の上）なら、今の点を結び目に足して絵が戻らないようにする
            if (m_knots.size() > firstNew && (firstNew == 0 || tMs > m_knots[firstNew - 1].w))
                m_knots.insert(m_knots.begin() + static_cast<std::ptrdiff_t>(firstNew), Knot{ tMs, pNow });
        }
        m_lastWall = tMs;
        std::erase_if(m_taken, [&](const TakenCue& k) { return k.playAtMs < m_lastWall; });   // もう鳴った予約は取り消し対象から外す
        Trim();
    }

    // 「指針が wallMs に deg 度で止まっている」状態から始める（GaugeFastDriver の持ち替え用）。Update より前に 1 回だけ呼ぶ。
    void StartHeld(double wallMs, double deg)
    {
        m_lastWall = m_lastT = wallMs;
        m_deficit = 0.0;
        m_theta = deg;
        m_omega = 0.0;
        m_holdDeg = deg;                 // 進捗が低くても、持ち替えた角度より戻らない
        m_timeOffsetMs = gauge::kDisplayLagMs;   // 絵の時計は wall - 84 から始まるので、timelineMs だけ 84 足して正規の時刻から連続させる（指針は 345 度で止まっているので影響しない）
        m_knots.clear();
        m_nextTooth = 0;
        while (m_nextTooth < CalibrateEvents::kClicks - 1 && m_ev.clickDeg[m_nextTooth] <= deg + 1e-9) ++m_nextTooth;
        m_segs.clear();
        m_segs.push_back(Seg{ -1e9, wallMs, deg, 0.0, 0.0 });
    }

    // スキップ: ロックより前なら、絵は tMs にロックのフレームへ飛ぶ。ロック以降（最終接近が決まっていて tMs >= LockWallMs）は何もしない。tMs より先の未取得の予約は捨て、LockTail を tMs に即時で入れる
    //（鳴っているボイスの 30ms フェードアウトと lock_tail の頭出しは音の側）。
    void Skip(double tMs)
    {
        if (m_skipped) return;
        tMs = std::max(tMs, m_lastWall);               // 契約: スキップ時刻 >= 直近の Update 時刻（それより前なら Update 時刻へ丸める）
        if (m_approach && tMs >= m_lockWall) return;   // ロック以降のスキップは何もしない（巻き戻さない・ロック音を重ねない）
        m_skipped = true;
        m_skipWall = tMs;
        std::erase_if(m_cues, [&](const GaugeCue& c) { return c.playAtMs >= tMs; });   // 未取得のうち tMs 以降のもの（ちょうど tMs も含む）は捨てる
        // 取り出し済み（音の側へ渡した）で、まだ鳴っていない予約（playAt >= tMs。ちょうど tMs も「まだ鳴っていない」側。Click も最終接近の LockTail も含む）の取り消しを積む。音の側は TakeCancelledCueIds() の id を
        // 予約表から外し、鳴っているボイスの 30ms フェードアウトと LockTail の頭出しとあわせて行う（取り消さないと、スキップ後にクリックが鳴る）
        for (const TakenCue& k : m_taken)
            if (k.playAtMs >= tMs) m_cancelIds.push_back(k.id);
        m_taken.clear();
        GaugeCue c;
        c.playAtMs = tMs; c.issuedAtMs = tMs; c.kind = GaugeCueKind::LockTail;
        c.id = ++m_nextCueId;
        m_cues.push_back(c);
    }

    // たまった予約を取り出す（以後は空）。取り出した予約は、スキップのときに取り消しの対象になりうる（TakeCancelledCueIds）
    std::vector<GaugeCue> TakeCues()
    {
        std::vector<GaugeCue> out;
        out.swap(m_cues);
        for (const GaugeCue& c : out)
            if (!m_skipped) m_taken.push_back(TakenCue{ c.id, c.playAtMs });   // Click も最終接近の LockTail も記録（スキップ後の LockTail は取り消さないので記録しない）
        return out;
    }
    // スキップで取り消す予約の id（TakeCues で受け渡し済みで、スキップ時刻より後に鳴るはずだったもの）。取り出すと空になる。
    // スキップしなければ常に空。最終接近の LockTail も取り消す（スキップの LockTail は TakeCues の側にあり、取り消さない。音の側が鳴らす LockTail は 1 本だけになる）
    std::vector<std::uint32_t> TakeCancelledCueIds() { std::vector<std::uint32_t> out; out.swap(m_cancelIds); return out; }
    const std::vector<GaugeCue>& PendingCues() const { return m_cues; }

    bool Skipped() const { return m_skipped; }
    bool Approaching() const { return m_approach; }
    // 最終接近が絵に現れる時刻（Update の時計）。完了要求の前は NaN
    double ApproachWallMs() const { return m_approach ? m_approachWall : std::nan(""); }
    // ロックの絵が出る時刻（Update の時計）。完了要求の前は NaN。Frame(LockWallMs()) は EvaluateAfterLock(0) と一致する
    double LockWallMs() const
    {
        if (m_skipped) return m_skipWall;
        return m_approach ? m_lockWall : std::nan("");
    }

    // 退場（ピン・フェード）が実際に始まる時刻（Update の時計）。スキップ後は max(ロックの絵 + 自然な間隔, 完了要求の観測時刻)。
    // スキップ後に完了要求が来るまでは NaN（ロード中は窓を出したまま、ピン 0・フェード 0 で保持する）。スキップしない経路は最終接近の前まで NaN。
    // 窓を閉じる・図番を出す側（BD3 / BD5）はこの時刻を使う。
    double ExitStartWallMs() const
    {
        if (m_skipped) return m_finish ? m_skipWall + SkipExitSinceLock() : std::nan("");
        return m_approach ? m_lockWall + (m_ev.pingMs - m_ev.lockMs) : std::nan("");
    }
    bool FinishObserved() const { return m_finish; }

    // 指針の角度（倍精度。行き過ぎは含まない）。解析・テスト用
    double NeedleDegAt(double tMs) const { return NeedleAt(tMs); }

    GaugeFrame Frame(double tMs) const
    {
        if (m_skipped && tMs >= m_skipWall) return EvaluateAfterLock(tMs - m_skipWall, m_ev, SkipExitSinceLock());
        if (m_approach && tMs >= m_lockWall) return EvaluateAfterLock(tMs - m_lockWall, m_ev);
        const double d = PictureTime(tMs);

        GaugeFrame f;
        gauge_detail::FillItems(m_ev, std::min(std::max(d + m_timeOffsetMs, 0.0), m_ev.lockMs - 1e-6), f);   // timelineMs も単調（ロックまで lockMs-1e-6 で止まる）
        const double th = NeedleAtPicture(d);
        f.needleDeg = static_cast<float>(th);
        f.blurDeg = static_cast<float>(std::max(0.0, th - NeedleAtPicture(d - gauge::kBlurWindowMs)));
        if (m_approach && d >= m_toothLastMs)
            f.overshootPx = static_cast<float>(gauge_detail::OvershootPxAt(d - m_toothLastMs, gauge_detail::OvershootSpanMs(m_ev)));
        return f;
    }

private:
    // スキップ後の退場の開始（ロックの絵からの経過 ms）。完了要求の前は +inf
    double SkipExitSinceLock() const
    {
        return GaugeExitStartMs(m_ev.pingMs - m_ev.lockMs, m_finish ? m_finishWall - m_skipWall : std::nan(""));
    }

    struct Seg { double t0, t1, target, a, b; };   // x(u) = (a + b u) e^{-w u} + target

    struct Knot { double w, p; };   // 実時計 w に絵の時計（S 軸）が p

    // 絵の時計 p(wall): 結び目の折れ線。最初の結び目より前は wall - 84、最後の結び目より後は傾き 1
    //（最終接近の前は S を超えない: 後から生まれる歯は必ず S より先にあるので、絵が歯を追い越さない）。
    double PictureTime(double wallMs) const
    {
        const double capS = m_approach ? 1e18 : m_lastT;
        if (m_knots.empty()) return std::min(wallMs - gauge::kDisplayLagMs, capS);
        if (wallMs < m_knots.front().w) return std::min(wallMs - gauge::kDisplayLagMs, m_knots.front().p);
        const auto it = std::upper_bound(m_knots.begin(), m_knots.end(), wallMs, [](double x, const Knot& k) { return x < k.w; });
        if (it == m_knots.end()) return std::min(m_knots.back().p + (wallMs - m_knots.back().w), capS);
        const Knot& a = *(it - 1);
        const Knot& b = *it;
        return a.p + (b.p - a.p) * (wallMs - a.w) / (b.w - a.w);
    }

    double SegTheta(const Seg& s, double t) const
    {
        const double u = (t - s.t0) * 1e-3;
        return std::min(gauge::kSlowCapDeg, std::max(0.0, s.target + (s.a + s.b * u) * std::exp(-gauge::kSlowOmega * u)));
    }

    // simEnd までバネを 1 区間で進める（simEnd - m_lastT <= kMaxFrameMs）。wallNow は予約の issuedAt。
    void Follow(double simEnd, double wallNow)
    {
        const double dt = simEnd - m_lastT;
        if (dt <= 0.0) return;
        // 指針が現れる（NeedleStartMs）まではバネを 0° に保つ（早い進捗で途中の角度に突然現れない）
        const double target = (m_lastT < gauge_detail::NeedleStartMs(m_ev)) ? 0.0
            : std::max(m_holdDeg, std::min(gauge::kSlowCapDeg, 360.0 * std::min(m_progress, gauge::kSlowProgressCap)));
        const double w = gauge::kSlowOmega;
        Seg s;
        s.t0 = m_lastT; s.t1 = simEnd; s.target = target;
        s.a = m_theta - target;
        s.b = m_omega + w * s.a;                       // m_omega は度/s
        const double u = dt * 1e-3, e = std::exp(-w * u);
        const double newTheta = std::min(gauge::kSlowCapDeg, std::max(0.0, target + (s.a + s.b * u) * e));
        const double newOmega = (s.b - w * (s.a + s.b * u)) * e;

        // 歯を跨いだ時刻（二分探索）と予約。追従中は 30 回/s まで。
        while (m_nextTooth < CalibrateEvents::kClicks - 1 && newTheta >= m_ev.clickDeg[m_nextTooth])
        {
            const double deg = m_ev.clickDeg[m_nextTooth];
            double lo = s.t0, hi = s.t1;
            for (int i = 0; i < 40; ++i)
            {
                const double mid = 0.5 * (lo + hi);
                (SegTheta(s, mid) >= deg ? hi : lo) = mid;
            }
            if (hi - m_lastCueSim >= 1000.0 / gauge::kClickMaxRateHz)
            {
                m_lastCueSim = hi;
                PushClick(m_nextTooth, hi, wallNow);
            }
            ++m_nextTooth;
        }
        m_segs.push_back(s);
        m_theta = newTheta;
        m_omega = newOmega;
        m_lastT = simEnd;
    }

    void PushClick(int tooth, double simMs, double issuedWallMs)
    {
        GaugeCue c;
        c.playAtMs = simMs + gauge::kDisplayLagMs + m_deficit;
        m_knots.push_back(Knot{ c.playAtMs, simMs });   // この歯を絵が跨ぐ時刻 = 音の再生時刻
        c.issuedAtMs = issuedWallMs;
        c.kind = GaugeCueKind::Click;
        c.index = tooth;
        c.pan = static_cast<float>(m_ev.clickPan[tooth]);
        c.id = ++m_nextCueId;
        m_cues.push_back(c);
    }

    // 完了要求: 現在の (θ, ω) から残りの歯を Calibrate の減速で送る。fMs は計算の時計 S、wallNow は実時計。
    // 結び目 = (F, θ0) と、残りの歯（時刻は正規の間隔を所要 D に伸縮）。始点の傾きは今の角速度（C1）、終点の傾きは 0。
    // 以降 m_deficit は固定（ここで全ての予約が決まる）。
    void BeginApproach(double fMs, double wallNow)
    {
        m_approach = true;
        fMs = std::max(fMs, gauge_detail::NeedleStartMs(m_ev));   // 指針が現れる前に動かさない（早すぎる完了要求は現れる時刻まで待つ）
        m_fApp = fMs;
        m_approachWall = fMs + gauge::kDisplayLagMs + m_deficit;
        const int last = CalibrateEvents::kClicks - 1;
        const double theta0 = m_theta;
        int m = 0;                                           // 跨ぎ済みの歯の数
        while (m < CalibrateEvents::kClicks && m_ev.clickDeg[m] <= theta0 + 1e-9) ++m;
        m = std::min(m, last);                               // 345° までしか来ないので m <= 23。念のため
        const double anchor = (m == 0) ? gauge_detail::NeedleStartMs(m_ev) : m_ev.clickMs[m - 1];
        const double span = m_ev.clickMs[last] - anchor;

        double dMs = std::clamp((360.0 - theta0) / 360.0 * gauge::kApproachFullMs, gauge::kApproachMinMs, gauge::kApproachFullMs);
        double scale = dMs / span;
        double v0 = std::max(0.0, m_omega * 1e-3);           // 度/ms
        // 単調性（始点の傾き <= 3 x 最初の割線）: 今の角速度が速いときは最初の歯までを短くして満たす（以降の歯も同じだけ前へ詰める）
        double firstH = (m_ev.clickMs[m] - anchor) * scale;
        double pull = 0.0;
        if (v0 > 0.0)
        {
            const double maxH = gauge::kHermiteSlopeLimit * (m_ev.clickDeg[m] - theta0) / v0 / 1.001;
            if (firstH > maxH) pull = firstH - maxH;
        }
        m_spline.Clear();
        m_spline.Add(fMs, theta0);
        for (int j = m; j <= last; ++j) m_spline.Add(fMs + (m_ev.clickMs[j] - anchor) * scale - pull, m_ev.clickDeg[j]);
        m_spline.Build(v0, 0.0);

        m_toothLastMs = m_spline.t[static_cast<std::size_t>(m_spline.n - 1)];
        m_lockSim = m_toothLastMs + gauge_detail::OvershootSpanMs(m_ev);
        m_lockWall = m_lockSim + gauge::kDisplayLagMs + m_deficit;
        for (int j = m; j <= last; ++j) PushClick(j, m_spline.t[static_cast<std::size_t>(1 + j - m)], wallNow);
        m_knots.push_back(Knot{ m_lockWall, m_lockSim });   // 歯の結び目の後ろ（w 昇順）
        GaugeCue lock;
        lock.playAtMs = m_lockWall;
        lock.issuedAtMs = wallNow;
        lock.kind = GaugeCueKind::LockTail;
        lock.id = ++m_nextCueId;
        m_cues.push_back(lock);
    }

    // d = 絵の時計（S 軸）。最終接近以降は spline、それ以前は履歴
    double NeedleAtPicture(double d) const
    {
        if (d <= 0.0) return 0.0;
        if (m_approach && d >= m_fApp) return m_spline.Eval(d);
        if (m_segs.empty()) return 0.0;
        auto it = std::lower_bound(m_segs.begin(), m_segs.end(), d, [](const Seg& s, double x) { return s.t1 < x; });
        if (it == m_segs.end()) return m_theta;
        if (d < it->t0) return SegTheta(*it, it->t0);
        return SegTheta(*it, d);
    }
    double NeedleAt(double wallMs) const { return NeedleAtPicture(PictureTime(wallMs)); }

    // 履歴は 5 秒分だけ保持（それより古い時刻を Frame に渡すと不定）
    void Trim()
    {
        const double keepFrom = m_lastT - gauge::kDisplayLagMs - 5000.0;
        std::size_t drop = 0;
        while (drop + 1 < m_segs.size() && m_segs[drop].t1 < keepFrom) ++drop;
        if (drop) m_segs.erase(m_segs.begin(), m_segs.begin() + static_cast<std::ptrdiff_t>(drop));
        std::size_t kdrop = 0;
        while (kdrop + 1 < m_knots.size() && m_knots[kdrop + 1].w < m_lastWall - 6000.0) ++kdrop;
        if (kdrop) m_knots.erase(m_knots.begin(), m_knots.begin() + static_cast<std::ptrdiff_t>(kdrop));
    }

    CalibrateEvents m_ev;
    std::vector<Seg> m_segs;
    std::vector<GaugeCue> m_cues;
    std::vector<Knot> m_knots;              // 絵の時計の結び目（発行済みの予約。w 昇順）
    double m_timeOffsetMs = 0;              // timelineMs への加算（持ち替え用）
    HermiteSpline m_spline;                 // 最終接近
    double m_lastWall = 0;                  // 直近の Update の実時計
    double m_lastT = 0;                     // 計算の時計 S
    double m_deficit = 0;                   // D = 実時計 - S（ヒッチの積み。最終接近の開始で固定）
    double m_theta = 0, m_omega = 0;        // 度、度/s（S の時点）
    double m_progress = 0;
    double m_holdDeg = 0;                   // 指針の下限（StartHeld の角度）
    int m_nextTooth = 0;
    double m_lastCueSim = -1e9;
    bool m_approach = false;
    double m_fApp = 0, m_toothLastMs = 0, m_lockSim = 0;
    double m_approachWall = 0, m_lockWall = 0;
    bool m_skipped = false;
    double m_skipWall = 0;
    bool m_finish = false;                  // 完了要求を観測した（スキップの後でも記録する）
    double m_finishWall = 0;                // 観測した Update の時刻
    struct TakenCue { std::uint32_t id; double playAtMs; };
    std::vector<TakenCue> m_taken;          // TakeCues で渡した予約（Click と最終接近の LockTail。Skip で playAt >= tSkip のものを取り消す）
    std::vector<std::uint32_t> m_cancelIds; // Skip が積んだ取り消し
    std::uint32_t m_nextCueId = 0;
};

// ---------------------------------------------------------------- 速い経路の「ロードが C22 までに終わらなかった」ときの縮退
//
// PROGRAM_PLAN BA-2 の規則: 完了要求が C22（1,475ms）より後なら、C22 の歯（345 度）で止めて待ち、完了要求から最終接近でロックする。
// 速い経路は t=0 では選べない（ロードがいつ終わるか分からない）ので、最初は正規のタイムライン（graphite_1.wav と同じ時計）を出し、
// C22 の時刻でまだ完了要求が来ていなければ GaugeSlowDriver（345 度で静止、残りは C23 の 1 歯 + ロック）へ持ち替える。
//   ・持ち替えの瞬間の角速度は C22 の正規の速度（約 300 度/s）から 0 へ段差で止まる（C0。C22 の絵は 345 度で一致）。
//   ・持ち替え後の予約（C23 とロックの LockTail）は GaugeSlowDriver と同じ。持ち替える前の音は graphite_1.wav 側（BD5 が C22 で止める判断をする）。
//   ・完了要求が C22 より前なら最後まで正規（ロックは ev.lockMs）。以降の全フレームは EvaluateAfterLock と同じ。
// スレッド: GaugeSlowDriver と同じ（スプラッシュのスレッド専用）。
class GaugeFastDriver
{
public:
    explicit GaugeFastDriver(const CalibrateEvents& ev = DefaultCalibrateEvents())
        : m_ev(ev), m_slow(ev), m_decisionMs(ev.clickMs[CalibrateEvents::kClicks - 2]) {}

    void Update(double tMs, float loadProgress01, bool finishRequested)
    {
        m_lastWall = std::max(tMs, m_lastWall);
        if (finishRequested && !m_finish) { m_finish = true; m_finishWall = m_lastWall; }
        if (!m_decided && tMs >= m_decisionMs)
        {
            m_decided = true;
            if (!m_finish && !m_skipped)   // スキップ済みなら持ち替えない（ロックのフレームから巻き戻らない）
            {
                m_handed = true;
                m_slow.StartHeld(tMs, m_ev.clickDeg[CalibrateEvents::kClicks - 2]);
            }
        }
        if (m_handed) m_slow.Update(tMs, loadProgress01, finishRequested);
    }

    // スキップ。持ち替え前は予約を出さない（速い経路の音は graphite_1.wav で、止める・頭出しするのは BD5。LockTail の予約は
    // GaugeSlowDriver に持ち替えた後だけ出る）。BD5 は HandedOver() / LockWallMs() で分岐すること。
    void Skip(double tMs)
    {
        if (m_handed) { m_slow.Skip(tMs); return; }
        if (m_skipped) return;
        tMs = std::max(tMs, m_lastWall);               // 契約: スキップ時刻 >= 直近の Update 時刻
        if (m_finish && tMs >= m_ev.lockMs) return;   // ロック以降は何もしない
        m_skipped = true;
        m_skipWall = tMs;
    }

    GaugeFrame Frame(double tMs) const
    {
        if (m_handed) return m_slow.Frame(tMs);
        if (m_skipped && tMs >= m_skipWall) return EvaluateAfterLock(tMs - m_skipWall, m_ev, SkipExitSinceLock());
        return EvaluateGauge(GaugePath::Fast, tMs, 0.f, m_ev);
    }

    // 持ち替えた後の予約（持ち替える前は空。速い経路の音は graphite_1.wav）
    std::vector<GaugeCue> TakeCues() { return m_slow.TakeCues(); }
    // スキップで取り消す予約の id（GaugeSlowDriver と同じ。持ち替える前は常に空）
    std::vector<std::uint32_t> TakeCancelledCueIds() { return m_slow.TakeCancelledCueIds(); }

    // 退場が実際に始まる時刻（GaugeSlowDriver::ExitStartWallMs と同じ意味）。完了要求の前は NaN
    double ExitStartWallMs() const
    {
        if (m_handed) return m_slow.ExitStartWallMs();
        if (m_skipped) return m_finish ? m_skipWall + SkipExitSinceLock() : std::nan("");
        return m_finish ? m_ev.pingMs : std::nan("");
    }
    bool FinishObserved() const { return m_finish; }

    bool HandedOver() const { return m_handed; }   // 345 度で待つ縮退に入った
    double HandOverMs() const { return m_decisionMs; }
    // ロックの絵が出る時刻。完了要求（または縮退）の前は NaN
    double LockWallMs() const
    {
        if (m_handed) return m_slow.LockWallMs();
        if (m_skipped) return m_skipWall;
        return m_finish ? m_ev.lockMs : std::nan("");
    }

private:
    double SkipExitSinceLock() const
    {
        return GaugeExitStartMs(m_ev.pingMs - m_ev.lockMs, m_finish ? m_finishWall - m_skipWall : std::nan(""));
    }

    CalibrateEvents m_ev;
    GaugeSlowDriver m_slow;
    double m_decisionMs;
    bool m_finish = false, m_decided = false, m_handed = false, m_skipped = false;
    double m_skipWall = 0;
    double m_lastWall = 0, m_finishWall = 0;
};

} // namespace dx12e::splash
