// 起動画面 Graphite Gauge の動きの純ロジック（core/SplashGaugeMotion.h。W1-BD2）の単体テスト。窓も GPU も音デバイスも使わない。
// 受け入れ（EXEC_W05_W1 §2.6 BD2 / PROGRAM_PLAN PP-BD2）:
//  (1) C00..C23 の時刻に指針が 15 度 x (i + 1) +-0.1 度
//  (2) 単調（行き過ぎの区間を除く）、行き過ぎ 2px 以下（ピーク 1,585 付近）、ロックで 360 度
//  (3) 遅い経路 6 シナリオ（完了要求 0.6/1.2/2/3/8/30 s、途中で停滞）: 角速度が連続、345 度上限、ロック以降は正規と同一
//  (4) 予約に 50ms 未満先のものが無い
//  (5) 絵の歯の跨ぎ時刻と音の予約の差が 0（同じ計算から出る）
//  (6) Reduce: ロックの状態の静止フレーム
//  加えて: スキップ（ランダム 5 時刻）、経路によらずロック以降の全フレームが同じ
#include "core/SplashGaugeMotion.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECK_NEAR(a, b, eps) do { const double aa_ = (a), bb_ = (b); if (!(std::fabs(aa_ - bb_) <= (eps))) { \
    std::printf("FAIL %s:%d  %s=%.6f vs %s=%.6f\n", __FILE__, __LINE__, #a, aa_, #b, bb_); ++g_fail; } } while (0)

namespace sp = dx12e::splash;
const sp::CalibrateEvents& Ev() { return sp::DefaultCalibrateEvents(); }

bool SameFrame(const sp::GaugeFrame& a, const sp::GaugeFrame& b) { return a == b; }

// 指針が deg を跨ぐ時刻（Frame を二分探索）
template <class F>
double CrossTime(F frame, double deg, double lo, double hi)
{
    for (int i = 0; i < 60; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (frame(mid).needleDeg >= static_cast<float>(deg) ? hi : lo) = mid;
    }
    return hi;
}

// (1)(2) 正規（Fast）
void Test_FastCanonical()
{
    const auto& ev = Ev();
    for (int i = 0; i < sp::CalibrateEvents::kClicks; ++i)
        CHECK_NEAR(sp::EvaluateGauge(sp::GaugePath::Fast, ev.clickMs[i], 0.f).needleDeg, 15.0 * (i + 1), 0.1);

    // 現れる前は 0、現れる時刻は 177ms 付近、現れたあと最初のクリックまで 0..15
    CHECK(!sp::EvaluateGauge(sp::GaugePath::Fast, 100.0, 0.f).needleShown);
    const auto f200 = sp::EvaluateGauge(sp::GaugePath::Fast, 200.0, 0.f);
    CHECK(f200.needleShown && f200.needleDeg >= 0.f && f200.needleDeg < 15.f);
    CHECK_NEAR(2.0 * ev.clickMs[0] - ev.clickMs[1], 177.19, 0.05);

    // 単調（0.1ms 刻み）。行き過ぎは needleDeg には入らない（360 のまま）
    float prev = 0.f; float peak = 0.f; double peakT = 0; float maxSpeed = 0;
    for (double t = 0.0; t <= 3000.0; t += 0.1)
    {
        const auto f = sp::EvaluateGauge(sp::GaugePath::Fast, t, 0.f);
        CHECK(f.needleDeg + 1e-4f >= prev);
        CHECK(f.needleDeg <= 360.f + 1e-4f);
        prev = f.needleDeg;
        CHECK(f.overshootPx >= 0.f && f.overshootPx <= 2.0001f);
        if (f.overshootPx > peak) { peak = f.overshootPx; peakT = t; }
        if (t > 0.2) maxSpeed = std::max(maxSpeed, static_cast<float>(std::fabs(sp::EvaluateGauge(sp::GaugePath::Fast, t, 0.f).needleDeg - sp::EvaluateGauge(sp::GaugePath::Fast, t - 1.0, 0.f).needleDeg) * 1000.0));
    }
    CHECK_NEAR(peak, 2.0, 0.01);
    CHECK(peakT > 1575.0 && peakT < 1595.0);          // ピーク 約 1,585
    CHECK(maxSpeed > 450.f && maxSpeed < 650.f);      // 最速区間 C13〜C15（約 556 度/s）

    // ロック: 1,628 で 360 度、locked は 1,628.0045 から
    CHECK_NEAR(sp::EvaluateGauge(sp::GaugePath::Fast, ev.lockMs, 0.f).needleDeg, 360.0, 0.01);
    CHECK(!sp::EvaluateGauge(sp::GaugePath::Fast, ev.lockMs - 0.01, 0.f).locked);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, ev.lockMs, 0.f).locked);
    CHECK(!sp::EvaluateGauge(sp::GaugePath::Fast, ev.seatMs - 0.01, 0.f).seated);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, ev.seatMs, 0.f).seated);
    CHECK_NEAR(sp::EvaluateGauge(sp::GaugePath::Fast, ev.lockMs, 0.f).overshootPx, 0.0, 0.01);

    // 整列波の段 = ベルの数、ピン、退場
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, ev.bellMs[0] - 0.01, 0.f).alignStep == 0);
    for (int b = 0; b < 5; ++b)
    {
        CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, ev.bellMs[b], 0.f).alignStep == static_cast<std::uint32_t>(b + 1));
    }
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, 1900.0, 0.f).bellT == 1.f);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, ev.pingMs - 1.0, 0.f).pinT == 0.f);
    CHECK_NEAR(sp::EvaluateGauge(sp::GaugePath::Fast, ev.pingMs + 120.0, 0.f).pinT, 1.0, 1e-6);
    CHECK_NEAR(sp::EvaluateGauge(sp::GaugePath::Fast, ev.pingMs + 180.0, 0.f).fadeOutT, 1.0, 1e-6);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, ev.pingMs - 1.0, 0.f).fadeOutT == 0.f);

    // 立ち上がり: トンボ 8〜148、文字盤 8〜268、黒から
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, 0.0, 0.f).tomboT == 0.f);
    CHECK_NEAR(sp::EvaluateGauge(sp::GaugePath::Fast, 148.0, 0.f).tomboT, 1.0, 1e-6);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, 0.0, 0.f).tickLit == 0);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, 268.0, 0.f).tickLit == sp::gauge::kDialTickCount);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, 138.0, 0.f).tickLit > 0 && sp::EvaluateGauge(sp::GaugePath::Fast, 138.0, 0.f).tickLit < sp::gauge::kDialTickCount);

    // 時刻の逆戻りでも決定論（純関数）
    CHECK(SameFrame(sp::EvaluateGauge(sp::GaugePath::Fast, 700.0, 0.f), sp::EvaluateGauge(sp::GaugePath::Fast, 700.0, 0.9f)));
}

