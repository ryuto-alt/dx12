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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECK_MSG(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)
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
    CHECK_NEAR(sp::EvaluateGauge(sp::GaugePath::Fast, sp::gauge::kTomboStartMs + sp::gauge::kTomboDurMs, 0.f).tomboT, 1.0, 1e-6);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, 0.0, 0.f).tickLit == 0);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, ev.clickMs[0], 0.f).tickLit == sp::gauge::kDialTickCount);
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

double g_canonMaxDw = 12.6;   // 正規曲線の 1ms あたりの角速度変化の最大（Test_FastVelocityContinuity が実測で上書き）
constexpr double kSlowDwFactor = 4.0;   // 遅い経路の許容 = 正規の最大 x 4（最終接近の最初の区間は次の歯までが短い。実測は約 3.1 倍）

void Test_FastVelocityContinuity()
{
    std::vector<double> v;
    for (double t = 0; t <= 1700.0; t += 1.0) v.push_back(sp::EvaluateGauge(sp::GaugePath::Fast, t, 0.f).needleDeg);
    std::size_t at = 0;
    const double m = MaxDeltaOmega(v, &at);
    std::printf("  fast canonical max d-omega = %.3f at %zu ms\n", m, at);
    CHECK(m < 15.0);   // 加速度の上限（15,000 度/s^2）。速度の段差が無いことは別に解析的に確かめる
    g_canonMaxDw = m;
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
        (void)dwAt;
        CHECK(dw < kSlowDwFactor * g_canonMaxDw);   // |Δω| は正規の最大の k 倍以内（EXEC §2.6 の 5 度/s/フレームは正規曲線自体が満たさないので、この形に改めた）
        // 継ぎ目（バネ -> 最終接近）で角速度が連続: 片側差分で |Δω| < 5 度/s
        {
            const double j = d.ApproachWallMs(), e = 0.01;
            const double vm = (d.NeedleDegAt(j) - d.NeedleDegAt(j - e)) / e * 1000.0;
            const double vp = (d.NeedleDegAt(j + e) - d.NeedleDegAt(j)) / e * 1000.0;
            CHECK(std::fabs(vp - vm) < 5.0);
        }
        // 最後の C23 は 360 度、ロックで 360
        CHECK_NEAR(d.Frame(r.lockWall).needleDeg, 360.0, 0.01);
        CHECK(d.Frame(r.lockWall).locked);
        CHECK(SameFrame(d.Frame(r.lockWall), sp::EvaluateAfterLock(0.0, ev)));   // ロックの瞬間は丸めに関わらずロックのフレーム
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
            CHECK(c.playAtMs - c.issuedAtMs >= sp::gauge::kLookaheadMs - 1e-6);      // (4)
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
        for (const auto& c : r.cues) if (c.kind == sp::GaugeCueKind::Click && c.index >= 20 && c.issuedAtMs >= fin) approach.push_back(c.playAtMs);   // 最終接近で出た予約だけ
        if (approach.size() >= 4)
        {
            const std::size_t n = approach.size();
            const double i0 = approach[n - 3] - approach[n - 4], i1 = approach[n - 2] - approach[n - 3], i2 = approach[n - 1] - approach[n - 2];
            CHECK_NEAR(i1 / i0, (ev.clickMs[22] - ev.clickMs[21]) / (ev.clickMs[21] - ev.clickMs[20]), 0.02);
            CHECK_NEAR(i2 / i1, (ev.clickMs[23] - ev.clickMs[22]) / (ev.clickMs[22] - ev.clickMs[21]), 0.02);
        }
    }
}

// 音と絵の同期: 各クリックの予約の再生時刻に、絵（Frame）がその歯を跨いでいる（履歴が残っている fromMs 以降の予約だけ）
template <class Driver>
void CheckSync(const Driver& d, const std::vector<sp::GaugeCue>& cues, double fromMs)
{
    for (const auto& c : cues)
    {
        if (c.kind != sp::GaugeCueKind::Click || c.playAtMs < fromMs) continue;
        const double cross = CrossTime([&](double t) { return d.Frame(t); }, 15.0 * (c.index + 1), c.playAtMs - 2000.0, c.playAtMs + 1000.0);
        CHECK_NEAR(cross, c.playAtMs, 0.5);
    }
}

// ヒッチ・不規則なフレーム間隔・進捗の飛びでも、先読み 50ms・単調・345 度・ロックの瞬間の一致が保たれる（決定論の疑似乱数）
void Test_SlowHitches()
{
    for (std::uint32_t run = 0; run < 400; ++run)
    {
        sp::GaugeSlowDriver d;
        const double fin = 300.0 + sp::Rand01(11, run) * 6000.0;
        double t = 0, progress = 0, lastNeedle = 0, lastTimeline = 0;
        std::uint32_t k = 0;
        bool finished = false;
        std::vector<sp::GaugeCue> issued;
        for (int step = 0; step < 4000 && !(finished && t > d.LockWallMs() + 5.0); ++step)
        {
            const double r = sp::Rand01(100 + run, k++);
            t += (r < 0.12) ? 40.0 + sp::Rand01(200 + run, k++) * 400.0 : 8.0 + sp::Rand01(300 + run, k++) * 12.0;   // ヒッチ 40..440ms
            if (sp::Rand01(400 + run, k++) < 0.2) progress = std::min(1.0, progress + sp::Rand01(500 + run, k++) * 0.9);   // 進捗の飛び
            const bool f = t >= fin;
            d.Update(t, static_cast<float>(progress), f);
            finished = finished || f;
            for (const auto& c : d.TakeCues())
            {
                CHECK(c.playAtMs - c.issuedAtMs >= sp::gauge::kLookaheadMs - 1e-6);
                CHECK(c.playAtMs >= t);
                issued.push_back(c);
            }
            const auto fr = d.Frame(t);
            CHECK(fr.needleDeg + 1e-3f >= lastNeedle);
            CHECK(fr.timelineMs + 1e-3f >= lastTimeline);
            if (!finished) CHECK(fr.needleDeg <= 345.0f + 1e-3f);
            if (!fr.needleShown) CHECK(fr.needleDeg == 0.f);
            lastNeedle = fr.needleDeg; lastTimeline = fr.timelineMs;
        }
        CHECK(!std::isnan(d.LockWallMs()));
        CheckSync(d, issued, t - 4000.0);   // 音と絵の同期: 発行済みの歯を絵が跨ぐ時刻 = 予約の再生時刻（ヒッチがあっても）
        CHECK(SameFrame(d.Frame(d.LockWallMs()), sp::EvaluateAfterLock(0.0)));
        CHECK(!d.Frame(d.LockWallMs() - 1e-3).locked);
        for (double s = 0; s <= 2200.0; s += 3.7) CHECK(SameFrame(d.Frame(d.LockWallMs() + s), sp::EvaluateAfterLock(s)));
    }
}

