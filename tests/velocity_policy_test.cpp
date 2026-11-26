// FU-E-23: 速度プリパスを要求する理由が 1 つでもあれば、前ワールドの追跡 / 前ワールド VB の確保が要る。
// RT 影デノイザだけのフレーム（TAA / SSR / SSGI が OFF）でも true になること（ここが false だと、速度パスが個別ドローへ落ちて
// フォワードの自動インスタンシングと Z がずれ、大きな面が欠ける）。
#include "renderer/VelocityPolicy.h"

#include <cstdio>

using dx12e::VelocityPrepassReasons;
using dx12e::WantsVelocityPrevWorld;
using dx12e::WantsVelocityPrepass;

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
    static_assert(!WantsVelocityPrevWorld({}), "理由が無ければ追跡しない（既定の経路を変えない）");
    if (g_fail == 0) std::printf("velocity_policy_test: OK\n");
    return g_fail == 0 ? 0 : 1;
}
