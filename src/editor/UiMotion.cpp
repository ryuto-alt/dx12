// ui::anim の実装（W1-A4）。UiMotion.h を参照。
#include "editor/UiMotion.h"

#include "gui/UiClock.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace dx12e::ui::anim
{
namespace mt = motion_tokens;

namespace
{
constexpr float kSpringSettleOmegaT = 7.0f;   // Ease(Spring) の 0..1 を ωt = 0..7（残り 0.7%）に写す

// ---- cubic-bezier（P0=(0,0) P3=(1,1)）----
float BezX(const mt::CubicBezier& b, float s)
{
    const float u = 1.0f - s;
    return 3.0f * u * u * s * b.x1 + 3.0f * u * s * s * b.x2 + s * s * s;
}
float BezY(const mt::CubicBezier& b, float s)
{
    const float u = 1.0f - s;
    return 3.0f * u * u * s * b.y1 + 3.0f * u * s * s * b.y2 + s * s * s;
}
float BezDX(const mt::CubicBezier& b, float s)
{
    const float u = 1.0f - s;
    return 3.0f * u * u * b.x1 + 6.0f * u * s * (b.x2 - b.x1) + 3.0f * s * s * (1.0f - b.x2);
}

float SolveBezier(const mt::CubicBezier& b, float x)
{
    // x(s) は x1,x2 が [0,1] なら単調。ニュートン法（外れたら二分法）。
    float s = x;
    for (int i = 0; i < 8; ++i)
    {
        const float err = BezX(b, s) - x;
        if (std::fabs(err) < 1e-6f) return BezY(b, s);
        const float d = BezDX(b, s);
        if (std::fabs(d) < 1e-6f) break;
        s -= err / d;
        if (s < 0.0f || s > 1.0f) break;
    }
    float lo = 0.0f, hi = 1.0f;
    s = x;
    for (int i = 0; i < 48; ++i)
    {
        const float v = BezX(b, s);
        if (std::fabs(v - x) < 1e-7f) break;
        if (v < x) lo = s; else hi = s;
        s = 0.5f * (lo + hi);
    }
    return BezY(b, s);
}

float Spring01(float t)   // 0..1 -> 0..1（t=1 でちょうど 1。単調）
{
    const auto raw = [](float x) { return 1.0f - (1.0f + x) * std::exp(-x); };
    return raw(kSpringSettleOmegaT * t) / raw(kSpringSettleOmegaT);
}

// ---- 状態 ----
struct State
{
    float from[4]{};
    float target[4]{};
    float cur[4]{};
    double start = 0.0;
    uint64_t lastFrame = 0;
    Curve curve = Curve::Standard;
    float durMs = 0.0f;
    bool  active = false;
};

std::unordered_map<ImGuiID, State>& Store() { static std::unordered_map<ImGuiID, State> s; return s; }
uint64_t g_lastPurge = 0;
constexpr uint64_t kPurgeEveryFrames = 600;
constexpr uint64_t kExpireFrames     = 600;   // これだけ参照されなければ捨てる

std::atomic<bool> g_background{false};
std::atomic<bool> g_virtual{false};
std::atomic<int>  g_reduceOverride{-1};
std::atomic<bool> g_toolUnderlineHosted{false};

// Reduce Motion は「動きの時間を 0 にする」（トークン kReduceMotionDurationMs）。0 以外に変わったら短縮の実装が要る。
static_assert(mt::kReduceMotionDurationMs == 0.0f, "Reduce Motion = 即時。トークンが 0 でなくなったら Instant() を見直す");

void Purge()
{
    const uint64_t now = clock::FrameIndex();
    if (now - g_lastPurge < kPurgeEveryFrames) return;
    g_lastPurge = now;
    auto& m = Store();
    for (auto it = m.begin(); it != m.end();)
    {
        if (now - it->second.lastFrame > kExpireFrames) it = m.erase(it); else ++it;
    }
}

float Eval(const State& s, double now)
{
    const double dtMs = (now - s.start) * 1000.0;
    if (s.durMs <= 0.0f || dtMs >= static_cast<double>(s.durMs)) return 1.0f;
    if (dtMs <= 0.0) return 0.0f;
    return Ease(s.curve, static_cast<float>(dtMs / static_cast<double>(s.durMs)));
}

// n 成分（1 か 4）を 1 つのタイムラインで動かす。戻りは cur。
const float* Step(ImGuiID id, const float* target, int n, float durMs, Curve curve, float snapEps)
{
    Purge();
    State& s = Store()[id];
    const uint64_t frame = clock::FrameIndex();
    const double now = clock::Now();

    // Reduce Motion でも Stepped の刻みは残す。決定論 / background / 仮想入力は常に即時。
    const bool instant = (curve == Curve::Stepped)
        ? (clock::Deterministic() || g_background.load() || g_virtual.load())
        : Instant();

    if (!s.active)
    {
        for (int i = 0; i < n; ++i) { s.from[i] = s.cur[i] = s.target[i] = target[i]; }
        s.active = true;
        s.start  = now;
        s.durMs  = 0.0f;
        s.curve  = curve;
        s.lastFrame = frame;
        return s.cur;
    }
    s.lastFrame = frame;

    bool changed = false;
    for (int i = 0; i < n; ++i)
        if (std::fabs(target[i] - s.target[i]) > 1e-6f) { changed = true; break; }
    if (changed)
    {
        for (int i = 0; i < n; ++i) { s.from[i] = s.cur[i]; s.target[i] = target[i]; }
        s.start = now;
        s.durMs = durMs;
        s.curve = curve;
    }
    if (instant || s.durMs <= 0.0f)
    {
        for (int i = 0; i < n; ++i) { s.from[i] = s.cur[i] = s.target[i]; }
        s.durMs = 0.0f;
        return s.cur;
    }

    if (s.curve == Curve::Spring)
    {
        // 臨界減衰ばね。行き先が変わったら現在値から再開（速度は引き継がない）。
        const float tSec = static_cast<float>(std::max(0.0, now - s.start));
        float worst = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            s.cur[i] = SpringValue(s.from[i], s.target[i], tSec);
            worst = std::max(worst, std::fabs(s.target[i] - s.cur[i]));
        }
        // ばねは durMs で打ち切らない（臨界減衰 ω=14 は約 500ms かかる。打ち切ると途中で跳ぶ）
        if (worst < snapEps)
            for (int i = 0; i < n; ++i) s.cur[i] = s.from[i] = s.target[i];
        return s.cur;
    }

    const float k = Eval(s, now);
    float worst = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        s.cur[i] = s.from[i] + (s.target[i] - s.from[i]) * k;
        worst = std::max(worst, std::fabs(s.target[i] - s.cur[i]));
    }
    // Stepped は階段の途中でスナップしない（snap で段が潰れる）。終点でだけ確定する。
    if (k >= 1.0f || (s.curve != Curve::Stepped && worst < snapEps))
        for (int i = 0; i < n; ++i) s.cur[i] = s.from[i] = s.target[i];
    return s.cur;
}
} // namespace