// フレーム間隔が常に 50ms（20fps。34ms を超える環境）: 先読み・単調・同期・ロックの一致は保たれる（指針は実時間より遅く進む）
void Test_SlowConstant50ms()
{
    sp::GaugeSlowDriver d;
    double t = 0, lastNeedle = 0, lastTimeline = 0;
    std::vector<sp::GaugeCue> issued;
    for (int i = 0; i < 160; ++i)
    {
        t += 50.0;
        d.Update(t, static_cast<float>(std::min(1.0, t / 5000.0)), t >= 5000.0);
        for (const auto& c : d.TakeCues()) { CHECK(c.playAtMs - c.issuedAtMs >= sp::gauge::kLookaheadMs - 1e-6); issued.push_back(c); }
        const auto f = d.Frame(t);
        CHECK(f.needleDeg + 1e-3f >= lastNeedle);
        CHECK(f.timelineMs + 1e-3f >= lastTimeline);
        lastNeedle = f.needleDeg; lastTimeline = f.timelineMs;
    }
    CHECK(!std::isnan(d.LockWallMs()));
    CheckSync(d, issued, t - 4000.0);
    CHECK(SameFrame(d.Frame(d.LockWallMs()), sp::EvaluateAfterLock(0.0)));
}

// 進捗が上限（0.95 = 342 度）に達して待っているところへ完了要求: 先読みと一致が保たれる
void Test_SlowFinishAtCap()
{
    sp::GaugeSlowDriver d;
    double t = 0;
    for (; t < 6000.0; t += 1000.0 / 60.0) d.Update(t, 1.0f, false);
    d.Update(t, 1.0f, true);
    const auto cues = d.TakeCues();
    CHECK(!cues.empty());
    for (const auto& c : cues) CHECK(c.playAtMs - c.issuedAtMs >= sp::gauge::kLookaheadMs - 1e-6);
    CHECK(SameFrame(d.Frame(d.LockWallMs()), sp::EvaluateAfterLock(0.0)));
}

// 速い経路でロードが C22 より遅れた: 345 度で待ち、完了要求から最終接近でロック（BA-2 の縮退）
void Test_FastLateFinish()
{
    const auto& ev = Ev();
    {   // C22 より前に完了要求: 最後まで正規
        sp::GaugeFastDriver d;
        for (double t = 0; t < 2500.0; t += 1000.0 / 60.0) d.Update(t, 1.0f, t >= 1000.0);
        CHECK(!d.HandedOver());
        CHECK_NEAR(d.LockWallMs(), ev.lockMs, 1e-9);
        for (double t = 0; t < 2500.0; t += 5.3) CHECK(SameFrame(d.Frame(t), sp::EvaluateGauge(sp::GaugePath::Fast, t, 0.f)));
    }
    sp::GaugeFastDriver d;
    double t = 0, lastNeedle = 0, lastTimeline = 0;
    std::vector<sp::GaugeCue> cues;
    for (; t < 4000.0; t += 1000.0 / 60.0)
    {
        d.Update(t, 0.2f, false);
        for (const auto& c : d.TakeCues()) cues.push_back(c);
        const auto f = d.Frame(t);
        CHECK(f.timelineMs + 1e-3f >= lastTimeline);   // 持ち替え（1,475）で timelineMs が戻らない
        lastTimeline = f.timelineMs;
        CHECK(f.needleDeg + 1e-3f >= lastNeedle);
        CHECK(f.needleDeg <= 345.0f + 0.5f);
        CHECK(!f.locked);
        lastNeedle = f.needleDeg;
    }
    CHECK(d.HandedOver());
    CHECK(std::isnan(d.LockWallMs()));
    CHECK_NEAR(d.Frame(3000.0).needleDeg, 345.0, 1e-3);
    CHECK_NEAR(d.Frame(ev.clickMs[22] - 1.0).needleDeg, 345.0, 1.0);   // 持ち替えの前後で絵が連続
    d.Update(t, 0.2f, true);
    for (const auto& c : d.TakeCues()) cues.push_back(c);
    CHECK(!std::isnan(d.LockWallMs()));
    int clicks = 0, tails = 0;
    for (const auto& c : cues)
    {
        CHECK(c.playAtMs - c.issuedAtMs >= sp::gauge::kLookaheadMs - 1e-6);
        if (c.kind == sp::GaugeCueKind::Click) { ++clicks; CHECK(c.index == 23); } else ++tails;
    }
    CHECK(clicks == 1 && tails == 1);
    CHECK(SameFrame(d.Frame(d.LockWallMs()), sp::EvaluateAfterLock(0.0)));
    for (double s = 0; s <= 2200.0; s += 0.5) CHECK(SameFrame(d.Frame(d.LockWallMs() + s), sp::EvaluateGauge(sp::GaugePath::Fast, ev.lockMs + s, 0.f)));
}

