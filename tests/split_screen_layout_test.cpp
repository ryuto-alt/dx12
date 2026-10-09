// renderer/SplitScreenLayout.h（画面分割の矩形計算）の単体テスト。ヘッダオンリー。
#include "renderer/SplitScreenLayout.h"

#include <cstdio>

using namespace dx12e;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static bool Overlap(const SplitRect& a, const SplitRect& b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

int main()
{
    const SplitRect full{10, 20, 1280, 720};

    // n<=1: 全体そのまま
    { const SplitRect r = ComputeSplitRect(full, 1, 0); CHECK(r.x == 10 && r.y == 20 && r.w == 1280 && r.h == 720); }

    // n=2: 上下
    {
        const SplitRect a = ComputeSplitRect(full, 2, 0), b = ComputeSplitRect(full, 2, 1);
        CHECK(a.x == 10 && a.w == 1280 && b.x == 10 && b.w == 1280);
        CHECK(a.y == 20);
        CHECK(b.y == a.y + a.h + kSplitGap);
        CHECK(b.y + b.h == full.y + full.h);
        CHECK(!Overlap(a, b));
    }

    // n=4: 2x2、隙間 2px、全体に収まる
    {
        SplitRect r[4];
        for (u32 i = 0; i < 4; ++i) r[i] = ComputeSplitRect(full, 4, i);
        CHECK(r[0].x == 10 && r[0].y == 20);
        CHECK(r[1].x == r[0].x + r[0].w + kSplitGap && r[1].y == r[0].y);
        CHECK(r[2].x == r[0].x && r[2].y == r[0].y + r[0].h + kSplitGap);
        CHECK(r[3].x == r[1].x && r[3].y == r[2].y);
        CHECK(r[3].x + r[3].w == full.x + full.w);
        CHECK(r[3].y + r[3].h == full.y + full.h);
        for (u32 i = 0; i < 4; ++i)
            for (u32 j = i + 1; j < 4; ++j) CHECK(!Overlap(r[i], r[j]));
    }

    // n=3: 1..3 番目は n=4 と同じ配置
    {
        for (u32 i = 0; i < 3; ++i)
        {
            const SplitRect a = ComputeSplitRect(full, 3, i), b = ComputeSplitRect(full, 4, i);
            CHECK(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h);
        }
    }

    // 極小でも w/h >= 1
    for (u32 n = 2; n <= 4; ++n)
        for (u32 i = 0; i < n; ++i)
            for (u32 sz = 0; sz <= 5; ++sz)
            {
                const SplitRect r = ComputeSplitRect(SplitRect{0, 0, sz, sz}, n, i);
                CHECK(r.w >= 1 && r.h >= 1);
            }

    // 要求のクランプ
    {
        SplitScreenRequest q;
        q.SetCount(1); CHECK(q.n == 0);
        q.SetCount(3); CHECK(q.n == 3);
        q.SetCount(9); CHECK(q.n == 4);
        q.SetCount(-2); CHECK(q.n == 0);
    }

    std::printf("split_screen_layout: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
