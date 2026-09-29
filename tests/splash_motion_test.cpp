// 起動画面の動き / 進捗 / 起動時間の純ロジックの単体テスト。
//   ・core/SplashMotion.h   … イージング / スプリング / 火花 / タイムライン状態機械 / 文言クロスフェード / Tips ローテーション
//   ・core/SplashProgress.h … 重み付き進捗 / 微進み / startup.json の EMA とクランプ
// ヘッダオンリー・依存ゼロ（Win32 も GPU も要らない）なので、スタンドアロンの CI でも回る。
#include "core/SplashDirector.h"
#include "core/SplashMotion.h"
#include "core/SplashProgress.h"
#include "core/SplashTips.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECK_NEAR(a, b, eps) do { const double aa_ = (a), bb_ = (b); if (!(std::fabs(aa_ - bb_) <= (eps))) { \
    std::printf("FAIL %s:%d  %s=%.6f vs %s=%.6f\n", __FILE__, __LINE__, #a, aa_, #b, bb_); ++g_fail; } } while (0)

namespace sp = dx12e::splash;

// ------------------------------------------------------------ イージング / スプリング

void Test_Easing()
{
    using F = double (*)(double);
    const F outs[] = { sp::EaseOutCubic, sp::EaseOutQuint, sp::EaseOutExpo, sp::EaseInQuad, sp::EaseInCubic,
                       sp::EaseInOutCubic, sp::EaseInOutSine };
    for (F f : outs)
    {
        CHECK_NEAR(f(0.0), 0.0, 1e-9);
        CHECK_NEAR(f(1.0), 1.0, 1e-9);
        double prev = -1.0;
        for (int i = 0; i <= 100; ++i)
        {
            const double v = f(i / 100.0);
            CHECK(v >= prev - 1e-12);   // 単調非減少
            prev = v;
        }
        CHECK_NEAR(f(-3.0), 0.0, 1e-9);   // 範囲外は丸める
        CHECK_NEAR(f(9.0), 1.0, 1e-9);
    }
    // out 系は前半が速い / in 系は後半が速い / inOut は中点が 0.5
    CHECK(sp::EaseOutCubic(0.5) > 0.5);
    CHECK(sp::EaseOutExpo(0.3) > sp::EaseOutCubic(0.3));   // expo は鋭く減速
    CHECK(sp::EaseInCubic(0.5) < 0.5);
    CHECK_NEAR(sp::EaseInOutCubic(0.5), 0.5, 1e-9);
    // back: 端は 0/1 で、途中で 1 を超える（約 10%）。inBack は先に負へ引く
    CHECK_NEAR(sp::EaseOutBack(0.0), 0.0, 1e-9);
    CHECK_NEAR(sp::EaseOutBack(1.0), 1.0, 1e-9);
    double mx = 0.0, mn = 0.0;
    for (int i = 0; i <= 200; ++i) { mx = std::max(mx, sp::EaseOutBack(i / 200.0)); mn = std::min(mn, sp::EaseInBack(i / 200.0)); }
    CHECK(mx > 1.08 && mx < 1.12);
    CHECK(mn < -0.05);
}

void Test_Spring()
{
    const double w = 25.0, z = 0.46;
    CHECK_NEAR(sp::SpringStep(0.0, w, z), 0.0, 1e-12);
    CHECK_NEAR(sp::SpringStepVelocity(0.0, w, z), 0.0, 1e-12);
    // 落ち着く
    CHECK_NEAR(sp::SpringStep(2.0, w, z), 1.0, 1e-4);
    // 最大オーバーシュートが解析式と一致（ζ=0.46 で約 19%）
    double peak = 0.0;
    for (int i = 0; i < 4000; ++i) peak = std::max(peak, sp::SpringStep(i * 0.0005, w, z));
    CHECK_NEAR(peak - 1.0, sp::SpringOvershoot(z), 0.003);
    CHECK(peak > 1.10);    // 「ポン」と行き過ぎる
    // ζ が大きいほどオーバーシュートが小さい
    CHECK(sp::SpringOvershoot(0.3) > sp::SpringOvershoot(0.5));
    CHECK(sp::SpringOvershoot(0.5) > sp::SpringOvershoot(0.8));
    CHECK_NEAR(sp::SpringOvershoot(1.0), 0.0, 1e-12);
    // 速度は位置の微分（数値微分と一致）
    for (double u : { 0.02, 0.08, 0.15, 0.4 })
    {
        const double h = 1e-5;
        const double num = (sp::SpringStep(u + h, w, z) - sp::SpringStep(u - h, w, z)) / (2 * h);
        CHECK_NEAR(sp::SpringStepVelocity(u, w, z), num, 1e-3);
    }
    // 初期条件が保たれる（x0 / v0）
    const sp::SpringState s0 = sp::SpringFree(0.0, 30.0, 0.35, -0.09, 8.5);
    CHECK_NEAR(s0.x, -0.09, 1e-12);
    CHECK_NEAR(s0.v, 8.5, 1e-12);
    const sp::SpringState s1 = sp::SpringFree(1e-6, 30.0, 0.35, -0.09, 8.5);
    CHECK_NEAR(s1.x, -0.09, 1e-4);
    CHECK_NEAR(s1.v, 8.5, 1e-2);
    // ζ >= 1 を渡しても NaN にならない
    const sp::SpringState s2 = sp::SpringFree(0.1, 20.0, 1.5, -1.0, 0.0);
    CHECK(std::isfinite(s2.x) && std::isfinite(s2.v));
}

void Test_Sparks()
{
    const sp::BurstSpec b = sp::PopBurst();
    int visible0 = 0;
    for (int i = 0; i < b.count; ++i)
    {
        sp::Spark a, c;
        CHECK(sp::SparkAt(b, i, 0.05, a));
        CHECK(sp::SparkAt(b, i, 0.05, c));
        CHECK(a.x == c.x && a.y == c.y && a.size == c.size);   // 決定論
        CHECK(a.alpha >= 0.0 && a.alpha <= 1.0);
        CHECK(a.size > 0.0);
        ++visible0;
        sp::Spark d;
        CHECK(!sp::SparkAt(b, i, b.life + 0.01, d));            // 寿命の後は出ない
        CHECK(!sp::SparkAt(b, i, -0.1, d));                      // 発生前は出ない
    }
    CHECK(visible0 == b.count);
    // 時間とともに中心から離れる
    sp::Spark p1, p2;
    sp::SparkAt(b, 3, 0.05, p1); sp::SparkAt(b, 3, 0.25, p2);
    CHECK(std::hypot(p2.x, p2.y) > std::hypot(p1.x, p1.y));
    // 別の種は別の軌道
    sp::BurstSpec b2 = b; b2.seed = b.seed + 1;
    sp::Spark q; sp::SparkAt(b2, 3, 0.25, q);
    CHECK(q.x != p2.x || q.y != p2.y);
}

// ------------------------------------------------------------ タイムライン

// dt 刻みで [t0, t1] を進めて、各フレームを返す。
struct Run
{
    sp::Timeline tl;
    std::vector<sp::SplashFrame> frames;
    double dt = 1.0 / 60.0;
    void Go(double t0, double t1, double (*target)(double), double finishAt, double mainShownAt)
    {
        for (double t = t0; t <= t1 + 1e-9; t += dt)
            frames.push_back(tl.Step(t, target ? target(t) : 0.0, finishAt, mainShownAt));
    }
};

double Ramp(double t) { return sp::Clamp01((t - 0.9) / 3.0); }

void Test_Timeline_Intro()
{
    Run r;
    r.Go(0.0, 0.9, Ramp, -1.0, -1.0);
    const auto& f0 = r.frames.front();
    CHECK(f0.phase == sp::Phase::Intro);
    CHECK_NEAR(f0.logoScaleX, 0.0, 1e-9);            // 始まりは大きさ 0
    CHECK_NEAR(f0.windowAlpha, 0.0, 1e-9);           // 窓は透明から登場
    CHECK(f0.progress == 0.0);
    double mx = 0.0;
    for (const auto& f : r.frames) mx = std::max(mx, f.logoScaleY);
    CHECK(mx > 1.10);                                // 行き過ぎる（ポン）
    // イントロ終盤にはほぼ 1 に落ち着く
    const auto& fe = r.frames.back();
    CHECK_NEAR(fe.logoScaleX, 1.0, 0.06);
    CHECK_NEAR(fe.windowAlpha, 1.0, 1e-6);
    // 火花のバーストは 0.2s から
    bool sawBurst = false;
    for (const auto& f : r.frames) { if (f.t < 0.199) CHECK(f.introBurstAge < 0.0); else sawBurst = sawBurst || f.introBurstAge >= 0.0; }
    CHECK(sawBurst);
    // ワードマーク: 各文字は順に始まる（i が大きいほど遅い）
    for (int i = 0; i < 9; ++i) CHECK(sp::LetterIntro(i, 0.45).alpha >= sp::LetterIntro(i + 1, 0.45).alpha);
    CHECK_NEAR(sp::LetterIntro(0, 0.0).alpha, 0.0, 1e-9);
    CHECK_NEAR(sp::LetterIntro(9, 0.9).dy, 0.0, 1.0);   // 0.9s には全文字が着地間近
    CHECK(std::fabs(sp::LetterIntro(0, 5.0).dy) < 1e-3);
}

// 実際の終わり方に依らず、ready 以降の並びは同じ。t=finish 基準で検証する。
void CheckReadySequence(double finishAt, double expectedReadyStart)
{
    sp::Timeline tl;
    std::vector<sp::SplashFrame> fr;
    double shownAt = -1.0;
    int wantCount = 0;
    for (double t = 0.0; t < expectedReadyStart + 2.5; t += 1.0 / 60.0)
    {
        sp::SplashFrame f = tl.Step(t, Ramp(t), finishAt, shownAt);
        if (f.wantMainShow) { ++wantCount; shownAt = t; }
        fr.push_back(f);
    }
    CHECK(wantCount == 1);                              // メイン窓を出す合図は一度だけ
    CHECK_NEAR(tl.ReadyStart(), expectedReadyStart, 1e-9);
    const auto& P = tl.params();
    // フェーズの順序: Intro/Loading → Ready → Transition → Done（逆戻りしない）
    int rank = 0; bool sawReady = false, sawTrans = false, sawDone = false;
    for (const auto& f : fr)
    {
        int rk = f.phase == sp::Phase::Intro ? 0 : f.phase == sp::Phase::Loading ? 1 : f.phase == sp::Phase::Ready ? 2
               : f.phase == sp::Phase::Transition ? 3 : 4;
        CHECK(rk >= rank); rank = rk;
        sawReady |= rk == 2; sawTrans |= rk == 3; sawDone |= rk == 4;
        // 100% は「リングが完成してから」だけ
        if (f.percent == 100) CHECK(f.sinceReady >= P.catchUpDur - 1e-9);
        // 進捗は ready 前に 1 へ届かない
        if (f.sinceReady < 0.0) CHECK(f.progress <= P.progressCap + 1e-9);
    }
    CHECK(sawReady && sawTrans && sawDone);
    // ポンは ready 開始 + (catchUp + lap)。閃光/衝撃波/火花は同時に始まる
    const double pop = expectedReadyStart + P.PopAfterReady();
    CHECK_NEAR(tl.PopTime(), pop, 1e-9);
    for (const auto& f : fr)
    {
        if (f.t < pop - 1e-9) { CHECK(f.sincePop < 0.0); CHECK(f.popBurstAge < 0.0); CHECK(f.flash == 0.0); }
        if (f.t >= pop + 1e-9 && f.t < pop + 0.05) { CHECK(f.popBurstAge >= 0.0); CHECK(f.flash > 0.4); CHECK(f.shock >= 0.0); }
    }
    // ハイライト周回は catchUp の後・ポンの前
    bool lapSeen = false;
    for (const auto& f : fr)
        if (f.lapActive) { lapSeen = true; CHECK(f.t >= expectedReadyStart + P.catchUpDur - 1e-9 && f.t <= pop + 1e-9); }
    CHECK(lapSeen);
    // メイン窓は余韻の後（transReady）に出す。退場はその後に始まる
    const double transReady = expectedReadyStart + P.ReadyDur();
    CHECK(shownAt >= transReady - 1e-9 && shownAt < transReady + 0.05);
    // 退場: 拡大しながら透明になる
    double zoomMax = 0.0; double lastAlpha = 1.0; bool alphaDrops = false;
    for (const auto& f : fr)
        if (f.sinceTransition >= 0.0)
        {
            zoomMax = std::max(zoomMax, f.zoom);
            CHECK(f.windowAlpha <= lastAlpha + 1e-9);
            if (f.windowAlpha < lastAlpha - 1e-9) alphaDrops = true;
            lastAlpha = f.windowAlpha;
        }
    CHECK(zoomMax > 1.05);
    CHECK(alphaDrops);
    CHECK_NEAR(fr.back().windowAlpha, 0.0, 1e-9);
    CHECK(fr.back().finished);
}

void Test_Timeline_Ready_OnTime() { CheckReadySequence(2.0, 2.0); }     // 定刻
void Test_Timeline_Ready_Early()  { CheckReadySequence(0.3, 0.9); }     // 早着: intro が終わるまで待つ
void Test_Timeline_Ready_Late()   { CheckReadySequence(6.0, 6.0); }     // 遅着: loading が続いてから

void Test_Timeline_LateStaysLoading()
{
    sp::Timeline tl;
    for (double t = 0.0; t < 6.0; t += 1.0 / 60.0)
    {
        const auto f = tl.Step(t, 0.3, -1.0, -1.0);
        CHECK(f.phase == (t < 0.9 ? sp::Phase::Intro : sp::Phase::Loading));
        CHECK(f.progress <= tl.params().progressCap + 1e-9);
        CHECK(f.percent < 100);
        CHECK(f.windowAlpha > 0.99 || t < 0.2);
    }
}

void Test_Timeline_ProgressMonotonic()
{
    sp::Timeline tl;
    double prev = 0.0;
    for (double t = 0.0; t < 4.0; t += 1.0 / 60.0)
    {
        const double target = 0.5 + 0.4 * std::sin(t * 7.0);   // 揺れる（一時的に下がる）目標
        const auto f = tl.Step(t, target, -1.0, -1.0);
        CHECK(f.progress >= prev - 1e-12);
        prev = f.progress;
    }
    CHECK(prev > 0.5);   // 目標に追いついている
}

void Test_Timeline_MainShowTimeout()
{
    // メインスレッドが詰まってメイン窓を出せない: mainShownAt が来なくても transReady + timeout で退場を始める
    sp::Timeline tl;
    const double finish = 1.5;
    const double transReady = finish + tl.params().ReadyDur();
    bool sawTransBefore = false, sawTransAfter = false;
    for (double t = 0.0; t < transReady + 3.0; t += 1.0 / 60.0)
    {
        const auto f = tl.Step(t, 0.5, finish, -1.0);
        if (f.phase == sp::Phase::Transition)
        {
            if (t < transReady + tl.params().mainShowTimeout - 1e-9) sawTransBefore = true; else sawTransAfter = true;
        }
    }
    CHECK(!sawTransBefore);   // メイン窓が出るまで、退場は始めない
    CHECK(sawTransAfter);
}

void Test_Timeline_Deterministic()
{
    sp::Timeline a, b;
    for (double t = 0.0; t < 5.0; t += 1.0 / 60.0)
    {
        const auto fa = a.Step(t, Ramp(t), 3.3, t > 4.0 ? 4.0 : -1.0);
        const auto fb = b.Step(t, Ramp(t), 3.3, t > 4.0 ? 4.0 : -1.0);
        CHECK(fa.progress == fb.progress && fa.logoScaleX == fb.logoScaleX && fa.windowAlpha == fb.windowAlpha
              && fa.phase == fb.phase && fa.lapAngle == fb.lapAngle);
    }
}

// ------------------------------------------------------------ 文言 / Tips

void Test_TextSwapper()
{
    sp::TextSwapper sw(0.25);
    sw.Set(L"A", 0.0);
    CHECK(sw.Query(0.0).u == 1.0);              // 最初の文言はフェードせず出す
    sw.Set(L"B", 1.0);
    auto s = sw.Query(1.0);
    CHECK(s.cur == L"B" && s.prev == L"A" && s.u == 0.0);
    s = sw.Query(1.125);
    CHECK_NEAR(s.u, 0.5, 1e-9);
    s = sw.Query(1.3);
    CHECK(s.u == 1.0 && s.prev.empty());
    sw.Set(L"B", 2.0);                          // 同じ文言ではやり直さない
    CHECK(sw.Query(2.0).u == 1.0);
    // 連続で変わっても、直前の文言が prev になる
    sw.Set(L"C", 3.0); sw.Set(L"D", 3.05);
    s = sw.Query(3.1);
    CHECK(s.cur == L"D" && s.prev == L"C");
}

void Test_TipRotator()
{
    const int n = static_cast<int>(kSplashTipCount);
    CHECK(n >= 20);
    sp::TipRotator r(n, 12345u);
    CHECK(r.period() == 3.5);
    const int start = r.Query(0.0).index;
    CHECK(start == r.start());
    // 3.5 秒ごとに 1 つ進み、順番に一周する
    for (int k = 0; k < n + 3; ++k)
        CHECK(r.Query(3.5 * k + 1.0).index == (start + k) % n);
    // フェード: 切替の瞬間は 0、中ほどは 1、切替直前は 0 へ
    CHECK_NEAR(r.Query(3.5).alpha, 0.0, 1e-9);
    CHECK_NEAR(r.Query(1.75).alpha, 1.0, 1e-9);
    CHECK(r.Query(3.5 - 0.05).alpha < 0.6);
    CHECK(r.Query(3.5 + 0.05).alpha < 0.6);
    // alpha は [0,1]
    for (double t = 0.0; t < 20.0; t += 0.01) { const auto s = r.Query(t); CHECK(s.alpha >= 0.0 && s.alpha <= 1.0); }
    // 種が違えば開始位置が散る（ランダム開始）
    std::set<int> starts;
    for (uint32_t s = 0; s < 200; ++s) starts.insert(sp::TipRotator(n, s).start());
    CHECK(starts.size() >= static_cast<size_t>(n / 2));
    // Tips が 1 個でも壊れない / 0 個でも落ちない
    CHECK(sp::TipRotator(1, 7u).Query(10.0).index == 0);
    CHECK(sp::TipRotator(0, 7u).Query(10.0).index == 0);
}

// ------------------------------------------------------------ 演出監督（文言のクロスフェード / Tips / ready の一言）

void Test_Director()
{
    sp::DirectorConfig cfg;
    cfg.tips = { L"tip-a", L"tip-b", L"tip-c", L"tip-d" };
    cfg.tipSeed = 99;
    cfg.base.title = L"Uno Engine";
    cfg.readyLabels = sp::DefaultReadyLabels();
    sp::Director d(cfg);
    const auto labels = sp::DefaultReadyLabels();

    // 文言: 変わった瞬間にクロスフェードが始まり、0.25 秒で終わる
    d.Step(0.0, 0.1, L"A", 1, 14, -1.0, -1.0);
    CHECK(d.content().stepCur == L"A" && d.content().stepU == 1.0);
    d.Step(1.0, 0.2, L"B", 2, 14, -1.0, -1.0);
    CHECK(d.content().stepCur == L"B" && d.content().stepPrev == L"A" && d.content().stepU == 0.0);
    d.Step(1.3, 0.2, L"B", 2, 14, -1.0, -1.0);
    CHECK(d.content().stepU == 1.0 && d.content().stepPrev.empty());
    CHECK(d.content().stepIndex == 2 && d.content().stepTotal == 14);

    // Tips: 登場の後に出て、3.5 秒ごとに次へ。alpha は [0,1]
    std::set<std::wstring> seen;
    for (double t = 1.3; t < 20.0; t += 1.0 / 60.0)
    {
        d.Step(t, 0.2, L"B", 2, 14, -1.0, -1.0);
        seen.insert(d.content().tip);
        CHECK(d.content().tipAlpha >= 0.0 && d.content().tipAlpha <= 1.0);
    }
    CHECK(seen.size() == 4);   // 20 秒で 4 本すべてを一巡（3.5 秒 × 5 回）

    // ready: 文言が ready の一言に替わり、手順は「n / n」になる
    sp::Director r(cfg);
    for (double t = 0.0; t < 3.0; t += 1.0 / 60.0) r.Step(t, 0.5, L"Working", 5, 14, t >= 1.5 ? 1.5 : -1.0, -1.0);
    CHECK(std::find(labels.begin(), labels.end(), r.content().stepCur) != labels.end());
    CHECK(r.content().stepIndex == 14 && r.content().stepTotal == 14);

    // ready の一言は種で変わる（毎回同じではない）が、同じ種なら同じ（プレビューが決定論）
    std::set<std::wstring> picked;
    for (uint32_t seed = 0; seed < 60; ++seed)
    {
        sp::DirectorConfig c2 = cfg; c2.tipSeed = seed;
        sp::Director a(c2), b(c2);
        for (double t = 0.0; t < 3.0; t += 1.0 / 30.0) { a.Step(t, 0.5, L"x", 1, 2, 1.0, -1.0); b.Step(t, 0.5, L"x", 1, 2, 1.0, -1.0); }
        CHECK(a.content().stepCur == b.content().stepCur);
        picked.insert(a.content().stepCur);
    }
    CHECK(picked.size() >= 3);

    // プロジェクトを開くスプラッシュ: 一言は固定（読み込み後にプロジェクトが開く）
    sp::DirectorConfig pc = cfg; pc.projectMode = true; pc.readyLabels.clear(); pc.readyLabel = L"プロジェクトを開きます";
    sp::Director p(pc);
    for (double t = 0.0; t < 3.0; t += 1.0 / 30.0) p.Step(t, 0.5, L"x", 1, 2, 1.0, -1.0);
    CHECK(p.content().stepCur == L"プロジェクトを開きます" && p.content().projectMode);
}

// ------------------------------------------------------------ 進捗

// 段階 s の先頭までの重みの累計（計画から計算する）。
double CumBefore(const std::vector<sp::StageWeight>& plan, sp::Stage s)
{
    double c = 0.0;
    for (const auto& w : plan) { if (w.stage == s) return c; c += w.weight; }
    return c;
}
double WeightOf(const std::vector<sp::StageWeight>& plan, sp::Stage s)
{
    for (const auto& w : plan) if (w.stage == s) return w.weight;
    return 0.0;
}

void Test_Progress_Weights()
{
    const auto plan = sp::StartupPlan(false);
    sp::ProgressTracker t;
    t.Configure(plan);
    double sum = 0.0;
    for (const auto& s : plan) { sum += s.weight; CHECK(s.weight > 0.0 && s.creepTau > 0.0); }
    CHECK_NEAR(sum, 1.0, 1e-9);
    // 段階の先頭 = それまでの重みの累計 / 総和（段階内の報告 0 → 微進み前）
    t.Begin(sp::Stage::UpdateCheck, 0.0);
    CHECK_NEAR(t.Value(0.0), 0.0, 1e-9);
    t.Begin(sp::Stage::Window, 1.0);
    t.SetSub(0.0);
    CHECK_NEAR(t.Value(1.0), CumBefore(plan, sp::Stage::Window), 1e-9);
    t.Begin(sp::Stage::Graphics, 1.1);
    t.SetSub(0.0);
    CHECK_NEAR(t.Value(1.1), CumBefore(plan, sp::Stage::Graphics), 1e-9);
    // 段階内の進み具合 0.5 → 半分の重みだけ進む
    t.Begin(sp::Stage::Shaders, 2.0);
    t.SetSub(0.5);
    CHECK_NEAR(t.Value(2.0), CumBefore(plan, sp::Stage::Shaders) + WeightOf(plan, sp::Stage::Shaders) * 0.5, 1e-9);
    // 段階を飛ばしても後退しない（Thumbnails を飛ばして EnvMap へ）
    t.Begin(sp::Stage::EnvMap, 3.0);
    t.SetSub(0.0);
    const double v = t.Value(3.0);
    CHECK_NEAR(v, CumBefore(plan, sp::Stage::EnvMap), 1e-9);
    CHECK_NEAR(v, 1.0 - WeightOf(plan, sp::Stage::EnvMap) - WeightOf(plan, sp::Stage::Finalize), 1e-9);
    // 逆戻りの Begin は無視される
    CHECK(!t.Begin(sp::Stage::Window, 3.5));
    CHECK(t.Value(3.5) >= v);
    // 計画に無い段階（起動プランにプロジェクト段階）は無視
    CHECK(!t.Begin(sp::Stage::ProjectAssets, 3.6));
    CHECK(t.StepTotal() == 14);
    CHECK(t.StepIndex() == static_cast<int>(sp::Stage::EnvMap) + 1);
    // 段階の並びは実行順（enum の順）で、重い段階（Renderer）が最大
    double mx = 0.0; sp::Stage mxs = sp::Stage::Window;
    for (const auto& w : plan) if (w.weight > mx) { mx = w.weight; mxs = w.stage; }
    CHECK(mxs == sp::Stage::Renderer);
}

void Test_Progress_CreepAndCap()
{
    sp::ProgressTracker t;
    t.Configure(sp::StartupPlan(false));
    t.Begin(sp::Stage::UpdateCheck, 0.0);   // 更新確認: 報告なし。WinHTTP で数秒かかりうる
    double prev = 0.0;
    for (double now = 0.0; now < 30.0; now += 0.1)
    {
        const double v = t.Value(now);
        CHECK(v >= prev - 1e-12);                          // 単調
        CHECK(v < WeightOf(sp::StartupPlan(false), sp::Stage::UpdateCheck) * 0.9 + 1e-9);   // 段階の 90% までしか進まない（＝100% に達しない）
        prev = v;
    }
    {
        sp::ProgressTracker c;                             // 微進みの様子（別のトラッカーで、時刻を進める順に読む）
        c.Configure(sp::StartupPlan(false));
        c.Begin(sp::Stage::UpdateCheck, 0.0);
        const double v1 = c.Value(1.0), v25 = c.Value(2.5);
        CHECK(v1 > 0.0);                                   // 1 秒後には微進みしている（止まって見えない）
        CHECK(v25 > v1);
    }
    // 明示の報告があれば、それが微進みより優先（sub=1 で段階完了）
    t.SetSub(1.0);
    CHECK_NEAR(t.Value(31.0), WeightOf(sp::StartupPlan(false), sp::Stage::UpdateCheck), 1e-9);
    // SetProgress（絶対値）
    sp::ProgressTracker a;
    a.Configure(sp::StartupPlan(false));
    a.Begin(sp::Stage::Window, 0.0);
    a.SetAbsolute(0.6);
    CHECK_NEAR(a.Value(0.0), 0.6, 1e-9);
    a.SetAbsolute(1.5);                                    // 上限は kMaxProgress（100% は演出が出す）
    CHECK(a.Value(0.1) <= sp::ProgressTracker::kMaxProgress + 1e-12);
    // 最後の段階の完了でも 1.0 には届かない
    sp::ProgressTracker z;
    z.Configure(sp::StartupPlan(false));
    z.Begin(sp::Stage::Finalize, 0.0); z.SetSub(1.0);
    CHECK(z.Value(0.0) <= sp::ProgressTracker::kMaxProgress + 1e-12);
    CHECK(z.Value(0.0) > 0.99);
}

void Test_Progress_ProjectPlans()
{
    // --project 直開き: 起動の続きにプロジェクトの段階が並ぶ
    const auto p = sp::StartupPlan(true);
    CHECK(p.size() == sp::StartupPlan(false).size() + 4);
    sp::ProgressTracker t; t.Configure(p);
    t.Begin(sp::Stage::Finalize, 0.0); t.SetSub(1.0);
    const double v0 = t.Value(0.0);
    CHECK(v0 < 0.6);   // 起動の終わりでも、プロジェクト読込ぶんが残っている
    t.Begin(sp::Stage::ProjectAssets, 1.0); t.SetSub(0.5);
    CHECK(t.Value(1.0) > v0);
    // ランチャーから開く単独: 新規のときだけ作成段階が付く
    CHECK(sp::ProjectLoadPlan(true).size() == sp::ProjectLoadPlan(false).size() + 1);
    sp::ProgressTracker u; u.Configure(sp::ProjectLoadPlan(false));
    u.Begin(sp::Stage::ProjectScene, 0.0); u.SetSub(0.0);
    CHECK_NEAR(u.Value(0.0), 0.0, 1e-9);
    u.Begin(sp::Stage::ProjectAssets, 0.5); u.SetSub(0.0);
    CHECK_NEAR(u.Value(0.5), 0.10 / 0.90, 1e-9);
    u.SetSub(0.5);
    CHECK_NEAR(u.Value(0.5), (0.10 + 0.55 * 0.5) / 0.90, 1e-9);
}

// ------------------------------------------------------------ startup.json

void Test_StartupStats()
{
    using namespace sp;
    CHECK_NEAR(ClampStartupMs(0.0), kStartupDefaultMs, 1e-9);
    CHECK_NEAR(ClampStartupMs(-5.0), kStartupDefaultMs, 1e-9);
    CHECK_NEAR(ClampStartupMs(std::nan("")), kStartupDefaultMs, 1e-9);
    CHECK_NEAR(ClampStartupMs(100.0), 1500.0, 1e-9);
    CHECK_NEAR(ClampStartupMs(99999.0), 6000.0, 1e-9);
    CHECK_NEAR(ClampStartupMs(2345.0), 2345.0, 1e-9);
    // 読み込み: 正常 / 壊れている / 値が変 / 空
    CHECK_NEAR(ParseStartupJson("{\"startupMs\": 3123.4}"), 3123.4, 1e-9);
    CHECK_NEAR(ParseStartupJson("{ \"startupMs\":2000 }"), 2000.0, 1e-9);
    CHECK_NEAR(ParseStartupJson("{\"startupMs\": 100}"), 1500.0, 1e-9);        // 下限へ
    CHECK_NEAR(ParseStartupJson("{\"startupMs\": 90000}"), 6000.0, 1e-9);      // 上限へ
    CHECK_NEAR(ParseStartupJson(""), kStartupDefaultMs, 1e-9);
    CHECK_NEAR(ParseStartupJson("garbage{{{"), kStartupDefaultMs, 1e-9);
    CHECK_NEAR(ParseStartupJson("{\"startupMs\": \"abc\"}"), kStartupDefaultMs, 1e-9);
    CHECK_NEAR(ParseStartupJson("{\"startupMs\": -12}"), kStartupDefaultMs, 1e-9);
    CHECK_NEAR(ParseStartupJson("{\"startupMs\": nan}"), kStartupDefaultMs, 1e-9);
    CHECK_NEAR(ParseStartupJson("{\"other\": 1}"), kStartupDefaultMs, 1e-9);
    // 指数移動平均（新しい実測の重み 0.4）
    CHECK_NEAR(UpdateStartupEma(3000.0, 5000.0), 3800.0, 1e-6);
    CHECK_NEAR(UpdateStartupEma(3000.0, 2000.0), 2600.0, 1e-6);
    CHECK_NEAR(UpdateStartupEma(0.0, 4200.0), 4200.0, 1e-9);                   // 初回は実測をそのまま
    CHECK_NEAR(UpdateStartupEma(3000.0, 100.0), 3000.0 + (1500.0 - 3000.0) * 0.4, 1e-6);   // 実測は丸めてから混ぜる
    CHECK_NEAR(UpdateStartupEma(3000.0, 1e9), 3000.0 + (6000.0 - 3000.0) * 0.4, 1e-6);
    // 何度でも範囲内・収束する
    double e = 6000.0;
    for (int i = 0; i < 40; ++i) e = UpdateStartupEma(e, 2000.0);
    CHECK_NEAR(e, 2000.0, 1.0);
    // 書き出し → 読み込みの往復
    CHECK_NEAR(ParseStartupJson(FormatStartupJson(3456.7)), 3456.7, 0.06);
    CHECK_NEAR(ParseStartupJson(FormatStartupJson(50.0)), 1500.0, 1e-9);
}
} // namespace

int main()
{
    Test_Easing();
    Test_Spring();
    Test_Sparks();
    Test_Timeline_Intro();
    Test_Timeline_Ready_OnTime();
    Test_Timeline_Ready_Early();
    Test_Timeline_Ready_Late();
    Test_Timeline_LateStaysLoading();
    Test_Timeline_ProgressMonotonic();
    Test_Timeline_MainShowTimeout();
    Test_Timeline_Deterministic();
    Test_TextSwapper();
    Test_TipRotator();
    Test_Director();
    Test_Progress_Weights();
    Test_Progress_CreepAndCap();
    Test_Progress_ProjectPlans();
    Test_StartupStats();
    if (g_fail == 0) std::printf("splash motion tests: OK\n");
    else std::printf("splash motion tests: %d failure(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