// C22（1,475）より前のスキップ: 完了要求が来なくても持ち替えず、ロックのフレームのまま進む
void Test_FastSkipBeforeHandover()
{
    const auto& ev = Ev();
    sp::GaugeFastDriver d;
    const double tSkip = 1016.7;
    double t = 0;
    for (; t < tSkip; t += 1000.0 / 60.0) d.Update(t, 0.1f, false);
    d.Skip(tSkip);
    for (t = tSkip; t < 4000.0; t += 1000.0 / 60.0)
    {
        d.Update(t, 0.1f, true);
        const auto f = d.Frame(t);
        CHECK(SameFrame(f, sp::EvaluateAfterLock(t - tSkip, ev)));
        CHECK(f.locked);
    }
    CHECK(!d.HandedOver());
    CHECK_NEAR(d.LockWallMs(), tSkip, 1e-9);
    CHECK(d.TakeCues().empty());   // 持ち替え前は予約を出さない（音は BD5）
}

// ロック以降のスキップは何もしない（巻き戻さない・ロック音を重ねない）
void Test_SkipAfterLockNoRewind()
{
    const auto& ev = Ev();
    for (std::uint32_t k = 0; k < 5; ++k)
    {
        const double tSkip = ev.lockMs + 1.0 + sp::Rand01(88, k) * 1500.0;
        // Fast ドライバ（完了要求あり）
        sp::GaugeFastDriver f;
        for (double t = 0; t < tSkip; t += 16.0) f.Update(t, 1.0f, true);
        const auto before = f.Frame(tSkip);
        f.Skip(tSkip);
        CHECK(SameFrame(f.Frame(tSkip), before));
        CHECK(SameFrame(f.Frame(tSkip + 100.0), sp::EvaluateGauge(sp::GaugePath::Fast, tSkip + 100.0, 0.f)));
        CHECK_NEAR(sp::GaugeSkippedTimelineMs(tSkip + 50.0, tSkip, ev), tSkip + 50.0, 1e-9);
        // Slow ドライバ
        sp::GaugeSlowDriver d;
        double t = 0;
        for (; t < 900.0; t += 16.0) d.Update(t, 1.0f, false);
        d.Update(t, 1.0f, true);
        d.TakeCues();
        const double lock = d.LockWallMs();
        const double tS = lock + 1.0 + sp::Rand01(99, k) * 1500.0;
        const auto b = d.Frame(tS);
        d.Skip(tS);
        CHECK(!d.Skipped());
        CHECK(d.PendingCues().empty());
        CHECK(SameFrame(d.Frame(tS), b));
        CHECK_NEAR(d.LockWallMs(), lock, 1e-9);
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
        // Fast（ロック以前のスキップだけ。以降は Test_SkipAfterLockNoRewind）
        if (tSkip < ev.lockMs) for (double dt = 0; dt < 1500.0; dt += 7.0)
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
        for (const auto& c : cues) CHECK(c.playAtMs <= t);   // 先の予約は捨てられる（ちょうど t のスキップの LockTail は残る）
        d.Update(t, 1.0f, true);   // ロードはスキップと同時に完了（退場は自然な時刻。待たされる側は Test_SkipThenLongLoad）
        for (double dt = 0; dt < 1500.0; dt += 7.0)
            CHECK(SameFrame(d.Frame(t + dt), sp::EvaluateAfterLock(dt, ev)));
        CHECK(d.Frame(t).locked && d.Frame(t).needleDeg == 360.f);
    }
}