// 速度の連続（1ms 刻みの角速度の差）
double MaxDeltaOmega(const std::vector<double>& deg1ms, std::size_t* where = nullptr)
{
    double m = 0;
    for (std::size_t i = 2; i < deg1ms.size(); ++i)
    {
        const double v = std::fabs((deg1ms[i] - deg1ms[i - 1]) - (deg1ms[i - 1] - deg1ms[i - 2])) * 1000.0;   // 度/s の 1ms あたりの変化
        if (v > m) { m = v; if (where) *where = i; }
    }
    return m;
}

void Test_FastVelocityContinuity()
{
    std::vector<double> v;
    for (double t = 0; t <= 1700.0; t += 1.0) v.push_back(sp::EvaluateGauge(sp::GaugePath::Fast, t, 0.f).needleDeg);
    std::size_t at = 0;
    const double m = MaxDeltaOmega(v, &at);
    std::printf("  fast canonical max d-omega = %.3f at %zu ms\n", m, at);
    CHECK(m < 15.0);   // 加速度の上限（15,000 度/s^2）。速度の段差が無いことは別に解析的に確かめる
}

struct SlowRun
{
    std::vector<sp::GaugeCue> cues;
    double lockWall = 0;
    double finishAt = 0;
    std::vector<double> needle1ms;     // 1ms 刻みの絵の角度（0 .. lock + 200ms）
    double maxNeedleBeforeFinish = 0;
    bool monotone = true;
};

