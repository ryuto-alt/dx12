// SpriteAnim.h（Sprite2D フリップブック/UVスクロールの UV 計算）の単体テスト。
// ヘッダオンリー・依存ゼロ（GPU/entt/DirectXMath 不要）なのでスタンドアロン構成でも動く。
#include "renderer/SpriteAnim.h"

#include <cmath>
#include <cstdio>

using namespace dx12e;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) < eps;
}

// 横1行ストリップ（cols=0 → frames と同じ）: 4フレーム、8fps
static void TestFlipbookStrip()
{
    // t=0 → frame 0: [0, 0.25] x [0, 1]
    SpriteUvRect r = ComputeFlipbookUv(4, 8.0f, 0, 0, 0, 0.0f);
    CHECK(Near(r.u0, 0.0f) && Near(r.u1, 0.25f));
    CHECK(Near(r.v0, 0.0f) && Near(r.v1, 1.0f));

    // t=0.125(1フレーム目) → frame 1: [0.25, 0.5]
    r = ComputeFlipbookUv(4, 8.0f, 0, 0, 0, 0.125f);
    CHECK(Near(r.u0, 0.25f) && Near(r.u1, 0.5f));

    // t=0.5(4フレーム目) → ループして frame 0 に戻る
    r = ComputeFlipbookUv(4, 8.0f, 0, 0, 0, 0.5f);
    CHECK(Near(r.u0, 0.0f) && Near(r.u1, 0.25f));

    // 負の時刻でも [0, frames) に折り返す（t=-0.125 → frame 3）
    r = ComputeFlipbookUv(4, 8.0f, 0, 0, 0, -0.125f);
    CHECK(Near(r.u0, 0.75f) && Near(r.u1, 1.0f));
}

// 4x4 グリッド 16 フレーム（cols=4 → 総行数は自動で 4）
static void TestFlipbookGrid()
{
    // frame 5 = 行1・列1: u [0.25, 0.5], v [0.25, 0.5]
    SpriteUvRect r = ComputeFlipbookUv(16, 10.0f, 4, 0, 0, 0.5f);
    CHECK(Near(r.u0, 0.25f) && Near(r.u1, 0.5f));
    CHECK(Near(r.v0, 0.25f) && Near(r.v1, 0.5f));

    // frame 15 = 行3・列3
    r = ComputeFlipbookUv(16, 10.0f, 4, 0, 0, 1.5f);
    CHECK(Near(r.u0, 0.75f) && Near(r.u1, 1.0f));
    CHECK(Near(r.v0, 0.75f) && Near(r.v1, 1.0f));
}

// 行指定シート: 4行のシートで animRow=2 の 3 フレームアニメ（animRows 明示）
static void TestFlipbookRowSelect()
{
    // frame 0 → 列0・行2: v [0.5, 0.75]（行高 = 1/4）
    SpriteUvRect r = ComputeFlipbookUv(3, 8.0f, 3, 2, 4, 0.0f);
    CHECK(Near(r.u0, 0.0f) && Near(r.u1, 1.0f / 3.0f));
    CHECK(Near(r.v0, 0.5f) && Near(r.v1, 0.75f));

    // animRows=0（自動）の場合は animRow + ceil(3/3) = 3 行とみなす → v [2/3, 1]
    r = ComputeFlipbookUv(3, 8.0f, 3, 2, 0, 0.0f);
    CHECK(Near(r.v0, 2.0f / 3.0f) && Near(r.v1, 1.0f));
}

// fps<=0 は既定 8fps 扱い
static void TestFlipbookDefaultFps()
{
    SpriteUvRect r = ComputeFlipbookUv(4, 0.0f, 0, 0, 0, 0.125f);  // 8fps なら frame 1
    CHECK(Near(r.u0, 0.25f) && Near(r.u1, 0.5f));
}