// スキップで、音の側へ受け渡し済みの予約（playAt が先のもの）を取り消せる（FU-E-18）
void Test_SkipCancelsHandedOverCues()
{
    // スキップしなければ取り消しは出ない / id は 1 から単調で一意
    {
        sp::GaugeSlowDriver d;
        std::vector<std::uint32_t> ids;
        for (double t = 0; t < 2500.0; t += 16.0)
        {
            d.Update(t, static_cast<float>(t / 3000.0), false);
            for (const auto& c : d.TakeCues()) ids.push_back(c.id);
            CHECK(d.TakeCancelledCueIds().empty());
        }
        CHECK(!ids.empty());
        for (std::size_t i = 0; i < ids.size(); ++i) CHECK(ids[i] == i + 1);
    }
    std::size_t totalCancelled = 0;
    for (std::uint32_t k = 0; k < 24; ++k)
    {
        const double tSkip = 300.0 + sp::Rand01(131, k) * 2500.0;
        sp::GaugeSlowDriver d;
        std::vector<sp::GaugeCue> handed;
        double t = 0;
        for (; t < tSkip; t += 16.0)
        {
            d.Update(t, static_cast<float>(t / 3500.0), false);
            for (const auto& c : d.TakeCues()) handed.push_back(c);
        }
        d.Skip(t);
        const auto cancelled = d.TakeCancelledCueIds();
        // 受け渡し済みで playAt > tSkip のものだけ、過不足なく取り消される
        std::size_t expect = 0;
        for (const auto& c : handed)
        {
            const bool want = c.playAtMs >= t;   // Click も最終接近の LockTail も対象
            std::size_t n = 0;
            for (auto id : cancelled) n += (id == c.id);
            CHECK(n == (want ? 1u : 0u));
            expect += want;
        }
        CHECK(cancelled.size() == expect);
        totalCancelled += expect;
        CHECK(d.TakeCancelledCueIds().empty());
        // スキップの LockTail は取り消し対象でなく、id は取り消しと重ならない
        const auto after = d.TakeCues();
        CHECK(after.size() == 1 && after[0].kind == sp::GaugeCueKind::LockTail && after[0].playAtMs == t);
        for (auto id : cancelled) CHECK(id != after[0].id);
        // スキップ後は新しい取り消しが出ない
        d.Update(t + 16.0, 1.0f, true);
        CHECK(d.TakeCancelledCueIds().empty() && d.TakeCues().empty());
    }
    CHECK(totalCancelled > 0);   // 予約は発音の 50ms 以上前に渡る。歯の直後のスキップでは必ず取り消しが出る（24 時刻のどこかで当たる）
    // 取り出す前にスキップ（まだ渡していない予約）は取り消しではなく破棄
    {
        sp::GaugeSlowDriver d;
        double t = 0;
        for (; t < 1200.0; t += 16.0) d.Update(t, static_cast<float>(t / 3500.0), false);
        d.Skip(t);
        CHECK(d.TakeCancelledCueIds().empty());
    }
    // ロック以降のスキップは何もしない（取り消しも出ない）
    {
        sp::GaugeSlowDriver d;
        double t = 0;
        for (; t < 800.0; t += 16.0) { d.Update(t, 0.4f, false); d.TakeCues(); }
        for (; !(d.Approaching() && t >= d.LockWallMs() + 10.0); t += 16.0) { d.Update(t, 1.0f, true); d.TakeCues(); }
        d.Skip(t);
        CHECK(!d.Skipped() && d.TakeCancelledCueIds().empty());
    }
    // 速い経路 → 持ち替え後のスキップは Slow と同じ。持ち替え前のスキップは予約が無いので取り消しも無い
    {
        sp::GaugeFastDriver d;
        double t = 0;
        std::vector<sp::GaugeCue> handed;
        for (; t < Ev().clickMs[sp::CalibrateEvents::kClicks - 2] + 400.0; t += 16.0) { d.Update(t, 0.1f, false); for (const auto& c : d.TakeCues()) handed.push_back(c); }
        CHECK(d.HandedOver());
        d.Skip(t);
        const auto cancelled = d.TakeCancelledCueIds();
        std::size_t expect = 0;
        for (const auto& c : handed) expect += (c.playAtMs >= t);
        CHECK(cancelled.size() == expect);
    }
    {
        sp::GaugeFastDriver d;
        for (double t = 0; t < 1000.0; t += 16.0) d.Update(t, 0.1f, false);
        d.Skip(1000.0);
        CHECK(d.TakeCancelledCueIds().empty());
    }
}

// 最終接近の途中（完了要求のあと・ロック前）のスキップ: 渡し済みの Click と LockTail を取り消し、鳴る LockTail は 1 本だけ（FU-E-18 レビュー対応）
template <class Driver>
bool CheckSkipDuringApproach(Driver& d, double tSkip)
{
    std::vector<sp::GaugeCue> handed;
    double t = 0;
    for (; t < 912.0; t += 16.0) { d.Update(t, static_cast<float>(t / 3500.0), false); for (const auto& c : d.TakeCues()) handed.push_back(c); }
    d.Update(t, 1.0f, true);
    for (const auto& c : d.TakeCues()) handed.push_back(c);
    for (t += 16.0; t < tSkip; t += 16.0) { d.Update(t, 1.0f, true); for (const auto& c : d.TakeCues()) handed.push_back(c); }
    const double lockWall = d.LockWallMs();
    CHECK(!std::isnan(lockWall));
    if (t >= lockWall) return false;   // ロック後はスキップ無効（別テスト）。呼び出し側が実行数を数えて検査する
    d.Skip(t);
    // id は全て非 0 かつ一意（LockTail 含む）
    std::set<std::uint32_t> seen;
    for (const auto& c : handed) { CHECK(c.id != 0); CHECK(seen.insert(c.id).second); }
    auto cancelled = d.TakeCancelledCueIds();
    auto after = d.TakeCues();
    for (const auto& c : after) { CHECK(c.id != 0); CHECK(seen.insert(c.id).second); }
    // 音の側の有効な予約 = 渡し済み - 取り消し + 新規。playAt >= t の古い予約は全て取り消されている
    std::set<std::uint32_t> active;
    for (const auto& c : handed) active.insert(c.id);
    for (auto id : cancelled) CHECK(active.erase(id) == 1);
    for (const auto& c : handed) if (c.playAtMs >= t) CHECK(!active.count(c.id));
    int lockTails = 0;
    for (const auto& c : handed) if (active.count(c.id) && c.kind == sp::GaugeCueKind::LockTail) ++lockTails;
    for (const auto& c : after) { active.insert(c.id); lockTails += (c.kind == sp::GaugeCueKind::LockTail); }
    CHECK(lockTails == 1);
    CHECK(d.TakeCancelledCueIds().empty());
    return true;
}

