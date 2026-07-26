// 速度バッファ（モーションベクター）と TAA ジッタの数式テスト。
//
// 守りたい不変条件は 2 つだけだが、どちらも壊すと症状が見えにくい:
//   (A) ジッタは投影行列への後乗算で「NDC の平行移動」になる（透視でも正射でも）。
//   (B) 何も動いていないフレームでは、ジッタが入っていても速度がぴったり 0 になる。
//       ここが崩れると TAA の履歴が毎フレーム別の場所を読み、収束が壊れる。
//
// GPU 不要（純粋な DirectXMath + ヘッダオンリーの TaaJitter.h）。
// 実行: ctest --output-on-failure

#include "renderer/TaaJitter.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdio>

using namespace dx12e;
using namespace DirectX;

namespace
{
int g_failures = 0;
int g_checks   = 0;

bool feq(float a, float b, float tol = 1e-5f)
{
    return std::fabs(a - b) <= tol * (1.0f + std::fabs(a) + std::fabs(b));
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

#define CHECK_F(a, b) CHECK(feq((a), (b)))

// ジッタありの viewProj を作る（Application::Render と同じ式）。
static XMMATRIX MakeJitteredVP(XMMATRIX view, XMMATRIX proj, TaaJitterNdc j)
{
    return XMMatrixMultiply(view,
        XMMatrixMultiply(proj, XMMatrixTranslation(j.x, j.y, 0.0f)));
}

// クリップ座標 → ビューポートローカル UV（VelocityCommon.hlsli と同じ規約）。
static XMFLOAT2 ClipToUv(XMVECTOR clip)
{
    const float w = XMVectorGetW(clip);
    const float x = XMVectorGetX(clip) / w;
    const float y = XMVectorGetY(clip) / w;
    return { x * 0.5f + 0.5f, y * -0.5f + 0.5f };
}

// ---------------------------------------------------------------------------
// (1) ハルトン列
// ---------------------------------------------------------------------------
static void Test_Halton()
{
    // base 2: 1/2, 1/4, 3/4, 1/8, 5/8, 3/8, 7/8
    CHECK_F(HaltonRadicalInverse(1, 2), 0.5f);
    CHECK_F(HaltonRadicalInverse(2, 2), 0.25f);
    CHECK_F(HaltonRadicalInverse(3, 2), 0.75f);
    CHECK_F(HaltonRadicalInverse(4, 2), 0.125f);
    CHECK_F(HaltonRadicalInverse(5, 2), 0.625f);
    // base 3: 1/3, 2/3, 1/9, 4/9, 7/9
    CHECK_F(HaltonRadicalInverse(1, 3), 1.0f / 3.0f);
    CHECK_F(HaltonRadicalInverse(2, 3), 2.0f / 3.0f);
    CHECK_F(HaltonRadicalInverse(3, 3), 1.0f / 9.0f);
    CHECK_F(HaltonRadicalInverse(4, 3), 4.0f / 9.0f);
    // 0 は 0（"+1" インデックスで踏まないようにしている値）
    CHECK_F(HaltonRadicalInverse(0, 2), 0.0f);
}

// ---------------------------------------------------------------------------
// (2) ジッタ量が ±0.5px を超えないこと
// ---------------------------------------------------------------------------
static void Test_JitterRange()
{
    constexpr u32 kW = 1920, kH = 1080;
    for (u32 i = 0; i < 64; ++i)
    {
        const TaaJitterNdc j = ComputeTaaJitterNdc(i, kW, kH);
        // ±0.5px = ±1/vp（NDC は幅 2.0 に vp ピクセル）
        CHECK(std::fabs(j.x) <= 1.0f / static_cast<float>(kW) + 1e-7f);
        CHECK(std::fabs(j.y) <= 1.0f / static_cast<float>(kH) + 1e-7f);
    }
    // スケール 0 なら完全に無ジッタ
    const TaaJitterNdc z = ComputeTaaJitterNdc(3, kW, kH, 0.0f);
    CHECK_F(z.x, 0.0f);
    CHECK_F(z.y, 0.0f);
    // vp=0 でもゼロ除算しない
    const TaaJitterNdc s = ComputeTaaJitterNdc(1, 0, 0);
    CHECK(std::isfinite(s.x) && std::isfinite(s.y));
}

// ---------------------------------------------------------------------------
// (3) ジッタは NDC の平行移動になる（透視・正射のどちらでも）
// ---------------------------------------------------------------------------
static void Test_JitterIsNdcTranslation()
{
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 2, -6, 1),
                                           XMVectorSet(0, 0, 0, 1),
                                           XMVectorSet(0, 1, 0, 0));
    const TaaJitterNdc j = ComputeTaaJitterNdc(5, 1920, 1080);
    const XMVECTOR p = XMVectorSet(1.3f, -0.7f, 2.1f, 1.0f);

