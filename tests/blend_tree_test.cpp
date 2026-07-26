// ブレンドツリーの重み計算（animation/BlendTree.h）のテスト。
//
// 1D は区間線形補間。2D は Gradient Band Interpolation
// （Rune Skovbo Johansen の修士論文 §6.3 / §6.3.1、Unity の Freeform Cartesian /
//  Freeform Directional の元になった式）。
//
// 一番大事なのは「重みの合計が必ず 1」と「サンプル点ちょうどでその点の重みが 1」
// （論文が sums-to-one / exact と呼ぶ性質）。ここが崩れるとポーズが縮む・暴れる。
//
// ヘッダオンリー・依存ゼロなのでスタンドアロン構成でも動く。
#include "animation/BlendTree.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace dx12e;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) < eps;
}

static float Sum(const float* w, size_t n)
{
    float s = 0.0f;
    for (size_t i = 0; i < n; ++i) s += w[i];
    return s;
}

// ===========================================================================
// 1D
// ===========================================================================

// idle(0) / walk(1.4) / run(4.5) の 3 サンプル
static void Test1DBasic()
{
    const float axis[3] = {0.0f, 1.4f, 4.5f};
    float w[3];

    // サンプル点ちょうど → その点だけ 1（exact）
    Eval1DBlend(axis, 3, 0.0f, w);
    CHECK(Near(w[0], 1.0f) && Near(w[1], 0.0f) && Near(w[2], 0.0f));
    Eval1DBlend(axis, 3, 1.4f, w);
    CHECK(Near(w[0], 0.0f) && Near(w[1], 1.0f) && Near(w[2], 0.0f));
    Eval1DBlend(axis, 3, 4.5f, w);
    CHECK(Near(w[0], 0.0f) && Near(w[1], 0.0f) && Near(w[2], 1.0f));

    // 中間 → 挟む 2 点だけに配分
    Eval1DBlend(axis, 3, 0.7f, w);
    CHECK(Near(w[0], 0.5f) && Near(w[1], 0.5f) && Near(w[2], 0.0f));

    Eval1DBlend(axis, 3, 1.4f + (4.5f - 1.4f) * 0.25f, w);
    CHECK(Near(w[0], 0.0f) && Near(w[1], 0.75f) && Near(w[2], 0.25f));
}

// 重みの合計は常に 1（範囲外・境界を含む）
static void Test1DSumsToOne()
{
    const float axis[4] = {-2.0f, 0.0f, 1.4f, 4.5f};
    float w[4];
    for (int i = -30; i <= 80; ++i)
    {
        const float x = static_cast<float>(i) * 0.1f;
        Eval1DBlend(axis, 4, x, w);
        CHECK(Near(Sum(w, 4), 1.0f, 1e-5f));
        for (int k = 0; k < 4; ++k) CHECK(w[k] >= -1e-6f && w[k] <= 1.0f + 1e-6f);
    }
}

// 範囲外は端でクランプ
static void Test1DClamp()
{
    const float axis[3] = {0.0f, 1.4f, 4.5f};
    float w[3];

    Eval1DBlend(axis, 3, -100.0f, w);
    CHECK(Near(w[0], 1.0f) && Near(Sum(w, 3), 1.0f));

    Eval1DBlend(axis, 3, 999.0f, w);
    CHECK(Near(w[2], 1.0f) && Near(Sum(w, 3), 1.0f));
}

// サンプル 1 個 / 0 個でも落ちない・NaN を返さない
static void Test1DDegenerate()
{
    float w[2] = {123.0f, 123.0f};

    const float one[1] = {3.0f};
    Eval1DBlend(one, 1, -5.0f, w);
    CHECK(Near(w[0], 1.0f));
    Eval1DBlend(one, 1, 5.0f, w);
    CHECK(Near(w[0], 1.0f));

    // 0 個 → 何も書かない（呼び出し側は使わない）
    Eval1DBlend(nullptr, 0, 1.0f, w);
    CHECK(std::isfinite(w[0]));

    // 同じ値のサンプルが並んでいる（不正データ）でも NaN を出さない
    const float dup[3] = {1.0f, 1.0f, 2.0f};
    Eval1DBlend(dup, 3, 1.0f, w);
    CHECK(std::isfinite(w[0]) && std::isfinite(w[1]));
}

// ===========================================================================
// 2D — Cartesian Gradient Band
// ===========================================================================

