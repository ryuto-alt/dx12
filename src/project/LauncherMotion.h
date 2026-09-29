#pragma once

// ===========================================================================
// プロジェクトランチャーの「動き」の純ロジック（ヘッダオンリー・std だけ。ImGui / Win32 に依存しない）
// ---------------------------------------------------------------------------
// ・Smoother  : ホバー / フォーカスの補間。指数（半減期指定）で目標へ寄る。dt に依らず同じ軌跡（フレームレート非依存）。
//               linear は使わない（docs/UI_STYLE_GUIDE.md）。アニメ OFF のときは即座に目標値へ。
// ・SpringCD  : 臨界減衰スプリング（行き過ぎない）。ナビのインジケータ（選択の帯）の移動に使う。
// ・Reveal    : 画面入場のスタッガー（要素ごとに少しずつ遅らせて、下からふわっと出る）。
// ・ParticleField : 背景のうっすら漂う粒。決定論（同じシード → 同じ軌跡）で、数十個・ImDrawList で描ける軽さ。
//
// 単位: 時間は秒、座標は論理 px。
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dx12e::launcher
{

inline float Clamp01f(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
inline float Lerpf(float a, float b, float t) { return a + (b - a) * t; }

inline float EaseOutCubicf(float t) { t = Clamp01f(t); const float u = 1.0f - t; return 1.0f - u * u * u; }
inline float EaseOutQuintf(float t) { t = Clamp01f(t); const float u = 1.0f - t; return 1.0f - u * u * u * u * u; }
inline float EaseInOutSinef(float t) { t = Clamp01f(t); return 0.5f - 0.5f * std::cos(3.14159265f * t); }

// 指数スムージング。halfLife 秒で目標までの距離が半分になる。dt を分割しても同じ軌跡（積が保存される）。
inline float SmoothTo(float cur, float target, float dt, float halfLife)
{
    if (dt <= 0.0f) return cur;
    if (halfLife <= 0.0f) return target;
    const float k = 1.0f - std::pow(0.5f, dt / halfLife);
    return cur + (target - cur) * k;
}

// 臨界減衰スプリング（ω = 角周波数）。閉形式なので dt が大きくても安定・行き過ぎなし。
struct SpringCD
{
    float x = 0.0f, v = 0.0f;
    void Snap(float value) { x = value; v = 0.0f; }
    void Step(float target, float omega, float dt)
    {
        if (dt <= 0.0f) return;
        const float d = x - target;
        const float e = std::exp(-omega * dt);
        const float j = v + omega * d;
        x = target + (d + j * dt) * e;
        v = (v - omega * j * dt) * e;
    }
};

// 画面入場のスタッガー。要素 index は delayPer 秒ずつ遅れて始まり、duration 秒で 0→1（out cubic）。
inline float RevealAmount(float tSinceEnter, int index, float delayPer, float duration)
{
    const float t = (tSinceEnter - delayPer * static_cast<float>(index)) / (duration > 1e-4f ? duration : 1e-4f);
    return EaseOutCubicf(t);
}

// 決定論の乱数（0..1）。
inline uint32_t LHash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline float LRand01(uint32_t seed, uint32_t i)
{
    return static_cast<float>(LHash32(seed * 0x9E3779B1U + i * 0x85EBCA6BU + 0x27D4EB2FU) >> 8) / 16777216.0f;
}

// 背景の粒。画面（w × h）の中をゆっくり上へ漂い、端で反対側へ回り込む。
struct ParticleField
{
    struct P
    {
        float x = 0, y = 0;       // 位置（論理 px）
        float vx = 0, vy = 0;     // 速度（px/s）
        float size = 1.0f;        // 半径 px
        float phase = 0.0f;       // またたきの位相
        float rate = 1.0f;        // またたきの速さ
        float depth = 0.0f;       // 0=遠い 1=近い（大きさ・明るさ・視差に使う）
    };
    std::vector<P> p;
    float w = 0, h = 0;
    float time = 0;

    void Reset(uint32_t seed, int count, float width, float height)
    {
        w = width; h = height; time = 0;
        p.assign(static_cast<size_t>(count > 0 ? count : 0), P{});
        for (size_t i = 0; i < p.size(); ++i)
        {
            const uint32_t k = static_cast<uint32_t>(i) * 7u;
            P& q = p[i];
            q.depth = LRand01(seed, k + 0);
            q.x = LRand01(seed, k + 1) * w;
            q.y = LRand01(seed, k + 2) * h;
            q.vx = (LRand01(seed, k + 3) - 0.5f) * 6.0f;
            q.vy = -(4.0f + LRand01(seed, k + 4) * 10.0f) * (0.5f + q.depth);
            q.size = 0.6f + q.depth * 1.5f;
            q.phase = LRand01(seed, k + 5) * 6.2831853f;
            q.rate = 0.4f + LRand01(seed, k + 6) * 1.1f;
        }
    }

    // 画面サイズが変わったら位置を比で持ち直す。
    void Resize(float width, float height)
    {
        if (width <= 0 || height <= 0 || (width == w && height == h)) return;
        const float sx = (w > 0) ? width / w : 1.0f, sy = (h > 0) ? height / h : 1.0f;
        for (auto& q : p) { q.x *= sx; q.y *= sy; }
        w = width; h = height;
    }

    void Update(float dt)
    {
        if (dt <= 0.0f || w <= 0.0f || h <= 0.0f) return;
        if (dt > 0.1f) dt = 0.1f;     // ウィンドウを掴んで止めた後などの飛びを抑える
        time += dt;
        for (auto& q : p)
        {
            q.x += q.vx * dt;
            q.y += q.vy * dt;
            if (q.y < -4.0f)  { q.y += h + 8.0f; }
            if (q.y > h + 4.0f) { q.y -= h + 8.0f; }
            if (q.x < -4.0f)  { q.x += w + 8.0f; }
            if (q.x > w + 4.0f) { q.x -= w + 8.0f; }
        }
    }

    // またたき（0.25..1）。
    float Twinkle(const P& q) const { return 0.625f + 0.375f * std::sin(time * q.rate + q.phase); }
};

}  // namespace dx12e::launcher