    for (int mode = 0; mode < 2; ++mode)
    {
        const XMMATRIX proj = (mode == 0)
            ? XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.0f / 9.0f, 0.1f, 1000.0f)
            : XMMatrixOrthographicLH(20.0f, 11.25f, 0.1f, 1000.0f);

        const XMVECTOR clip  = XMVector4Transform(p, XMMatrixMultiply(view, proj));
        const XMVECTOR clipJ = XMVector4Transform(p, MakeJitteredVP(view, proj, j));

        const float w  = XMVectorGetW(clip);
        const float wJ = XMVectorGetW(clipJ);
        // w は不変（平行移動は z/w 行に触らない）→ 深度も不変
        CHECK_F(w, wJ);
        CHECK_F(XMVectorGetZ(clip), XMVectorGetZ(clipJ));

        // NDC がちょうど (jx, jy) だけずれる
        CHECK_F(XMVectorGetX(clipJ) / wJ - XMVectorGetX(clip) / w, j.x);
        CHECK_F(XMVectorGetY(clipJ) / wJ - XMVectorGetY(clip) / w, j.y);

        // シェーダ側のジッタ除去（clip.xy -= jitter * clip.w）で元に戻る
        CHECK_F(XMVectorGetX(clipJ) - j.x * wJ, XMVectorGetX(clip));
        CHECK_F(XMVectorGetY(clipJ) - j.y * wJ, XMVectorGetY(clip));
    }
}

// ---------------------------------------------------------------------------
// (4) 静止時の速度はぴったり 0（ジッタが入っていても）— 最重要
// ---------------------------------------------------------------------------
static void Test_StaticVelocityIsZero()
{
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(3, 4, -9, 1),
                                           XMVectorSet(0, 1, 0, 1),
                                           XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.0f / 9.0f, 0.1f, 1000.0f);
    const XMMATRIX vp   = XMMatrixMultiply(view, proj);
    const XMMATRIX world = XMMatrixRotationY(0.6f) * XMMatrixTranslation(2.0f, 0.5f, 1.0f);

    // カメラも物体も動いていない: prevWorld == world, prevVP == vp（ジッタなし）
    for (u32 i = 0; i < 8; ++i)
    {
        const TaaJitterNdc j = ComputeTaaJitterNdc(i, 1280, 720);
        const XMMATRIX mvpJ    = XMMatrixMultiply(world, MakeJitteredVP(view, proj, j));
        const XMMATRIX prevMvp = XMMatrixMultiply(world, vp);   // 前フレームは非ジッタ

        const XMVECTOR p = XMVectorSet(0.25f, 1.5f, -0.75f, 1.0f);

        // VS がやること: SV_POSITION はジッタ込み、curClip はジッタを引いたもの
        XMVECTOR clipJ = XMVector4Transform(p, mvpJ);
        const float wJ = XMVectorGetW(clipJ);
        XMVECTOR curClip = XMVectorSetX(clipJ, XMVectorGetX(clipJ) - j.x * wJ);
        curClip          = XMVectorSetY(curClip, XMVectorGetY(clipJ) - j.y * wJ);
        const XMVECTOR prevClip = XMVector4Transform(p, prevMvp);

        const XMFLOAT2 curUv  = ClipToUv(curClip);
        const XMFLOAT2 prevUv = ClipToUv(prevClip);

        // velocity = 現UV - 前UV
        CHECK_F(curUv.x - prevUv.x, 0.0f);
        CHECK_F(curUv.y - prevUv.y, 0.0f);
    }
}

