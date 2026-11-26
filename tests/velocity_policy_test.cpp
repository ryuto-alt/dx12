// FU-E-23: 速度プリパスを要求する理由が 1 つでもあれば、前ワールドの追跡 / 前ワールド VB の確保が要る。
// RT 影デノイザだけのフレーム（TAA / SSR / SSGI が OFF）でも true になること（ここが false だと、速度パスが個別ドローへ落ちて
// フォワードの自動インスタンシングと Z がずれ、大きな面が欠ける）。
#include "renderer/VelocityPolicy.h"

#include <cstdio>

using dx12e::VelocityPrepassReasons;
using dx12e::WantsVelocityPrevWorld;
using dx12e::WantsVelocityPrepass;
using dx12e::PrepareFrameVelocityInputs;
using dx12e::MakePrepareFrameVelocityReasons;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main()
{
    CHECK(!WantsVelocityPrevWorld({}));
    { VelocityPrepassReasons r; r.taa = true;             CHECK(WantsVelocityPrevWorld(r)); }
    { VelocityPrepassReasons r; r.ssr = true;             CHECK(WantsVelocityPrevWorld(r)); }
    { VelocityPrepassReasons r; r.ssgi = true;            CHECK(WantsVelocityPrevWorld(r)); }
    { VelocityPrepassReasons r; r.rtShadowDenoise = true; CHECK(WantsVelocityPrevWorld(r)); }   // FU-E-23 の本命
    { VelocityPrepassReasons r; r.rtAo = true;            CHECK(WantsVelocityPrevWorld(r)); }
    { VelocityPrepassReasons r; r.rtReflections = true;   CHECK(WantsVelocityPrevWorld(r)); }
    { VelocityPrepassReasons r; r.stage = true;           CHECK(WantsVelocityPrepass(r)); }
    // PrevWorld は Prepass と同じ判定（同じ理由集合なら必ず一致する。ずれると速度パスが個別ドローへ落ちて大きな面が欠ける）。
    for (int m = 0; m < 128; ++m)
    {
        VelocityPrepassReasons r;
        r.taa = m & 1; r.ssr = m & 2; r.ssgi = m & 4; r.rtShadowDenoise = m & 8; r.rtAo = m & 16; r.rtReflections = m & 32; r.stage = m & 64;
        CHECK(WantsVelocityPrevWorld(r) == WantsVelocityPrepass(r));
        CHECK(WantsVelocityPrepass(r) == (m != 0));
    }
    // PrepareFrame の組み立て（今回の不具合は、ここで RT の理由を詰め漏らしたこと）。
    auto want = [](const PrepareFrameVelocityInputs& in) { return WantsVelocityPrevWorld(MakePrepareFrameVelocityReasons(in)); };
    CHECK(!want({}));
    { PrepareFrameVelocityInputs i; i.rtSceneReady = true; i.taaPassExists = true; i.rtShadowEnabled = true; i.rtShadowDenoise = true;
      CHECK(want(i)); }                                   // RT 影デノイザだけ（TAA/SSR/SSGI OFF）= FU-E-23 の再現条件
    { PrepareFrameVelocityInputs i; i.rtSceneReady = true; i.taaPassExists = true; i.rtShadowEnabled = true; i.rtShadowDenoise = false;
      CHECK(!want(i)); }                                  // デノイザ OFF なら要らない（既定の経路を変えない）
    { PrepareFrameVelocityInputs i; i.rtSceneReady = true; i.rtShadowEnabled = true; i.rtShadowDenoise = true;
      CHECK(!want(i)); }                                  // m_taaPass が無ければ RenderView も走らせない
    { PrepareFrameVelocityInputs i; i.rtSceneReady = false; i.rtShadowEnabled = true; i.rtShadowDenoise = true; i.taaPassExists = true;
      CHECK(!want(i)); }                                  // RT シーン未準備
    { PrepareFrameVelocityInputs i; i.rtSceneReady = true; i.rtAoEnabled = true;      CHECK(want(i)); }   // 2D / 正射でも追跡する
    { PrepareFrameVelocityInputs i; i.rtSceneReady = true; i.rtReflEnabled = true;    CHECK(want(i)); }
    { PrepareFrameVelocityInputs i; i.ssGiWantsPrepass = true; i.ssrEnabled = true;   CHECK(want(i)); }
    { PrepareFrameVelocityInputs i; i.ssGiWantsPrepass = true; i.ssgiEnabled = true;  CHECK(want(i)); }
    { PrepareFrameVelocityInputs i; i.ssrEnabled = true; i.ssgiEnabled = true;        CHECK(!want(i)); }  // 画面空間 GI が無ければ要らない
    { PrepareFrameVelocityInputs i; i.taaActive = true;                               CHECK(want(i)); }
    static_assert(!WantsVelocityPrevWorld({}), "理由が無ければ追跡しない（既定の経路を変えない）");
    if (g_fail == 0) std::printf("velocity_policy_test: OK\n");
    return g_fail == 0 ? 0 : 1;
}