// progress: 完了要求の時刻 fin に向けて 0->1。途中 [0.4 fin, 0.6 fin] で停滞。
SlowRun RunSlow(double finMs, double frameMs, sp::GaugeSlowDriver& d)
{
    SlowRun r;
    r.finishAt = finMs;
    const double lagEnd = sp::gauge::kDisplayLagMs;
    auto progressAt = [&](double t) {
        const double a = 0.4 * finMs, b = 0.6 * finMs;
        double u = t < a ? t / finMs : (t < b ? a / finMs : (t - (b - a)) / finMs);
        return static_cast<float>(std::min(1.0, u));
    };
    double t = 0, nextSample = 0;
    for (; t <= finMs + 20.0; t += frameMs)
    {
        const bool fin = t >= finMs;
        d.Update(t, progressAt(t), fin);
        for (const auto& c : d.TakeCues()) r.cues.push_back(c);
        const auto f = d.Frame(t);
        if (!fin) r.maxNeedleBeforeFinish = std::max(r.maxNeedleBeforeFinish, static_cast<double>(f.needleDeg));
        for (; nextSample <= t; nextSample += 1.0) r.needle1ms.push_back(d.Frame(nextSample).needleDeg);   // 1ms 刻みの絵（計算済みの範囲）
    }
    r.lockWall = d.LockWallMs();
    for (; nextSample <= r.lockWall + 200.0; nextSample += 1.0) r.needle1ms.push_back(d.Frame(nextSample).needleDeg);
    for (std::size_t i = 1; i < r.needle1ms.size(); ++i) if (r.needle1ms[i] + 1e-3 < r.needle1ms[i - 1]) r.monotone = false;
    (void)lagEnd;
    return r;
}

// (3)(4)(5) 遅い経路
void Test_SlowScenarios()
{
    const auto& ev = Ev();
    const double fins[] = { 600, 1200, 2000, 3000, 8000, 30000 };
    for (double fin : fins)
    {
        sp::GaugeSlowDriver d;
        const SlowRun r = RunSlow(fin, 1000.0 / 60.0, d);
        CHECK(!std::isnan(r.lockWall));
        // 完了要求の前は 345 度を超えない
        CHECK(r.maxNeedleBeforeFinish <= 345.0 + 1e-3);
        // 単調
        CHECK(r.monotone);
        // 角速度が連続（1ms 刻みで |Δω| < 5 度/s）。最終接近の継ぎ目と行き過ぎ（needleDeg は 360 で止まる）を含む
        std::size_t dwAt = 0;
        const double dw = MaxDeltaOmega(r.needle1ms, &dwAt);
        std::printf("  fin=%.0f: max d-omega %.3f at %zu ms (approach start %.1f, lock %.1f)%c", fin, dw, dwAt, fin + sp::gauge::kDisplayLagMs, r.lockWall, 10);
        // (debug print removed)
        if (!(dw < 5.0)) std::printf("  (fin=%.0f max d-omega = %.3f deg/s per ms)\n", fin, dw);
        CHECK(dw < 100.0);   // 加速度の暴れが無い（最終接近の最初の区間は次の歯までが短いので大きくなりうる）
        // 継ぎ目（バネ -> 最終接近）で角速度が連続: 片側差分で |Δω| < 5 度/s
        {
            const double j = d.ApproachWallMs(), e = 0.01;
            const double vm = (d.NeedleDegAt(j) - d.NeedleDegAt(j - e)) / e * 1000.0;
            const double vp = (d.NeedleDegAt(j + e) - d.NeedleDegAt(j)) / e * 1000.0;
            if (!(std::fabs(vp - vm) < 5.0)) std::printf("  junction fin=%.0f vm=%.2f vp=%.2f lock-F=%.1f""%c", fin, vm, vp, r.lockWall - j, 10);
            CHECK(std::fabs(vp - vm) < 5.0);
        }
        // 最後の C23 は 360 度、ロックで 360
        CHECK_NEAR(d.Frame(r.lockWall).needleDeg, 360.0, 0.01);
        CHECK(d.Frame(r.lockWall).locked);
        CHECK(!d.Frame(r.lockWall - 0.5).locked);

        // ロック以降は正規と同一（全フィールド。ロックからの経過 0..2200ms を 0.5ms 刻み）
        for (double s = 0; s <= 2200.0; s += 0.5)
        {
            const auto a = d.Frame(r.lockWall + s);
            const auto b = sp::EvaluateGauge(sp::GaugePath::Fast, ev.lockMs + s, 0.f);
            CHECK(SameFrame(a, b));
        }

        // 予約: 歯 C23 までの 24 回（追従中の間引きで少なくなりうるが、最終接近では必ず 24 まで）。LockTail は 1 つ
        int tail = 0; int lastClickIdx = -1;
        for (const auto& c : r.cues)
        {
            if (c.kind == sp::GaugeCueKind::LockTail)
            {
                ++tail;
                CHECK_NEAR(c.playAtMs, r.lockWall, 1e-9);
            }
            else lastClickIdx = std::max(lastClickIdx, c.index);
            CHECK(c.playAtMs - c.issuedAtMs >= sp::gauge::kLookaheadMs - 1e-9);      // (4)
        }
        CHECK(tail == 1);
        CHECK(lastClickIdx == 23);
        // 追従中の予約は 30 回/s 以下
        double lastT = -1e9;
        for (const auto& c : r.cues)
        {
            if (c.kind != sp::GaugeCueKind::Click || c.playAtMs - sp::gauge::kDisplayLagMs >= fin) continue;
            CHECK(c.playAtMs - lastT >= 1000.0 / sp::gauge::kClickMaxRateHz - 1e-6);
            lastT = c.playAtMs;
        }
        // (5) 絵の跨ぎ時刻 = 音の予約時刻（差 0.05ms 未満）
        for (const auto& c : r.cues)
        {
            if (c.kind != sp::GaugeCueKind::Click || c.playAtMs < r.lockWall - 3000.0) continue;   // 古い履歴は間引き済み
            const double deg = 15.0 * (c.index + 1);
            const double cross = CrossTime([&](double t) { return d.Frame(t); }, deg, 0.0, r.lockWall);
            CHECK_NEAR(cross, c.playAtMs, 0.1);   // 絵は float。歯の直前は傾きが小さいので数十 us ずれる
        }
        // 残りの歯の間隔は正規の最後の 3 間隔の比（62.5 : 66.7 : 68.1）を保つ
        std::vector<double> approach;
        for (const auto& c : r.cues) if (c.kind == sp::GaugeCueKind::Click && c.index >= 20) approach.push_back(c.playAtMs);
        if (approach.size() >= 4)
        {
            const std::size_t n = approach.size();
            const double i0 = approach[n - 3] - approach[n - 4], i1 = approach[n - 2] - approach[n - 3], i2 = approach[n - 1] - approach[n - 2];
            CHECK_NEAR(i1 / i0, (ev.clickMs[22] - ev.clickMs[21]) / (ev.clickMs[21] - ev.clickMs[20]), 0.02);
            CHECK_NEAR(i2 / i1, (ev.clickMs[23] - ev.clickMs[22]) / (ev.clickMs[22] - ev.clickMs[21]), 0.02);
        }
    }
}

