// b1（PerFrameConstants）の CPU 側の定義（renderer/FrameConstants.h）のレイアウト検証。
// 期待値は shaders/forward/Lighting.hlsli の PerFrameConstants の offset コメントと同じ表。
#include "renderer/FrameConstants.h"

#include <cstddef>
#include <cstdio>

using namespace dx12e;

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
    CHECK(sizeof(FrameConstantsData) == 1600);
    CHECK(offsetof(FrameConstantsData, view) == 0);
    CHECK(offsetof(FrameConstantsData, proj) == 64);
    CHECK(offsetof(FrameConstantsData, lightDir) == 128);
    CHECK(offsetof(FrameConstantsData, lightColor) == 144);
    CHECK(offsetof(FrameConstantsData, cascadeViewProj) == 160);
    CHECK(offsetof(FrameConstantsData, cascadeSplitsView) == 416);
    CHECK(offsetof(FrameConstantsData, shadowParams) == 432);
    CHECK(offsetof(FrameConstantsData, cameraPos) == 448);
    CHECK(offsetof(FrameConstantsData, numPointLights) == 464);
    CHECK(offsetof(FrameConstantsData, clusterParams) == 480);
    CHECK(offsetof(FrameConstantsData, clusterExtra) == 528);
    CHECK(offsetof(FrameConstantsData, pcssParams) == 544);
    CHECK(offsetof(FrameConstantsData, ddgiOrigin) == 560);
    CHECK(offsetof(FrameConstantsData, ddgiCounts) == 592);
    CHECK(offsetof(FrameConstantsData, _clusterReserved) == 608);
    CHECK(offsetof(FrameConstantsData, ddgiC1) == 1168);
    CHECK(offsetof(FrameConstantsData, giParams) == 1216);
    CHECK(offsetof(FrameConstantsData, giParams2) == 1232);
    CHECK(offsetof(FrameConstantsData, spotShadowMatrix) == 1248);
    CHECK(offsetof(FrameConstantsData, iblIntensity) == 1504);
    CHECK(offsetof(FrameConstantsData, contactShadowEnabled) == 1520);
    CHECK(offsetof(FrameConstantsData, normalFilterParams) == 1524);
    CHECK(offsetof(FrameConstantsData, ddgiC2) == 1536);
    CHECK(offsetof(FrameConstantsData, ddgiScroll3) == 1584);

    // PfSlot: _clusterReserved の番号。既存の使い方（植生の風 0/1、コンタクト 8、マルチスキャッタ 9、反射プローブ 34）を固定する。
    CHECK(static_cast<u32>(PfSlot::FoliageWind0) == 0);
    CHECK(static_cast<u32>(PfSlot::FoliageWind1) == 1);
    CHECK(static_cast<u32>(PfSlot::ContactMip) == 8);
    CHECK(static_cast<u32>(PfSlot::MultiScatter) == 9);
    CHECK(static_cast<u32>(PfSlot::ReflectionProbe) == 34);
    FrameConstantsData fc{};
    Pf(fc, PfSlot::ReflectionProbe).x = 2.0f;
    CHECK(&Pf(fc, PfSlot::ReflectionProbe) == &fc._clusterReserved[34]);
    CHECK(&Pf(fc, PfSlot::VfxFrame) == &fc._clusterReserved[10]);
    // 全スロットが配列の中に収まる
    CHECK(static_cast<u32>(PfSlot::ReflectionProbe) < sizeof(fc._clusterReserved) / sizeof(fc._clusterReserved[0]));

    std::printf("FrameConstantsLayoutTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