// ---------------------------------------------------------------------------
// (5) 物体が動いたら、期待どおりの NDC 差分が出る（符号規約の固定）
// ---------------------------------------------------------------------------
static void Test_MovingVelocityMatchesReprojection()
{
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -5, 1),
                                           XMVectorSet(0, 0, 0, 1),
                                           XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 1.0f, 0.1f, 100.0f);
    const XMMATRIX vp   = XMMatrixMultiply(view, proj);

    const XMMATRIX prevWorld = XMMatrixTranslation(0.0f, 0.0f, 0.0f);
    const XMMATRIX world     = XMMatrixTranslation(0.5f, 0.0f, 0.0f);   // +X へ移動
    const TaaJitterNdc j     = ComputeTaaJitterNdc(2, 640, 480);

    const XMVECTOR p = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);   // 物体の原点

    XMVECTOR clipJ = XMVector4Transform(p, XMMatrixMultiply(world, MakeJitteredVP(view, proj, j)));
    const float wJ = XMVectorGetW(clipJ);
    XMVECTOR curClip = XMVectorSetX(clipJ, XMVectorGetX(clipJ) - j.x * wJ);
    curClip          = XMVectorSetY(curClip, XMVectorGetY(clipJ) - j.y * wJ);
    const XMVECTOR prevClip = XMVector4Transform(p, XMMatrixMultiply(prevWorld, vp));

    const XMFLOAT2 curUv  = ClipToUv(curClip);
    const XMFLOAT2 prevUv = ClipToUv(prevClip);
    const XMFLOAT2 vel{curUv.x - prevUv.x, curUv.y - prevUv.y};

    // ジッタ抜きの「現フレーム世界」を直接投影したものと一致すること
    const XMFLOAT2 refUv = ClipToUv(XMVector4Transform(p, XMMatrixMultiply(world, vp)));
    CHECK_F(vel.x, refUv.x - prevUv.x);
    CHECK_F(vel.y, refUv.y - prevUv.y);

    // 符号規約: +X へ動いた → 速度 x は正（履歴は uv - velocity から読む＝画面左側）
    CHECK(vel.x > 0.0f);
    CHECK_F(vel.y, 0.0f);

    // 履歴 UV の再構成が前フレーム UV に一致する
    CHECK_F(curUv.x - vel.x, prevUv.x);
    CHECK_F(curUv.y - vel.y, prevUv.y);
}

// ---------------------------------------------------------------------------
// (6) カメラだけが動いた場合も同じ式で成立する
// ---------------------------------------------------------------------------
static void Test_CameraMotionVelocity()
{
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 1.0f, 0.1f, 100.0f);
    const XMMATRIX prevView = XMMatrixLookAtLH(XMVectorSet(0, 0, -5, 1),
                                               XMVectorSet(0, 0, 0, 1),
                                               XMVectorSet(0, 1, 0, 0));
    const XMMATRIX view     = XMMatrixLookAtLH(XMVectorSet(0.3f, 0, -5, 1),
                                               XMVectorSet(0.3f, 0, 0, 1),
                                               XMVectorSet(0, 1, 0, 0));   // 右へ平行移動
    const XMMATRIX prevVp = XMMatrixMultiply(prevView, proj);
    const TaaJitterNdc j  = ComputeTaaJitterNdc(4, 800, 600);

    const XMMATRIX world = XMMatrixIdentity();
    const XMVECTOR p     = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

    XMVECTOR clipJ = XMVector4Transform(p, XMMatrixMultiply(world, MakeJitteredVP(view, proj, j)));
    const float wJ = XMVectorGetW(clipJ);
    XMVECTOR curClip = XMVectorSetX(clipJ, XMVectorGetX(clipJ) - j.x * wJ);
    curClip          = XMVectorSetY(curClip, XMVectorGetY(clipJ) - j.y * wJ);
    const XMVECTOR prevClip = XMVector4Transform(p, XMMatrixMultiply(world, prevVp));

    const XMFLOAT2 curUv  = ClipToUv(curClip);
    const XMFLOAT2 prevUv = ClipToUv(prevClip);

    // カメラが右へ動いた → 物体は画面上で左へ流れる → 速度 x は負
    CHECK(curUv.x - prevUv.x < 0.0f);
    CHECK_F(curUv.y - prevUv.y, 0.0f);
}

int main()
{
    Test_Halton();
    Test_JitterRange();
    Test_JitterIsNdcTranslation();
    Test_StaticVelocityIsZero();
    Test_MovingVelocityMatchesReprojection();
    Test_CameraMotionVelocity();

    std::printf("velocity_math_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
