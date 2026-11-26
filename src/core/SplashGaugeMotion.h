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
//   Reduce … Reduce Motion。ロックの状態から静的に始める（回転と残像なし）。ピンと退場の時計だけ進む。
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
    float overshootPx = 0.f;        // 行き過ぎ 0..2px（指針の先端の外向き。C23 から ロックまでの 85ms）
    std::uint32_t alignStep = 0;    // 整列波の段 0..5（ベルの各音に 1 段ずつ）
    // --- 描画側が欲しい追加分（BD2 で足したもの） ---
    bool needleShown = false;       // 指針が現れた（177ms。0° に静止したまま）
    bool seated = false;            // 着座（1,646。表題欄の状態が「校正中」→「校正済」）
    float tomboT = 0.f;             // 四隅のトンボの進み 0..1（8〜148、linear）
    float fadeOutT = 0.f;           // 窓の退場 0..1（2,148〜2,328 の α フェード。alpha = 1 - fadeOutT）
    float timelineMs = 0.f;         // この絵の正規タイムライン上の時刻（Slow では実時間と一致しない）

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
inline constexpr double kTomboStartMs = 8.0;            // 四隅のトンボ（GG の tomboDraw 140ms）
inline constexpr double kTomboDurMs = 140.0;
inline constexpr double kDialStartMs = 8.0;             // 文字盤の目盛りを刻む区間（linear）
inline constexpr double kDialEndMs = 268.0;
inline constexpr double kBezelSweepMs = 200.0;          // ベゼルの鏡面スイープ（最初のベルから）
inline constexpr double kPinStampMs = 120.0;            // ピンの刻印が出きるまで
inline constexpr double kFadeOutMs = 180.0;             // 窓の α フェード（ピンから）
inline constexpr double kOvershootPx = 2.0;             // 行き過ぎの最大（2px）
inline constexpr double kNeedleRadiusPx = 248.0;        // 指針の半径（行き過ぎの px ↔ 度の換算）
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

// ロックからの経過 ms（>= 0）に対する絵。Fast / Slow / スキップがすべてこれを通る（ロック以降はビット一致）。
inline GaugeFrame EvaluateAfterLock(double sinceLockMs, const CalibrateEvents& ev = DefaultCalibrateEvents())
{
    return gauge_detail::CanonicalFrame(ev, gauge_detail::MakeCanonicalNeedle(ev), ev.lockMs + std::max(0.0, sinceLockMs));
}

// スキップ（キー・クリック）: 絵はその時点でロックのフレームへ飛ぶ。以降の正規タイムライン上の時刻を返す。
inline double GaugeSkippedTimelineMs(double tNowMs, double tSkipMs, const CalibrateEvents& ev = DefaultCalibrateEvents())
{
    return ev.lockMs + std::max(0.0, tNowMs - tSkipMs);
}

// 状態を持たない評価。Fast: 正規のタイムライン。Reduce: ロックの状態の静止フレーム（ピン・退場だけ時計で進む）。
// Slow: 状態が要るので GaugeSlowDriver を使うこと。ここでは「その時刻・進捗を保持した場合の目標の姿」（バネ前の素の値）を返す。
inline GaugeFrame EvaluateGauge(GaugePath path, double tMs, float loadProgress01, const CalibrateEvents& ev = DefaultCalibrateEvents())
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
        const double t = std::max(0.0, tMs);
        f.timelineMs = static_cast<float>(t);
        f.pinT = t >= ev.pingMs ? 1.f : 0.f;                       // 動かさない: 刻印は出るだけ
        f.fadeOutT = static_cast<float>(Clamp01((t - ev.pingMs) / gauge::kFadeOutMs));
        return f;
    }
    case GaugePath::Slow:
    default:
    {
        GaugeFrame f = gauge_detail::CanonicalFrame(ev, needle, std::min(std::max(0.0, tMs), ev.lockMs - 1e-6));
        const double target = std::min(gauge::kSlowCapDeg, 360.0 * std::min<double>(Clamp01(loadProgress01), gauge::kSlowProgressCap));
        f.needleDeg = static_cast<float>(target);
        f.blurDeg = 0.f; f.overshootPx = 0.f;
        return f;
    }
    }
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
};