float Ease(Curve c, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    float y = t;
    switch (c)
    {
    case Curve::Standard: y = SolveBezier(mt::kStandard, t); break;
    case Curve::Exit:     y = SolveBezier(mt::kExit, t);     break;
    case Curve::Expo:     y = SolveBezier(mt::kExpo, t);     break;
    case Curve::Linear:   y = t;                             break;
    case Curve::Spring:   y = Spring01(t);                   break;
    case Curve::Stepped:
    {
        const float n = static_cast<float>(mt::kSteppedSteps);
        y = (t >= 1.0f) ? 1.0f : std::floor(t * n) / n;
        break;
    }
    }
    return std::clamp(y, 0.0f, 1.0f);
}

float SpringValue(float x0, float target, float tSec)
{
    const float w = mt::kSpringOmega;
    const float t = std::max(0.0f, tSec);
    return target + (x0 - target) * (1.0f + w * t) * std::exp(-w * t);
}

float Animate(ImGuiID id, float target, float durMs, Curve curve, float snapEps)
{
    return *Step(id, &target, 1, durMs, curve, snapEps);
}

float AnimatePx(ImGuiID id, float targetPx, float durMs, Curve curve)
{
    return *Step(id, &targetPx, 1, durMs, curve, mt::kSnapPx);
}

ImVec4 AnimateColor(ImGuiID id, ImVec4 target, float durMs, Curve curve)
{
    const float t[4] = { target.x, target.y, target.z, target.w };
    const float* r = Step(id, t, 4, durMs, curve, kSnapColor);
    return ImVec4(r[0], r[1], r[2], r[3]);
}

void SetBackground(bool on)         { g_background.store(on); }
void SetToolUnderlineHosted(bool on){ g_toolUnderlineHosted.store(on); }
bool ToolUnderlineHosted()          { return g_toolUnderlineHosted.load(); }
void SetVirtualInput(bool on)       { g_virtual.store(on); }
void SetReduceMotionOverride(int v) { g_reduceOverride.store(v); }

bool ReduceMotion()
{
    const int o = g_reduceOverride.load();
    if (o >= 0) return o != 0;
#ifdef _WIN32
    // 毎フレームは問い合わせない（約 1 秒に 1 回）。
    static std::atomic<ULONGLONG> lastTick{0};
    static std::atomic<bool> cached{false};
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = lastTick.load();
    if (last == 0 || now - last > 1000)
    {
        BOOL enabled = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
            cached.store(enabled == FALSE);
        lastTick.store(now);
    }
    return cached.load();
#else
    return false;
#endif
}

bool Instant()
{
    return clock::Deterministic() || g_background.load() || g_virtual.load() || ReduceMotion();
}

int  StateCount() { return static_cast<int>(Store().size()); }
void Reset()      { Store().clear(); g_lastPurge = 0; }

} // namespace dx12e::ui::anim