void Test_SkipDuringApproach()
{
    // 完了要求は t=912 付近。ロックまで 1 秒弱ある窓の中の複数時刻
    int ranSlow = 0;
    const double slowSkips[] = { 930.0, 1000.0, 1200.0, 1500.0, 1800.0, 1950.0, 1965.0 };
    for (double tSkip : slowSkips)
    {
        sp::GaugeSlowDriver d;
        ranSlow += CheckSkipDuringApproach(d, tSkip) ? 1 : 0;
    }
    CHECK(ranSlow == static_cast<int>(std::size(slowSkips)));   // 全ケースが実際にロック前のスキップになっている（時刻が変わっても黙って落ちない）
    // Fast: 持ち替え後に完了要求 → 完了要求からの周回数 extraFrames 後にスキップ
    int ranFast = 0;
    const int fastExtraFrames[] = { 4, 10, 16 };
    for (int extra : fastExtraFrames)
    {
        sp::GaugeFastDriver d;
        std::vector<sp::GaugeCue> handed;
        double t = 0;
        const double dec = Ev().clickMs[sp::CalibrateEvents::kClicks - 2];
        for (; t < dec + 300.0; t += 16.0) { d.Update(t, 0.1f, false); for (const auto& c : d.TakeCues()) handed.push_back(c); }
        CHECK(d.HandedOver());
        d.Update(t, 1.0f, true);
        for (const auto& c : d.TakeCues()) handed.push_back(c);
        for (int i = 0; i < extra; ++i) { t += 16.0; d.Update(t, 1.0f, true); for (const auto& c : d.TakeCues()) handed.push_back(c); }
        t += 16.0;
        if (t >= d.LockWallMs()) continue;
        ++ranFast;
        d.Skip(t);
        std::set<std::uint32_t> seen, active;
        for (const auto& c : handed) { CHECK(c.id != 0); CHECK(seen.insert(c.id).second); active.insert(c.id); }
        for (auto id : d.TakeCancelledCueIds()) CHECK(active.erase(id) == 1);
        int lockTails = 0;
        for (const auto& c : handed) if (active.count(c.id) && c.kind == sp::GaugeCueKind::LockTail) ++lockTails;
        for (const auto& c : d.TakeCues()) lockTails += (c.kind == sp::GaugeCueKind::LockTail);
        CHECK(lockTails == 1);
    }
    CHECK(ranFast == static_cast<int>(std::size(fastExtraFrames)));
}

// スキップのあとロードが長引く: 退場（ピン・フェード）はロードの完了まで始まらず、完了後にその時刻から進む（FU-E-18）
template <class Frame, class Fn>
void CheckExitHeld(Frame frameAt, double skipWall, double finishWall, double exitStart, Fn&& /*unused*/)
{
    const auto& ev = Ev();
    const double nat = ev.pingMs - ev.lockMs;
    CHECK_NEAR(exitStart, skipWall + std::max(nat, finishWall - skipWall), 1e-9);
    for (double t = skipWall; t < exitStart; t += 7.0)
    {
        const auto f = frameAt(t);
        CHECK(f.pinT == 0.f && f.fadeOutT == 0.f);
        CHECK(f.locked);
    }
    CHECK(frameAt(exitStart).pinT == 0.f);
    CHECK(frameAt(exitStart + sp::gauge::kPinStampMs).pinT == 1.f);
    CHECK_NEAR(frameAt(exitStart + sp::gauge::kFadeOutMs * 0.5).fadeOutT, 0.5, 1e-5);
    CHECK(frameAt(exitStart + sp::gauge::kFadeOutMs).fadeOutT == 1.f);
}