// 使い方: 起動時に生成し、フレームごとに Update(t, 実進捗, 完了要求) → Frame(t) と TakeCues()。t は生成からの ms。
//   ・計算（バネ・歯の検出・予約）は t で進め、絵は kDisplayLagMs 遅れた時刻の状態を出す。こうすると、
//     歯を跨ぐ瞬間の絵と音が同じ時刻に揃い、予約は 50ms 以上先になる。
//   ・完了要求が来たら残りの歯を Calibrate の減速で送り、最後の歯 + 85ms にロックする。以後は EvaluateAfterLock と同じ。
//   ・指針の上限は 345°（最後の 1 歯はロックのために取っておく）。進捗は単調（減ったら無視）。
class GaugeSlowDriver
{
public:
    explicit GaugeSlowDriver(const CalibrateEvents& ev = DefaultCalibrateEvents())
        : m_ev(ev), m_canon(gauge_detail::MakeCanonicalNeedle(ev)) {}

    void Update(double tMs, float loadProgress01, bool finishRequested)
    {
        if (m_skipped) return;
        tMs = std::max(tMs, m_lastT);
        m_progress = std::max(m_progress, static_cast<double>(Clamp01(loadProgress01)));
        if (!m_approach)
        {
            Follow(tMs);
            if (finishRequested) BeginApproach(tMs);
        }
        m_nowMs = tMs;
        Trim(tMs);
    }

    // スキップ: 絵は tMs にロックのフレームへ飛ぶ。tMs より先の未取得の予約は捨て、LockTail を tMs に即時で入れる
    //（鳴っているボイスの 30ms フェードアウトと lock_tail の頭出しは音の側）。
    void Skip(double tMs)
    {
        if (m_skipped) return;
        m_skipped = true;
        m_skipWall = tMs;
        std::erase_if(m_cues, [&](const GaugeCue& c) { return c.playAtMs > tMs; });
        GaugeCue c;
        c.playAtMs = tMs; c.issuedAtMs = tMs; c.kind = GaugeCueKind::LockTail;
        m_cues.push_back(c);
    }

    // たまった予約を取り出す（以後は空）
    std::vector<GaugeCue> TakeCues() { std::vector<GaugeCue> out; out.swap(m_cues); return out; }
    const std::vector<GaugeCue>& PendingCues() const { return m_cues; }

    bool Skipped() const { return m_skipped; }
    bool Approaching() const { return m_approach; }
    // 最終接近が絵に現れる時刻（Update の時計）。完了要求の前は NaN
    double ApproachWallMs() const { return m_approach ? m_fApp + gauge::kDisplayLagMs : std::nan(""); }
    // ロックの絵が出る時刻（Update の時計）。完了要求の前は NaN
    double LockWallMs() const
    {
        if (m_skipped) return m_skipWall;
        return m_approach ? m_lockSim + gauge::kDisplayLagMs : std::nan("");
    }

    // 指針の角度（倍精度。絵の時計 = tMs - kDisplayLagMs の時点。行き過ぎは含まない）。解析・テスト用
    double NeedleDegAt(double tMs) const { return NeedleAt(tMs - gauge::kDisplayLagMs); }

    GaugeFrame Frame(double tMs) const
    {
        if (m_skipped && tMs >= m_skipWall) return EvaluateAfterLock(tMs - m_skipWall, m_ev);
        const double d = tMs - gauge::kDisplayLagMs;
        if (m_approach && d >= m_lockSim) return EvaluateAfterLock(d - m_lockSim, m_ev);

        GaugeFrame f;
        gauge_detail::FillItems(m_ev, std::min(std::max(d, 0.0), m_ev.lockMs - 1e-6), f);
        f.timelineMs = static_cast<float>(std::max(d, 0.0));
        const double th = NeedleAt(d);
        f.needleDeg = static_cast<float>(th);
        f.blurDeg = static_cast<float>(std::max(0.0, th - NeedleAt(d - gauge::kBlurWindowMs)));
        if (m_approach && d >= m_toothLastMs)
            f.overshootPx = static_cast<float>(gauge_detail::OvershootPxAt(d - m_toothLastMs, gauge_detail::OvershootSpanMs(m_ev)));
        return f;
    }

private:
    struct Seg { double t0, t1, target, a, b; };   // x(u) = (a + b u) e^{-w u} + target

