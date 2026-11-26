// ui::anim（src/editor/UiMotion.*）の検査（W1-A4 / GG-3）。ImGui のコンテキストも GPU も要らない（ui::clock の決定論モードで時間を進める）。
//   ・Ease: 単調 / [0,1] の外へ出ない（オーバーシュート 0）/ 端点 / cubic-bezier が独立に求めた参照値と 1e-3 以内
//   ・standard は 40% の時間で 90% 以上に達する
//   ・Spring: 臨界減衰の閉形式と一致、オーバーシュート 0
//   ・Animate: 時間で進む / retarget は現在値から / 決定論では 1 フレームで終点（Instant）/ Reduce Motion の Stepped 例外
//   ・状態の掃除（参照されない id は捨てる）
#include "editor/UiMotion.h"
#include "gui/UiClock.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <initializer_list>
#include <cstdio>

using namespace dx12e;
namespace anim = dx12e::ui::anim;
namespace mt   = dx12e::ui::motion_tokens;

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECKF(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// 独立の参照実装: ベジェを細かい s でサンプルし、x に最も近い点の y を返す（二分探索や Newton を使わない）。
double RefBezier(const mt::CubicBezier& b, double x)
{
    double best = 1e9, y = 0.0;
    const int N = 200000;
    for (int i = 0; i <= N; ++i)
    {
        const double s = static_cast<double>(i) / N, u = 1.0 - s;
        const double bx = 3 * u * u * s * b.x1 + 3 * u * s * s * b.x2 + s * s * s;
        const double by = 3 * u * u * s * b.y1 + 3 * u * s * s * b.y2 + s * s * s;
        const double d = std::fabs(bx - x);
        if (d < best) { best = d; y = by; }
    }
    return y;
}

void TestEase()
{
    const anim::Curve all[] = { anim::Curve::Standard, anim::Curve::Exit, anim::Curve::Expo, anim::Curve::Linear,
                                anim::Curve::Spring, anim::Curve::Stepped };
    for (anim::Curve c : all)
    {
        CHECK(anim::Ease(c, 0.0f) == 0.0f);
        CHECK(anim::Ease(c, 1.0f) == 1.0f);
        CHECK(anim::Ease(c, -3.0f) == 0.0f);   // 範囲外は丸める
        CHECK(anim::Ease(c, 7.0f) == 1.0f);
        float prev = 0.0f;
        for (int i = 0; i <= 1000; ++i)
        {
            const float y = anim::Ease(c, static_cast<float>(i) / 1000.0f);
            CHECKF(y >= prev - 1e-6f, "monotone curve=%d i=%d y=%f prev=%f", static_cast<int>(c), i, y, prev);
            CHECKF(y >= 0.0f && y <= 1.0f, "overshoot curve=%d i=%d y=%f", static_cast<int>(c), i, y);   // オーバーシュート 0
            prev = y;
        }
    }
    // cubic-bezier の値（CSS の cubic-bezier(x1,y1,x2,y2) と同じ。独立の参照実装と 1e-3 以内）
    const struct { anim::Curve c; mt::CubicBezier b; } beziers[] = {
        { anim::Curve::Standard, mt::kStandard }, { anim::Curve::Exit, mt::kExit },
        { anim::Curve::Expo, mt::kExpo },         { anim::Curve::Linear, mt::kLinear } };
    for (const auto& e : beziers)
        for (int i = 1; i < 20; ++i)
        {
            const double x = i / 20.0;
            const double ref = RefBezier(e.b, x);
            const double got = anim::Ease(e.c, static_cast<float>(x));
            CHECKF(std::fabs(ref - got) < 1e-3, "bezier curve=%d x=%.2f ref=%.5f got=%.5f", static_cast<int>(e.c), x, ref, got);
        }
    // standard は 40% の時間で 90% に達する
    CHECKF(anim::Ease(anim::Curve::Standard, 0.4f) >= 0.9f, "standard@0.4=%f", anim::Ease(anim::Curve::Standard, 0.4f));
    // Stepped は kSteppedSteps 段（値の種類が段数 + 1）
    {
        float last = -1.0f; int kinds = 0;
        for (int i = 0; i <= 1200; ++i)
        {
            const float y = anim::Ease(anim::Curve::Stepped, static_cast<float>(i) / 1200.0f);
            if (y != last) { ++kinds; last = y; }
        }
        CHECKF(kinds == mt::kSteppedSteps + 1, "stepped kinds=%d", kinds);
    }
}

// CSS の cubic-bezier(x1,y1,x2,y2)（仕様の文字列から独立に二分法で解いた値。x = 0.25 / 0.5 / 0.75）。
// トークンの転記ミス（x1/y1 の取り違えなど）を、トークン自身を使う参照実装では拾えないので、固定値で縛る。
void TestCssReference()
{
    struct R { anim::Curve c; float y25, y50, y75; };
    const R refs[] = {
        { anim::Curve::Standard, 0.76728f, 0.94608f, 0.99111f },   // cubic-bezier(0.2, 0.8, 0.2, 1)
        { anim::Curve::Exit,     0.09863f, 0.32481f, 0.63009f },   // cubic-bezier(0.4, 0, 1, 1)
        { anim::Curve::Expo,     0.82562f, 0.97178f, 0.99768f },   // cubic-bezier(0.16, 1, 0.3, 1)
    };
    for (const R& r : refs)
    {
        CHECKF(std::fabs(anim::Ease(r.c, 0.25f) - r.y25) < 1e-3f, "css ref curve=%d x=.25 got=%f", static_cast<int>(r.c), anim::Ease(r.c, 0.25f));
        CHECKF(std::fabs(anim::Ease(r.c, 0.50f) - r.y50) < 1e-3f, "css ref curve=%d x=.50 got=%f", static_cast<int>(r.c), anim::Ease(r.c, 0.50f));
        CHECKF(std::fabs(anim::Ease(r.c, 0.75f) - r.y75) < 1e-3f, "css ref curve=%d x=.75 got=%f", static_cast<int>(r.c), anim::Ease(r.c, 0.75f));
    }
}

void TestSpring()
{
    // 閉形式: x(t) = target + (x0 - target)(1 + wt)e^{-wt}
    const float w = mt::kSpringOmega;
    for (float t : { 0.0f, 0.05f, 0.1f, 0.3f, 1.0f })
    {
        const float expect = 10.0f + (0.0f - 10.0f) * (1.0f + w * t) * std::exp(-w * t);
        CHECK(std::fabs(anim::SpringValue(0.0f, 10.0f, t) - expect) < 1e-5f);
    }
    // オーバーシュート 0（単調に近づく）
    float prev = 0.0f;
    for (int i = 0; i <= 200; ++i)
    {
        const float v = anim::SpringValue(0.0f, 1.0f, i * 0.01f);
        CHECK(v >= prev - 1e-6f && v <= 1.0f);
        prev = v;
    }
}

// 決定論モードで n フレーム進める。
void Frames(int n) { for (int i = 0; i < n; ++i) ui::clock::Advance(); }

void TestAnimateInstant()
{
    anim::Reset();
    anim::SetReduceMotionOverride(0);
    ui::clock::SetDeterministic(true);
    CHECK(anim::Instant());
    CHECK(anim::Animate(1, 0.0f, 120.0f) == 0.0f);
    Frames(1);
    CHECK(anim::Animate(1, 1.0f, 120.0f) == 1.0f);   // 決定論は 1 フレームで終点
    CHECK(anim::AnimateColor(2, ImVec4(0, 0, 0, 1), 120.0f).x == 0.0f);
    Frames(1);
    const ImVec4 c = anim::AnimateColor(2, ImVec4(1, 0.5f, 0.25f, 1), 120.0f);
    CHECK(c.x == 1.0f && c.y == 0.5f && c.z == 0.25f && c.w == 1.0f);
    ui::clock::SetDeterministic(false);
}

// 実時間の経路（ImGui の時計を手で進める。NewFrame は呼ばない）。
void SetTime(double t) { ImGui::GetCurrentContext()->Time = t; }

void TestAnimateTimed()
{
    ImGuiContext* ctx = ImGui::CreateContext();
    anim::Reset();
    anim::SetReduceMotionOverride(0);
    anim::SetBackground(false);
    anim::SetVirtualInput(false);
    ui::clock::SetDeterministic(false);
    CHECK(!anim::Instant());

    SetTime(10.0);
    CHECK(anim::Animate(77, 0.0f, 120.0f) == 0.0f);              // 初回は行き先そのまま
    SetTime(10.1);
    const float a0 = anim::Animate(77, 1.0f, 100.0f);             // 行き先が変わった瞬間は現在値のまま
    CHECK(a0 == 0.0f);
    SetTime(10.15);                                               // 50ms / 100ms
    const float a1 = anim::Animate(77, 1.0f, 100.0f);
    CHECKF(std::fabs(a1 - anim::Ease(anim::Curve::Standard, 0.5f)) < 1e-4f, "a1=%f", a1);
    CHECK(a1 > 0.0f && a1 < 1.0f);
    // retarget: 途中で戻すと現在値から 0 へ向かう（飛ばない）
    const float mid = a1;
    SetTime(10.16);
    const float r0 = anim::Animate(77, 0.0f, 100.0f);
    CHECKF(std::fabs(r0 - mid) < 1e-4f, "retarget must start from current value: r0=%f mid=%f", r0, mid);
    SetTime(10.26);
    CHECK(anim::Animate(77, 0.0f, 100.0f) == 0.0f);               // 終点
    // 同じ時刻に何度呼んでも同じ値（冪等）
    SetTime(11.0);
    anim::Animate(78, 0.0f, 100.0f);
    SetTime(11.01);
    const float p = anim::Animate(78, 1.0f, 100.0f);
    SetTime(11.05);
    const float q1 = anim::Animate(78, 1.0f, 100.0f);
    const float q2 = anim::Animate(78, 1.0f, 100.0f);
    CHECK(q1 == q2 && p <= q1);
    // px の値は 0.5px でスナップする
    SetTime(12.0);
    anim::AnimatePx(79, 0.0f, 160.0f);
    SetTime(12.01);
    anim::AnimatePx(79, 100.0f, 160.0f);
    SetTime(12.159);
    const float px = anim::AnimatePx(79, 100.0f, 160.0f);
    CHECKF(px >= 99.5f, "px=%f (remaining under 0.5px snaps)", px);
    // 色: 4 成分が同じ時間で動く
    SetTime(13.0);
    anim::AnimateColor(80, ImVec4(0, 0, 0, 0), 100.0f);
    SetTime(13.01);
    anim::AnimateColor(80, ImVec4(1, 1, 1, 1), 100.0f);
    SetTime(13.06);
    const ImVec4 c = anim::AnimateColor(80, ImVec4(1, 1, 1, 1), 100.0f);
    CHECK(c.x > 0.0f && c.x < 1.0f && c.x == c.y && c.y == c.z && c.z == c.w);
    // Spring: 臨界減衰。単調に近づき、オーバーシュートしない
    SetTime(14.0);
    anim::Animate(81, 0.0f, 600.0f, anim::Curve::Spring);
    SetTime(14.01);
    anim::Animate(81, 1.0f, 600.0f, anim::Curve::Spring);
    float prev = 0.0f;
    for (int i = 1; i <= 60; ++i)
    {
        SetTime(14.01 + i * 0.01);
        const float v = anim::Animate(81, 1.0f, 600.0f, anim::Curve::Spring);
        CHECK(v >= prev - 1e-6f && v <= 1.0f);
        prev = v;
    }
    CHECK(prev == 1.0f);
    // Reduce Motion: 通常の曲線は即時、Stepped は刻みが残る
    anim::SetReduceMotionOverride(1);
    SetTime(15.0);
    anim::Animate(82, 0.0f, 100.0f);
    anim::Animate(83, 0.0f, 600.0f, anim::Curve::Stepped);
    SetTime(15.01);
    CHECK(anim::Animate(82, 1.0f, 100.0f) == 1.0f);
    anim::Animate(83, 1.0f, 600.0f, anim::Curve::Stepped);
    SetTime(15.31);                                              // 300ms / 600ms = 0.5 -> 6/12 段
    const float st = anim::Animate(83, 1.0f, 600.0f, anim::Curve::Stepped);
    CHECKF(std::fabs(st - 0.5f) < 1e-4f, "stepped under reduce motion st=%f", st);
    anim::SetReduceMotionOverride(0);   // 直前の Reduce Motion の検査を解除
    // Spring は durMs で打ち切らない: 120ms の指定でも 120ms で終点へ跳ばず、単調に近づいて 1 に着く
    SetTime(16.0);
    anim::Animate(84, 0.0f, 120.0f, anim::Curve::Spring);
    SetTime(16.01);
    anim::Animate(84, 1.0f, 120.0f, anim::Curve::Spring);
    SetTime(16.01 + 0.13);                                       // 指定時間を 10ms 超えた時点
    const float s130 = anim::Animate(84, 1.0f, 120.0f, anim::Curve::Spring);
    CHECKF(s130 > 0.3f && s130 < 0.9f, "spring must not jump at durMs: %f", s130);
    prev = s130;
    for (int i = 1; i <= 80; ++i)
    {
        SetTime(16.14 + i * 0.01);
        const float v = anim::Animate(84, 1.0f, 120.0f, anim::Curve::Spring);
        CHECK(v >= prev - 1e-6f && v <= 1.0f);
        prev = v;
    }
    CHECK(prev == 1.0f);
    anim::SetReduceMotionOverride(-1);
    ImGui::DestroyContext(ctx);
}

void TestReduceMotionStepped()
{
    // Reduce Motion ON: 通常の曲線は即時。Stepped は動く（刻みは残す）。決定論は切る必要があるので、時計は
    // 決定論のままにして Instant を Reduce Motion だけで判定できる別経路を使う: 決定論 OFF では ImGui の時計が要るため、
    // ここでは Instant() の真偽だけを検査する。
    ui::clock::SetDeterministic(false);
    anim::SetBackground(false);
    anim::SetVirtualInput(false);
    anim::SetReduceMotionOverride(1);
    CHECK(anim::ReduceMotion());
    CHECK(anim::Instant());
    anim::SetReduceMotionOverride(0);
    CHECK(!anim::ReduceMotion());
    CHECK(!anim::Instant());
    anim::SetBackground(true);
    CHECK(anim::Instant());
    anim::SetBackground(false);
    anim::SetVirtualInput(true);
    CHECK(anim::Instant());
    anim::SetVirtualInput(false);
    CHECK(!anim::Instant());
    anim::SetReduceMotionOverride(-1);
}

void TestPurge()
{
    anim::Reset();
    ui::clock::SetDeterministic(true);
    for (ImGuiID i = 1; i <= 100; ++i) anim::Animate(i, 1.0f, 100.0f);
    CHECK(anim::StateCount() == 100);
    Frames(1500);                          // 掃除の周期と期限を超えて、別の id だけを参照し続ける
    for (int f = 0; f < 5; ++f) { Frames(1); anim::Animate(5000, 1.0f, 100.0f); }
    CHECKF(anim::StateCount() <= 2, "state count=%d (expired ids must be dropped)", anim::StateCount());
    ui::clock::SetDeterministic(false);
}
// deco::Ease（Graphite）の slot → 時間の対応。選択中のツール（slot 3）は kToolUnderline、押下（slot 1）は kPress / kRelease。
void TestDecoSlots()
{
    using anim::dur::kPress; using anim::dur::kRelease; using anim::dur::kHover; using anim::dur::kUnfocusDim; using anim::dur::kToolUnderline;
    CHECK(anim::DecoSlotDur(0, true).ms == kHover.ms && anim::DecoSlotDur(0, false).ms == kRelease.ms);
    CHECK(anim::DecoSlotDur(1, true).ms == kPress.ms && anim::DecoSlotDur(1, false).ms == kRelease.ms);
    CHECK(anim::DecoSlotDur(2, true).ms == kUnfocusDim.ms);
    CHECK(anim::DecoSlotDur(3, true).ms == kToolUnderline.ms && anim::DecoSlotDur(3, false).ms == kToolUnderline.ms);
    CHECK(kToolUnderline.ms != kPress.ms);
}
} // namespace

int main()
{
    TestEase();
    TestCssReference();
    TestSpring();
    TestAnimateInstant();
    TestAnimateTimed();
    TestReduceMotionStepped();
    TestDecoSlots();
    TestPurge();
    if (g_fail == 0) std::printf("UiMotionTests: ALL PASS\n");
    return g_fail == 0 ? 0 : 1;
}
