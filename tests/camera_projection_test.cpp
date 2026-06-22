// Camera の投影行列（透視 / 正射）の単体テスト（Phase 4: 能力カタログ ortho）。
//
// 正射投影は 2D/2.5D・ボード・RTS俯瞰をエンジン無改造で成立させる中核能力。
// GPU 不要（純粋な DirectXMath の行列計算）なので Camera.cpp を直接ビルドして検証する。
//
// 実行: ctest --output-on-failure

#include "renderer/Camera.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdio>

using namespace dx12e;
using namespace DirectX;

namespace
{
int g_failures = 0;
int g_checks   = 0;

bool feq(float a, float b)
{
    return std::fabs(a - b) <= 1e-4f * (1.0f + std::fabs(a) + std::fabs(b));
}
} // namespace

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
    Camera cam;

    // --- 透視（既定モード） ---
    cam.SetPerspective(XM_PIDIV4, 16.0f / 9.0f, 0.1f, 1000.0f);
    CHECK(!cam.IsOrthographic());
    {
        XMFLOAT4X4 p;
        XMStoreFloat4x4(&p, cam.GetProjectionMatrix());
        // 透視は w = z（_34 = 1, _44 = 0）
        CHECK(feq(p._34, 1.0f));
        CHECK(feq(p._44, 0.0f));
    }

    // --- 正射 ---
    const float vh = 20.0f, aspect = 16.0f / 9.0f, nz = 0.1f, fz = 100.0f;
    cam.SetOrthographic(vh, aspect, nz, fz);
    CHECK(cam.IsOrthographic());
    {
        XMFLOAT4X4 p;
        XMStoreFloat4x4(&p, cam.GetProjectionMatrix());
        const float w = vh * aspect;
        // XMMatrixOrthographicLH(w,h,n,f): _11=2/w, _22=2/h, _33=1/(f-n), _34=0, _44=1
        CHECK(feq(p._11, 2.0f / w));
        CHECK(feq(p._22, 2.0f / vh));
        CHECK(feq(p._33, 1.0f / (fz - nz)));
        CHECK(feq(p._34, 0.0f));
        CHECK(feq(p._44, 1.0f));
    }

    // --- 透視へ戻せる（モードが排他的に切り替わる） ---
    cam.SetPerspective(XM_PIDIV4, aspect, nz, fz);
    CHECK(!cam.IsOrthographic());

    std::printf("camera_projection: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