    double SegTheta(const Seg& s, double t) const
    {
        const double u = (t - s.t0) * 1e-3;
        return std::min(gauge::kSlowCapDeg, std::max(0.0, s.target + (s.a + s.b * u) * std::exp(-gauge::kSlowOmega * u)));
    }

    void Follow(double tMs)
    {
        const double dt = tMs - m_lastT;
        if (dt <= 0.0) return;
        const double target = std::min(gauge::kSlowCapDeg, 360.0 * std::min(m_progress, gauge::kSlowProgressCap));
        const double w = gauge::kSlowOmega;
        Seg s;
        s.t0 = m_lastT; s.t1 = tMs; s.target = target;
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
                PushClick(m_nextTooth, hi, tMs);
            }
            ++m_nextTooth;
        }
        m_segs.push_back(s);
        m_theta = newTheta;
        m_omega = newOmega;
        m_lastT = tMs;
    }

    void PushClick(int tooth, double simMs, double issuedMs)
    {
        GaugeCue c;
        c.playAtMs = simMs + gauge::kDisplayLagMs;
        c.issuedAtMs = issuedMs;
        c.kind = GaugeCueKind::Click;
        c.index = tooth;
        c.pan = static_cast<float>(m_ev.clickPan[tooth]);
        m_cues.push_back(c);
    }

    // 完了要求: 現在の (θ, ω) から残りの歯を Calibrate の減速で送る。
    // 結び目 = (F, θ0) と、残りの歯（時刻は正規の間隔を所要 D に伸縮）。始点の傾きは今の角速度（C1）、終点の傾きは 0。
    void BeginApproach(double fMs)
    {
        m_approach = true;
        m_fApp = fMs;
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
        for (int j = m; j <= last; ++j) PushClick(j, m_spline.t[static_cast<std::size_t>(1 + j - m)], fMs);
        GaugeCue lock;
        lock.playAtMs = m_lockSim + gauge::kDisplayLagMs;
        lock.issuedAtMs = fMs;
        lock.kind = GaugeCueKind::LockTail;
        m_cues.push_back(lock);
    }

    double NeedleAt(double d) const
    {
        if (d <= 0.0) return 0.0;
        if (m_approach && d >= m_fApp) return m_spline.Eval(d);
        // 計算の履歴から（d は直近の kDisplayLagMs 以内が普通。古い d は先頭の区間に丸める）
        if (m_segs.empty()) return 0.0;
        auto it = std::lower_bound(m_segs.begin(), m_segs.end(), d, [](const Seg& s, double x) { return s.t1 < x; });
        if (it == m_segs.end()) return m_theta;
        if (d < it->t0) return SegTheta(*it, it->t0);
        return SegTheta(*it, d);
    }

    void Trim(double tMs)
    {
        const double keepFrom = tMs - gauge::kDisplayLagMs - 5000.0;
        std::size_t drop = 0;
        while (drop + 1 < m_segs.size() && m_segs[drop].t1 < keepFrom) ++drop;
        if (drop) m_segs.erase(m_segs.begin(), m_segs.begin() + static_cast<std::ptrdiff_t>(drop));
    }

    CalibrateEvents m_ev;
    HermiteSpline m_canon;                  // 予備（正規の形を参照したいとき）
    std::vector<Seg> m_segs;
    std::vector<GaugeCue> m_cues;
    HermiteSpline m_spline;                 // 最終接近
    double m_lastT = 0, m_nowMs = 0;
    double m_theta = 0, m_omega = 0;        // 度、度/s（計算の時計）
    double m_progress = 0;
    int m_nextTooth = 0;
    double m_lastCueSim = -1e9;
    bool m_approach = false;
    double m_fApp = 0, m_toothLastMs = 0, m_lockSim = 0;
    bool m_skipped = false;
    double m_skipWall = 0;
};

} // namespace dx12e::splash