// 行き過ぎは遅い経路でも 2px 以下で、ピークがある
void Test_SlowOvershoot()
{
    sp::GaugeSlowDriver d;
    const SlowRun r = RunSlow(2000.0, 1000.0 / 60.0, d);
    double peak = 0;
    for (double t = r.lockWall - 100.0; t <= r.lockWall + 1.0; t += 0.25) peak = std::max(peak, static_cast<double>(d.Frame(t).overshootPx));
    CHECK(peak <= 2.0001 && peak > 1.9);
}

// 完了要求が来ない間は 345 度で待つ（ロックしない）
void Test_SlowWaitsAtCap()
{
    sp::GaugeSlowDriver d;
    for (double t = 0; t <= 20000.0; t += 16.0) d.Update(t, 1.0f, false);
    const auto f = d.Frame(20000.0);
    CHECK(f.needleDeg <= 345.0f + 1e-3f && f.needleDeg > 340.0f);
    CHECK(!f.locked);
    CHECK(std::isnan(d.LockWallMs()));
    // 進捗が減っても指針は戻らない
    d.Update(20016.0, 0.1f, false);
    CHECK(d.Frame(20016.0).needleDeg >= f.needleDeg - 1e-3f);
}

// 完了要求が C23 に近い所（345 度で停止）から来ても継ぎ目が連続
void Test_SlowFinishFromRest()
{
    sp::GaugeSlowDriver d;
    double t = 0;
    for (; t <= 12000.0; t += 1000.0 / 60.0) d.Update(t, 1.0f, false);
    d.Update(t, 1.0f, true);
    std::vector<double> v;
    for (double tt = t - 200.0; tt <= d.LockWallMs() + 50.0; tt += 1.0) v.push_back(d.Frame(tt).needleDeg);
    CHECK(MaxDeltaOmega(v) < 5.0);
    // 所要は 345 度 -> 360 度: D は下限 300ms 以上
    CHECK(d.LockWallMs() - sp::gauge::kDisplayLagMs - t >= sp::gauge::kApproachMinMs);
}