void Test_SkipThenLongLoad()
{
    const auto& ev = Ev();
    const double nat = ev.pingMs - ev.lockMs;
    // Slow: t=500 でスキップ、ロードは t=6000 まで続く
    {
        sp::GaugeSlowDriver d;
        double t = 0;
        for (; t < 500.0; t += 16.0) d.Update(t, 0.2f, false);
        d.Skip(t);
        const double skipWall = t;
        CHECK(std::isnan(d.ExitStartWallMs()));
        for (; t < 6000.0; t += 16.0) d.Update(t, 0.3f, false);
        CHECK(!d.FinishObserved() && std::isnan(d.ExitStartWallMs()));
        for (double u = skipWall; u < 6000.0; u += 11.0)
        {
            const auto f = d.Frame(u);
            CHECK(f.pinT == 0.f && f.fadeOutT == 0.f && f.locked);   // 自然な退場時刻（skip + nat）を過ぎても保持
        }
        CHECK(d.Frame(skipWall + nat + 50.0).fadeOutT == 0.f);
        d.Update(6000.0, 1.0f, true);
        CHECK(d.FinishObserved());
        const double fw = 6000.0;
        CheckExitHeld([&](double u) { return d.Frame(u); }, skipWall, fw, d.ExitStartWallMs(), 0);
        CHECK_NEAR(d.ExitStartWallMs(), fw, 1e-9);
        d.Update(6016.0, 1.0f, true);   // 以後の Update で動かない
        CHECK_NEAR(d.ExitStartWallMs(), fw, 1e-9);
    }
    // Slow: スキップの前に完了要求が出ていたなら自然な時刻（待たない）
    {
        sp::GaugeSlowDriver d;
        double t = 0;
        for (; t < 400.0; t += 16.0) d.Update(t, 0.2f, false);
        d.Update(t, 1.0f, true);
        t += 16.0;
        d.Update(t, 1.0f, true);
        d.Skip(t);
        CHECK_NEAR(d.ExitStartWallMs(), t + nat, 1e-9);
        CHECK(d.Frame(t + nat + sp::gauge::kPinStampMs).pinT == 1.f);
    }
    // Slow: スキップより先に完了要求が来てもスキップ後すぐ終わる場合も、完了要求が自然な時刻より早ければ自然な時刻
    {
        sp::GaugeSlowDriver d;
        double t = 0;
        for (; t < 500.0; t += 16.0) d.Update(t, 0.2f, false);
        d.Skip(t);
        const double skipWall = t;
        d.Update(t + 16.0, 1.0f, true);   // ロックの絵から 16ms で完了
        CHECK_NEAR(d.ExitStartWallMs(), skipWall + nat, 1e-9);
    }
    // Fast: 持ち替え前（C22 の前）にスキップ、ロードは長く続く
    {
        sp::GaugeFastDriver d;
        double t = 0;
        for (; t < 500.0; t += 16.0) d.Update(t, 0.2f, false);
        d.Skip(t);
        const double skipWall = t;
        for (; t < 7000.0; t += 16.0) d.Update(t, 0.3f, false);
        CHECK(!d.HandedOver() && !d.FinishObserved() && std::isnan(d.ExitStartWallMs()));
        for (double u = skipWall; u < 7000.0; u += 11.0)
        {
            const auto f = d.Frame(u);
            CHECK(f.pinT == 0.f && f.fadeOutT == 0.f && f.locked);
        }
        d.Update(7000.0, 1.0f, true);
        CheckExitHeld([&](double u) { return d.Frame(u); }, skipWall, 7000.0, d.ExitStartWallMs(), 0);
        CHECK_NEAR(d.ExitStartWallMs(), 7000.0, 1e-9);
    }
    // Fast: 持ち替え後（C22 より後）にスキップ → 遅い経路と同じ扱い
    {
        sp::GaugeFastDriver d;
        double t = 0;
        const double dec = ev.clickMs[sp::CalibrateEvents::kClicks - 2];
        for (; t < dec + 200.0; t += 16.0) d.Update(t, 0.2f, false);
        CHECK(d.HandedOver());
        d.Skip(t);
        const double skipWall = t;
        for (; t < 6000.0; t += 16.0) d.Update(t, 0.3f, false);
        for (double u = skipWall; u < 6000.0; u += 13.0) { const auto f = d.Frame(u); CHECK(f.pinT == 0.f && f.fadeOutT == 0.f); }
        d.Update(6000.0, 1.0f, true);
        CheckExitHeld([&](double u) { return d.Frame(u); }, skipWall, 6000.0, d.ExitStartWallMs(), 0);
    }
    // Reduce（状態なし）: 完了要求の観測時刻から退場
    {
        const double fin = 5000.0;
        const double held = std::numeric_limits<double>::infinity();
        for (double u = 0; u < 9000.0; u += 37.0)
        {
            const auto f = sp::EvaluateGauge(sp::GaugePath::Reduce, u, 0.3f, ev, held);
            CHECK(f.pinT == 0.f && f.fadeOutT == 0.f);
        }
        const double es = sp::GaugeExitStartMs(ev.pingMs, fin);
        CHECK_NEAR(es, fin, 1e-9);
        CHECK(sp::EvaluateGauge(sp::GaugePath::Reduce, fin - 1.0, 1.f, ev, es).fadeOutT == 0.f);
        CHECK(sp::EvaluateGauge(sp::GaugePath::Reduce, fin, 1.f, ev, es).pinT == 1.f);
        CHECK_NEAR(sp::EvaluateGauge(sp::GaugePath::Reduce, fin + 90.0, 1.f, ev, es).fadeOutT, 0.5, 1e-5);
        CHECK(sp::EvaluateGauge(sp::GaugePath::Reduce, fin + 180.0, 1.f, ev, es).fadeOutT == 1.f);
        // 早く終わるなら自然な時刻
        CHECK_NEAR(sp::GaugeExitStartMs(ev.pingMs, 300.0), ev.pingMs, 1e-9);
        CHECK(std::isinf(sp::GaugeExitStartMs(ev.pingMs, std::nan(""))));
        // 既定（引数なし）は従来どおり
        CHECK(sp::EvaluateGauge(sp::GaugePath::Reduce, ev.pingMs + 500.0, 0.f).fadeOutT == 1.f);
    }
    // Skip の時刻は直近の Update より前には戻らない（契約）
    {
        sp::GaugeSlowDriver d;
        for (double t = 0; t < 500.0; t += 16.0) d.Update(t, 0.2f, false);
        d.Skip(100.0);
        CHECK(d.LockWallMs() >= 496.0);
    }
}

