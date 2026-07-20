// Frustum（Gribb-Hartmann 視錐台抽出 + 球カリング）の単体テスト。
// ヘッダオンリー・純 DirectXMath（GPU非依存）。実行: ctest --output-on-failure
//
// カリングの正しさは「画面内の点を残す/画面外の点を切る/大きい球は中心が遠くても残す」で担保する。
// 規約ミス（転置忘れ・OpenGL near 規約）だと in/out が反転して全 CHECK が落ちる＝回帰検知になる。

#include "renderer/Frustum.h"

#include <DirectXMath.h>
#include <cstdio>

using namespace dx12e;
using namespace DirectX;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

int main()
{
    // 原点を見下ろす LH 透視カメラ。eye=(0,0,-5) → target=(0,0,0) を +Z 方向に見る。
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -5, 1),
                                           XMVectorSet(0, 0, 0, 1),
                                           XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 1.0f, 0.1f, 100.0f);
    const Frustum f = Frustum::FromViewProj(XMMatrixMultiply(view, proj));

    auto P = [](float x, float y, float z) { return XMVectorSet(x, y, z, 1.0f); };

    // 画面内: カメラ前方 5u の原点 → 見える。
    CHECK(f.SphereVisible(P(0, 0, 0), 0.5f));

    // カメラ背後（z=-50, eye は z=-5）→ near 平面の外 → 切る。
    CHECK(!f.SphereVisible(P(0, 0, -50), 0.5f));

    // far(100) の外（前方 z=+200）→ 切る。
    CHECK(!f.SphereVisible(P(0, 0, 200), 0.5f));

    // 真横に大きく外れる → 切る。
    CHECK(!f.SphereVisible(P(1000, 0, 0), 0.5f));

    // 床/壁の性質: 中心が視錐台外でも半径が十分大きければ交差＝残す（保守的＝偽陰性なし）。
    CHECK(f.SphereVisible(P(1000, 0, 0), 2000.0f));

    // 境界の保守性: 右端のすぐ外側でも半径ぶん食い込めば見える。
    CHECK(f.SphereVisible(P(8, 0, 5), 5.0f));

    std::printf("frustum: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