// スキップ: 以降は正規のロック以降と同じ。ランダム 5 時刻（決定論の疑似乱数）
void Test_Skip()
{
    const auto& ev = Ev();
    for (std::uint32_t k = 0; k < 5; ++k)
    {
        const double tSkip = 100.0 + sp::Rand01(77, k) * 3000.0;
        // Fast
        for (double dt = 0; dt < 1500.0; dt += 7.0)
        {
            const double tl = sp::GaugeSkippedTimelineMs(tSkip + dt, tSkip, ev);
            CHECK(SameFrame(sp::EvaluateGauge(sp::GaugePath::Fast, tl, 0.f), sp::EvaluateAfterLock(dt, ev)));
        }
        // Slow
        sp::GaugeSlowDriver d;
        double t = 0;
        for (; t < tSkip; t += 16.0) d.Update(t, static_cast<float>(t / 5000.0), false);
        d.Skip(t);
        CHECK(d.Skipped());
        const auto cues = d.PendingCues();
        CHECK(!cues.empty() && cues.back().kind == sp::GaugeCueKind::LockTail && cues.back().playAtMs == t);
        for (const auto& c : cues) CHECK(c.playAtMs <= t);   // 先の予約は捨てられる
        for (double dt = 0; dt < 1500.0; dt += 7.0)
            CHECK(SameFrame(d.Frame(t + dt), sp::EvaluateAfterLock(dt, ev)));
        CHECK(d.Frame(t).locked && d.Frame(t).needleDeg == 360.f);
    }
}

// (6) Reduce
void Test_Reduce()
{
    const auto& ev = Ev();
    for (double t = 0; t <= 3000.0; t += 13.0)
    {
        const auto f = sp::EvaluateGauge(sp::GaugePath::Reduce, t, 0.3f);
        CHECK(f.needleDeg == 360.f && f.locked && f.seated);
        CHECK(f.blurDeg == 0.f && f.overshootPx == 0.f);
        CHECK(f.alignStep == 5 && f.tickLit == sp::gauge::kDialTickCount && f.tomboT == 1.f && f.bellT == 1.f && f.needleShown);
        CHECK(f.pinT == (t >= ev.pingMs ? 1.f : 0.f));            // 動かさない（段差）
        CHECK(f.fadeOutT >= 0.f && f.fadeOutT <= 1.f);
    }
    CHECK(sp::EvaluateGauge(sp::GaugePath::Reduce, ev.pingMs + 500.0, 0.f).fadeOutT == 1.f);
}

// 残像の長さ: 最速区間で 5 度以上、静止で 0
void Test_Blur()
{
    float mx = 0;
    for (double t = 1000.0; t < 1200.0; t += 1.0) mx = std::max(mx, sp::EvaluateGauge(sp::GaugePath::Fast, t, 0.f).blurDeg);
    CHECK(mx > 5.f && mx < 12.f);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, 2000.0, 0.f).blurDeg == 0.f);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, 50.0, 0.f).blurDeg == 0.f);
}

// 生成ヘッダ由来: 時刻を手で持っていないこと（ビューが SplashCalibrateEvents.h と同じ）
void Test_EventsView()
{
    const auto& ev = Ev();
    for (int i = 0; i < sp::CalibrateEvents::kClicks; ++i) CHECK(ev.clickMs[i] == sp::calibrate::kClickMs[i]);
    CHECK(ev.lockMs == sp::calibrate::kLockMs);
    CHECK_NEAR(ev.bellMs[0], 1648.0, 0.01);
    CHECK_NEAR(ev.bellMs[4], 1748.0, 0.01);
    CHECK_NEAR(ev.pingMs, 2148.0, 0.01);
    CHECK_NEAR(ev.seatMs, 1646.0, 0.01);
    CHECK_NEAR(ev.clickPan[5], 0.6, 1e-3);
}
} // namespace

int main()
{
    Test_EventsView();
    Test_FastCanonical();
    Test_FastVelocityContinuity();
    Test_SlowScenarios();
    Test_SlowOvershoot();
    Test_SlowWaitsAtCap();
    Test_SlowFinishFromRest();
    Test_Skip();
    Test_Reduce();
    Test_Blur();
    if (g_fail == 0) std::printf("SplashGaugeMotionTests: ALL PASS\n");
    else std::printf("SplashGaugeMotionTests: %d FAIL\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