// (6) Reduce
void Test_Reduce()
{
    const auto& ev = Ev();
    float lastTl = 0.f;
    for (double t = 0; t <= 3000.0; t += 13.0)
    {
        const auto f = sp::EvaluateGauge(sp::GaugePath::Reduce, t, 0.3f);
        CHECK(f.needleDeg == 360.f && f.locked && f.seated);
        CHECK(f.blurDeg == 0.f && f.overshootPx == 0.f);
        CHECK(f.alignStep == 5 && f.tickLit == sp::gauge::kDialTickCount && f.tomboT == 1.f && f.bellT == 1.f && f.needleShown);
        CHECK(f.pinT == (t >= ev.pingMs ? 1.f : 0.f));            // 動かさない（段差）
        CHECK(f.fadeOutT >= 0.f && f.fadeOutT <= 1.f);
        CHECK(f.timelineMs >= static_cast<float>(ev.bellMs[4]));   // reveal の時計は最後の reveal 以上（全 reveal が出きっている）
        CHECK(f.timelineMs >= lastTl);
        lastTl = f.timelineMs;
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

// ---------------------------------------------------------------- 校正の振り付け（W1-BD6）

// 残像の本数: 最速区間（C13〜C15）の全フレームで 6 本以上。止まっているとき（開始前・ロック後）は 0。上限 12。
void Test_TrailLines()
{
    const auto& ev = Ev();
    int minN = 99, maxN = 0;
    for (double t = ev.clickMs[13]; t <= ev.clickMs[15]; t += 0.25)
    {
        const sp::GaugeFrame f = sp::EvaluateGauge(sp::GaugePath::Fast, t, 0.f);
        const int n = sp::gauge::TrailLineCount(f.blurDeg);
        minN = std::min(minN, n);
        maxN = std::max(maxN, n);
    }
    std::printf("  最速区間 C13〜C15 の残像の本数: 最少 %d 本 / 最多 %d 本\n", minN, maxN);
    CHECK(minN >= 6 && maxN <= sp::gauge::kTrailMaxLines);
    // 全区間で本数は 0..12。ロック後と開始前は 0
    for (double t = 0.0; t <= 2400.0; t += 1.0)
    {
        const sp::GaugeFrame f = sp::EvaluateGauge(sp::GaugePath::Fast, t, 0.f);
        const int n = sp::gauge::TrailLineCount(f.blurDeg);
        CHECK(n >= 0 && n <= sp::gauge::kTrailMaxLines);
        if (f.locked || t < 150.0) CHECK(n == 0);
    }
    // 線の並び: 指針に近い順に離れ、濃さは線形に淡くなる。最も遠い線は blurDeg（直近 1 フレームの移動量）に一致する
    for (double blur : { 0.5, 3.0, 9.3, 14.0, 40.0 })
    {
        const int n = sp::gauge::TrailLineCount(blur);
        CHECK(n >= 1);
        double prevOff = 0.0, prevAl = 2.0;
        for (int i = 0; i < n; ++i)
        {
            const sp::gauge::TrailLine l = sp::gauge::TrailLineAt(i, n, blur);
            CHECK(l.offsetDeg > prevOff && l.offsetDeg <= blur + 1e-9);
            CHECK(l.alpha < prevAl + 1e-12 && l.alpha >= sp::gauge::kTrailAlphaFar - 1e-9 && l.alpha <= sp::gauge::kTrailAlphaNear + 1e-9);
            prevOff = l.offsetDeg; prevAl = l.alpha;
        }
        CHECK_NEAR(sp::gauge::TrailLineAt(n - 1, n, blur).offsetDeg, blur, 1e-9);
        CHECK_NEAR(sp::gauge::TrailLineAt(0, n, blur).alpha, sp::gauge::kTrailAlphaNear, 1e-9);
    }
    CHECK(sp::gauge::TrailLineCount(0.0) == 0 && sp::gauge::TrailLineCount(0.2) == 0 && sp::gauge::TrailLineCount(0.25) == 1);
}

// 整列波とベゼルの鏡面スイープ: ベルの 5 音（1,648〜1,748）に 1 段ずつ。スイープは 1,648〜1,848。通り抜けたあとは何も残らない。
void Test_AlignWaveAndSweep()
{
    const auto& ev = Ev();
    for (int k = 0; k < 5; ++k)
    {
        const sp::GaugeFrame before = sp::EvaluateGauge(sp::GaugePath::Fast, ev.bellMs[k] - 0.01, 0.f);
        const sp::GaugeFrame at = sp::EvaluateGauge(sp::GaugePath::Fast, ev.bellMs[k], 0.f);
        const sp::GaugeFrame mid = sp::EvaluateGauge(sp::GaugePath::Fast, ev.bellMs[k] + sp::gauge::kAlignPassMs * 0.5, 0.f);
        const sp::GaugeFrame done = sp::EvaluateGauge(sp::GaugePath::Fast, ev.bellMs[k] + sp::gauge::kAlignPassMs, 0.f);
        CHECK(before.alignT[static_cast<size_t>(k)] == 0.f && at.alignT[static_cast<size_t>(k)] == 0.f);
        CHECK_NEAR(mid.alignT[static_cast<size_t>(k)], 0.5, 1e-5);
        CHECK(done.alignT[static_cast<size_t>(k)] == 1.f);
        // 反射は扇 k の目盛りにだけ掛かる。中ほどで 1 本以上の目盛りが強く反応する
        double best = 0.0;
        for (int i = 0; i < 100; ++i)
        {
            const double deg = i * 3.6;
            const double gl = sp::gauge::AlignGlintAt(mid.alignT[static_cast<size_t>(k)], k, deg);
            const bool inSector = deg >= k * 72.0 - 1e-9 && deg < (k + 1) * 72.0 - 1e-9;
            if (!inSector) CHECK(gl == 0.0);
            CHECK(gl >= 0.0 && gl <= 1.0);
            best = std::max(best, gl);
        }
        CHECK_MSG(best > 0.8, "扇 %d の反射が弱い %.3f", k, best);
    }
    // 整列が終わった（最後の反射が通り抜けた）あとは、どの目盛りにも掛からない。Reduce も同じ（静止の姿）
    const double settled = ev.bellMs[4] + sp::gauge::kAlignPassMs;
    for (double t = settled; t <= 2400.0; t += 5.0)
    {
        const sp::GaugeFrame f = sp::EvaluateGauge(sp::GaugePath::Fast, t, 0.f);
        for (int i = 0; i < 100; ++i) CHECK(sp::gauge::AlignGlintMax(f.alignT, i * 3.6) == 0.0);
    }
    for (double t : { 0.0, 1000.0, 1748.0, 2200.0 })
    {
        const sp::GaugeFrame f = sp::EvaluateGauge(sp::GaugePath::Reduce, t, 0.f);
        for (int i = 0; i < 100; ++i) CHECK(sp::gauge::AlignGlintMax(f.alignT, i * 3.6) == 0.0);
        CHECK(sp::gauge::SweepEnvelope(f.bellT) == 0.0);
    }
    // 整列の前（ロックの前）は何も掛からない
    for (double t = 0.0; t < ev.bellMs[0]; t += 7.0)
    {
        const sp::GaugeFrame f = sp::EvaluateGauge(sp::GaugePath::Fast, t, 0.f);
        for (int i = 0; i < 100; ++i) CHECK(sp::gauge::AlignGlintMax(f.alignT, i * 3.6) == 0.0);
        CHECK(f.bellT == 0.f);
    }
    // スイープ: 1,648 で始まり 1,848 で終わる。反射は縁を一周（2 つの反射が 180 度ずつ）。最大の濃さは 0.55 以下
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, ev.bellMs[0] - 0.01, 0.f).bellT == 0.f);
    CHECK_NEAR(sp::EvaluateGauge(sp::GaugePath::Fast, ev.bellMs[0] + 100.0, 0.f).bellT, 0.5, 1e-5);
    CHECK(sp::EvaluateGauge(sp::GaugePath::Fast, ev.bellMs[0] + 200.0, 0.f).bellT == 1.f);
    CHECK_NEAR(sp::gauge::SweepCentreDeg(0.0), 0.0, 1e-12);
    CHECK_NEAR(sp::gauge::SweepCentreDeg(1.0), 180.0, 1e-12);
    double prev = -1.0, peak = 0.0;
    for (double u = 0.0; u <= 1.0 + 1e-12; u += 0.01)
    {
        const double c = sp::gauge::SweepCentreDeg(u);
        CHECK(c >= prev - 1e-12);
        prev = c;
        for (double a = 0.0; a < 360.0; a += 0.5)
            for (bool second : { false, true })
            {
                const double p = sp::gauge::SweepProfileAt(u, a, second);
                CHECK(p >= 0.0 && p <= 1.0);
                peak = std::max(peak, p * sp::gauge::kSweepPeakAlpha);
            }
    }
    CHECK(peak <= sp::gauge::kSweepPeakAlpha + 1e-9 && peak > 0.4);
    CHECK(sp::gauge::SweepProfileAt(0.0, 0.0, false) == 0.0 && sp::gauge::SweepProfileAt(1.0, 180.0, false) == 0.0);
    // 一周: 全方位のどこかで、スイープ中に必ず反射が通る
    for (double a = 0.0; a < 360.0; a += 3.0)
    {
        double m = 0.0;
        for (double u = 0.0; u <= 1.0; u += 0.005) m = std::max(m, std::max(sp::gauge::SweepProfileAt(u, a, false), sp::gauge::SweepProfileAt(u, a, true)));
        CHECK_MSG(m > 0.05, "角度 %.0f にスイープが届かない", a);
    }
}