static void Test2DCartesianExact()
{
    // 中心 + 8 方向（Unity の Freeform Cartesian と同じ配置）
    const BlendPoint2D pts[5] = {{0, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    float w[5];

    // 各サンプル点ちょうどでその点の重みが 1、他は 0（論文の "exact" 性質）
    for (int i = 0; i < 5; ++i)
    {
        GradientBandCartesian(pts, 5, pts[i], w);
        for (int k = 0; k < 5; ++k)
            CHECK(Near(w[k], (k == i) ? 1.0f : 0.0f, 1e-4f));
    }
}

static void Test2DCartesianSumsToOne()
{
    const BlendPoint2D pts[5] = {{0, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    float w[5];

    for (int gy = -15; gy <= 15; ++gy)
        for (int gx = -15; gx <= 15; ++gx)
        {
            BlendPoint2D p{static_cast<float>(gx) * 0.1f, static_cast<float>(gy) * 0.1f};
            GradientBandCartesian(pts, 5, p, w);
            CHECK(Near(Sum(w, 5), 1.0f, 1e-4f));
            for (int k = 0; k < 5; ++k) CHECK(w[k] >= -1e-6f);   // 負にならない（クランプ済み）
        }
}

// 1D の点列を Cartesian に入れると区間線形補間と一致する（論文の性質の確認）
static void Test2DCartesianMatches1D()
{
    const float axis[3] = {0.0f, 1.4f, 4.5f};
    const BlendPoint2D pts[3] = {{0.0f, 0}, {1.4f, 0}, {4.5f, 0}};

    for (int i = 0; i <= 45; ++i)
    {
        const float x = static_cast<float>(i) * 0.1f;
        float w1[3], w2[3];
        Eval1DBlend(axis, 3, x, w1);
        GradientBandCartesian(pts, 3, {x, 0.0f}, w2);
        for (int k = 0; k < 3; ++k) CHECK(Near(w1[k], w2[k], 1e-4f));
    }
}

static void Test2DCartesianDegenerate()
{
    float w[2] = {9.0f, 9.0f};

    const BlendPoint2D one[1] = {{1, 2}};
    GradientBandCartesian(one, 1, {0, 0}, w);
    CHECK(Near(w[0], 1.0f));

    // 完全に同一の 2 点（len2 == 0 の分岐）でも NaN を出さない
    const BlendPoint2D same[2] = {{1, 1}, {1, 1}};
    GradientBandCartesian(same, 2, {5, 5}, w);
    CHECK(std::isfinite(w[0]) && std::isfinite(w[1]));
    CHECK(Near(Sum(w, 2), 1.0f, 1e-4f));
}

// ===========================================================================
// 2D — Polar Gradient Band（alpha = 2 が論文の推奨値）
// ===========================================================================

static void Test2DPolarExact()
{
    const BlendPoint2D pts[4] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    float w[4];

    for (int i = 0; i < 4; ++i)
    {
        GradientBandPolar(pts, 4, pts[i], 2.0f, w);
        for (int k = 0; k < 4; ++k)
            CHECK(Near(w[k], (k == i) ? 1.0f : 0.0f, 1e-3f));
    }
}

// 同じ方向・大きさ違いの 2 点の間では「大きさ」で補間される
static void Test2DPolarMagnitude()
{
    const BlendPoint2D pts[2] = {{1, 0}, {3, 0}};   // 同じ方向（+X）、長さ 1 と 3
    float w[2];

    GradientBandPolar(pts, 2, {2.0f, 0.0f}, 2.0f, w);
    CHECK(Near(Sum(w, 2), 1.0f, 1e-4f));
    CHECK(w[0] > 0.05f && w[1] > 0.05f);   // 両方に配分される

    // 端に寄せると寄せた方が優勢
    GradientBandPolar(pts, 2, {1.1f, 0.0f}, 2.0f, w);
    CHECK(w[0] > w[1]);
    GradientBandPolar(pts, 2, {2.9f, 0.0f}, 2.0f, w);
    CHECK(w[1] > w[0]);
}

// 同じ大きさ・方向違いの 2 点の間では「角度」で補間される
static void Test2DPolarDirection()
{
    const BlendPoint2D pts[2] = {{1, 0}, {0, 1}};   // 同じ長さ 1、90 度違い
    float w[2];

    // ちょうど中間（45 度）で half-half
    const float s = 0.70710678f;
    GradientBandPolar(pts, 2, {s, s}, 2.0f, w);
    CHECK(Near(Sum(w, 2), 1.0f, 1e-4f));
    CHECK(Near(w[0], 0.5f, 1e-3f) && Near(w[1], 0.5f, 1e-3f));

    // +X 寄りなら pts[0] が優勢
    GradientBandPolar(pts, 2, {0.97f, 0.24f}, 2.0f, w);
    CHECK(w[0] > w[1]);
}

static void Test2DPolarSumsToOne()
{
    const BlendPoint2D pts[4] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    float w[4];

    // 一周ぐるっと回して合計が常に 1
    for (int i = 0; i < 72; ++i)
    {
        const float a = static_cast<float>(i) * (6.2831853f / 72.0f);
        const float r = 1.0f + 0.5f * std::sin(a * 3.0f);
        GradientBandPolar(pts, 4, {r * std::cos(a), r * std::sin(a)}, 2.0f, w);
        CHECK(Near(Sum(w, 4), 1.0f, 1e-3f));
        for (int k = 0; k < 4; ++k) CHECK(w[k] >= -1e-6f);
    }
}

static void Test2DPolarDegenerate()
{
    const BlendPoint2D pts[3] = {{0, 0}, {1, 0}, {0, 1}};   // 原点サンプル（長さ 0）を含む
    float w[3];
    GradientBandPolar(pts, 3, {0, 0}, 2.0f, w);
    CHECK(std::isfinite(w[0]) && std::isfinite(w[1]) && std::isfinite(w[2]));
    CHECK(Near(Sum(w, 3), 1.0f, 1e-3f));
}

int main()
{
    Test1DBasic();
    Test1DSumsToOne();
    Test1DClamp();
    Test1DDegenerate();

    Test2DCartesianExact();
    Test2DCartesianSumsToOne();
    Test2DCartesianMatches1D();
    Test2DCartesianDegenerate();

    Test2DPolarExact();
    Test2DPolarMagnitude();
    Test2DPolarDirection();
    Test2DPolarSumsToOne();
    Test2DPolarDegenerate();

    std::printf("BlendTreeTests: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