// 再生モード: once は最終フレームで停止、pingpong は 0..n-1..0 で往復
static void TestFlipbookModes()
{
    // once: 4フレーム 8fps。t=0/0.125/0.375 は素直に進み、t>=0.5 は frame 3 で止まる
    CHECK(ComputeFlipbookFrame(4, 8.0f, kFlipbookOnce, 0.0f) == 0);
    CHECK(ComputeFlipbookFrame(4, 8.0f, kFlipbookOnce, 0.375f) == 3);
    CHECK(ComputeFlipbookFrame(4, 8.0f, kFlipbookOnce, 0.5f) == 3);
    CHECK(ComputeFlipbookFrame(4, 8.0f, kFlipbookOnce, 100.0f) == 3);
    CHECK(ComputeFlipbookFrame(4, 8.0f, kFlipbookOnce, -1.0f) == 0);

    // once の終了判定
    CHECK(!IsFlipbookFinished(4, 8.0f, kFlipbookOnce, 0.375f));
    CHECK(IsFlipbookFinished(4, 8.0f, kFlipbookOnce, 0.5f));
    CHECK(!IsFlipbookFinished(4, 8.0f, kFlipbookLoop, 100.0f));   // ループは永遠に終わらない

    // pingpong: 4フレームなら周期 6 → 0,1,2,3,2,1, 0,1,..
    const int expect[7] = {0, 1, 2, 3, 2, 1, 0};
    for (int i = 0; i < 7; ++i)
        CHECK(ComputeFlipbookFrame(4, 8.0f, kFlipbookPingPong, i * 0.125f) == expect[i]);

    // pingpong で 1 フレームしかない場合は常に frame 0（ゼロ除算/負周期にしない）
    CHECK(ComputeFlipbookFrame(1, 8.0f, kFlipbookPingPong, 3.0f) == 0);

    // ループ既定は従来どおり
    CHECK(ComputeFlipbookFrame(4, 8.0f, kFlipbookLoop, 0.5f) == 0);

    // Ex 版の UV は frame → セル変換が同じ（pingpong で frame 2 = 列2）
    SpriteUvRect r = ComputeFlipbookUvEx(4, 8.0f, 0, 0, 0, kFlipbookPingPong, 0.5f);
    CHECK(Near(r.u0, 0.5f) && Near(r.u1, 0.75f));
}

// UVスクロール: min/max 双方に同じオフセット（矩形サイズ不変）、[0,1) 折り返し
static void TestScroll()
{
    // 0.5 units/s * 0.5s = +0.25
    SpriteUvRect r = ComputeScrollUv(0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0.0f, 0.5f);
    CHECK(Near(r.u0, 0.25f) && Near(r.u1, 1.25f));
    CHECK(Near(r.v0, 0.0f) && Near(r.v1, 1.0f));

    // 長時間: 10.25 → 折り返して +0.25（矩形サイズは維持）
    r = ComputeScrollUv(0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 0.0f, 10.25f);
    CHECK(Near(r.u0, 0.25f) && Near(r.u1, 0.75f));
    CHECK(Near(r.v0, 0.5f) && Near(r.v1, 1.0f));

    // 負方向スクロールも [0,1) に折り返す（-0.25 → +0.75）
    r = ComputeScrollUv(0.0f, 0.0f, 1.0f, 1.0f, -0.5f, 0.0f, 0.5f);
    CHECK(Near(r.u0, 0.75f) && Near(r.u1, 1.75f));

    // V 方向
    r = ComputeScrollUv(0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 2.0f, 0.25f);
    CHECK(Near(r.v0, 0.5f) && Near(r.v1, 1.5f));
    CHECK(Near(r.u0, 0.0f) && Near(r.u1, 1.0f));
}

int main()
{
    TestFlipbookStrip();
    TestFlipbookGrid();
    TestFlipbookRowSelect();
    TestFlipbookDefaultFps();
    TestFlipbookModes();
    TestScroll();

    std::printf("SpriteAnimTests: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