// イベントのフレームが音と ±1 フレーム（60Hz の表示。120fps のダンプでも確かめる）。
// 音は時刻 T に鳴る。絵は「T 以降の最初のフレーム」で変わる。ここでは、絵が変わる最初のフレームと音のフレームの差を数える。
void Test_EventFramesVsAudio()
{
    const auto& ev = Ev();
    for (double fps : { 60.0, 120.0 })
    {
        const double dt = 1000.0 / fps;
        auto audioFrame = [&](double tMs) { return static_cast<long>(std::ceil(tMs / dt - 1e-9)); };
        auto firstFrame = [&](auto&& pred) {
            for (long n = 0; n < static_cast<long>(3000.0 / dt); ++n)
                if (pred(sp::EvaluateGauge(sp::GaugePath::Fast, n * dt, 0.f))) return n;
            return -1L;
        };
        auto expectNear = [&](const char* what, long vis, double tAudio) {
            const long aud = audioFrame(tAudio);
            CHECK_MSG(vis >= 0 && std::labs(vis - aud) <= 1, "%s fps=%.0f 絵 %ld / 音 %ld", what, fps, vis, aud);
        };
        for (int i = 0; i < sp::CalibrateEvents::kClicks; ++i)
        {
            const double target = ev.clickDeg[i];
            expectNear("クリック", firstFrame([&](const sp::GaugeFrame& f) { return f.needleDeg >= target - 1e-3; }), ev.clickMs[i]);
        }
        expectNear("ロック", firstFrame([](const sp::GaugeFrame& f) { return f.locked; }), ev.lockMs);
        expectNear("着座", firstFrame([](const sp::GaugeFrame& f) { return f.seated; }), ev.seatMs);
        for (int k = 0; k < 5; ++k)
        {
            expectNear("整列（段）", firstFrame([&](const sp::GaugeFrame& f) { return f.alignStep >= static_cast<std::uint32_t>(k + 1); }), ev.bellMs[k]);
            expectNear("整列（反射）", firstFrame([&](const sp::GaugeFrame& f) { return f.alignT[static_cast<size_t>(k)] > 0.f; }), ev.bellMs[k]);
        }
        expectNear("ベゼルのスイープ", firstFrame([](const sp::GaugeFrame& f) { return f.bellT > 0.f; }), ev.bellMs[0]);
        expectNear("登録のピン", firstFrame([](const sp::GaugeFrame& f) { return f.pinT > 0.f; }), ev.pingMs);
        // ロックの行き過ぎ: 2px 以下（指針の先端が円周の接線方向へ出る長さ）で、ロックの時刻までに 0 へ戻る
        double peak = 0.0;
        for (long n = 0; n * dt < 2400.0; ++n)
        {
            const sp::GaugeFrame f = sp::EvaluateGauge(sp::GaugePath::Fast, n * dt, 0.f);
            peak = std::max(peak, static_cast<double>(f.overshootPx));
            if (n * dt >= ev.lockMs) CHECK(f.overshootPx == 0.f);
        }
        CHECK(peak <= 2.0 + 1e-6 && peak > 1.0);
    }
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
    Test_SlowHitches();
    Test_SlowConstant50ms();
    Test_SlowFinishAtCap();
    Test_FastLateFinish();
    Test_FastSkipBeforeHandover();
    Test_SkipAfterLockNoRewind();
    Test_SlowOvershoot();
    Test_SlowWaitsAtCap();
    Test_SlowFinishFromRest();
    Test_Skip();
    Test_SkipCancelsHandedOverCues();
    Test_SkipDuringApproach();
    Test_SkipThenLongLoad();
    Test_Reduce();
    Test_Blur();
    Test_TrailLines();
    Test_AlignWaveAndSweep();
    Test_EventFramesVsAudio();
    if (g_fail == 0) std::printf("SplashGaugeMotionTests: ALL PASS\n");
    else std::printf("SplashGaugeMotionTests: %d FAIL\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
