// GpuTimer のスコープ表の検査（W05-26）。GPU は使わない（属性表と純関数だけ）。
// 64 ビットのマスク、葉と親の判定、別名の解決、既存 22 本の名前と番号の不変、葉の合計の規則。
#include "graphics/GpuTimer.h"

#include <cstdio>
#include <cstring>

using namespace dx12e;

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
    // ---- 数と上限 ----
    CHECK(GpuTimer::Count == 52);
    CHECK(GpuTimer::kMaxScopes == 64);
    CHECK(GpuTimer::Count <= 64);

    // ---- 64 ビットのマスク: 33 本目以降のビットが落ちない ----
    {
        u64 mask = 0;
        for (u32 s = 0; s < GpuTimer::Count; ++s) mask |= (1ull << s);
        CHECK(__popcnt64(mask) == GpuTimer::Count);
        CHECK((mask & (1ull << (GpuTimer::GameHud))) != 0);
        CHECK(GpuTimer::GameHud >= 32);
    }

    // ---- 既存 22 本の名前と番号が不変（perf_stats を読む MCP を壊さない）----
    static const char* kOld[22] = {
        "total", "shadows", "depthPrepass", "prepassSsao", "clusterCull", "raytracing", "rtScreen", "rtReflection",
        "ddgi", "screenSpaceGi", "volFog", "hiZ", "mainScene", "particles", "postFx", "ui", "vgCull", "clouds",
        "cloudShadow", "rtShadowDenoise", "upscale", "vgShadow" };
    for (u32 i = 0; i < 22; ++i) CHECK(std::strcmp(GpuTimer::Name(i), kOld[i]) == 0);
    CHECK(GpuTimer::VgShadow == 21);
    CHECK(GpuTimer::ShadowCsm == 22);

    // ---- Name と Info の名前が一致、名前が重複しない、全部非空 ----
    for (u32 i = 0; i < GpuTimer::Count; ++i)
    {
        const auto sc = static_cast<GpuTimer::Scope>(i);
        CHECK(std::strcmp(GpuTimer::Name(i), GpuTimer::Info(sc).name) == 0);
        CHECK(GpuTimer::Info(sc).name[0] != '\0');
        for (u32 j = i + 1; j < GpuTimer::Count; ++j)
            CHECK(std::strcmp(GpuTimer::Info(sc).name, GpuTimer::Info(static_cast<GpuTimer::Scope>(j)).name) != 0);
        // 親は自分より前の番号に限らないが、循環しない（Total まで辿れる）。
        u32 cur = i, hops = 0;
        while (cur != GpuTimer::Total && hops++ < GpuTimer::Count) cur = GpuTimer::Info(static_cast<GpuTimer::Scope>(cur)).parent;
        CHECK(cur == GpuTimer::Total);
    }

    // ---- 親子 ----
    CHECK(GpuTimer::Info(GpuTimer::CloudShadow).parent == GpuTimer::PrepassSSAO);
    CHECK(GpuTimer::Info(GpuTimer::Clouds).parent == GpuTimer::Particles);
    CHECK(GpuTimer::Info(GpuTimer::VgShadow).parent == GpuTimer::Shadows);
    CHECK(GpuTimer::Info(GpuTimer::Upscale).parent == GpuTimer::PostFX);
    CHECK(GpuTimer::Info(GpuTimer::RtShadowDenoise).parent == GpuTimer::RtScreen);
    CHECK(GpuTimer::Info(GpuTimer::RtScreen).parent == GpuTimer::PrepassSSAO);
    CHECK(GpuTimer::Info(GpuTimer::Total).parent == GpuTimer::Total);

    // ---- 葉の判定 ----
    CHECK(!GpuTimer::IsLeaf(GpuTimer::Total));
    CHECK(!GpuTimer::IsLeaf(GpuTimer::Shadows));
    CHECK(!GpuTimer::IsLeaf(GpuTimer::PrepassSSAO));
    CHECK(!GpuTimer::IsLeaf(GpuTimer::RtScreen));
    CHECK(!GpuTimer::IsLeaf(GpuTimer::Particles));
    CHECK(!GpuTimer::IsLeaf(GpuTimer::PostFX));
    CHECK(!GpuTimer::IsLeaf(GpuTimer::UI));
    CHECK(GpuTimer::IsLeaf(GpuTimer::MainScene));
    CHECK(GpuTimer::IsLeaf(GpuTimer::Clouds));
    CHECK(GpuTimer::IsLeaf(GpuTimer::Upscale));
    CHECK(GpuTimer::IsLeaf(GpuTimer::VfxSim));
    CHECK(GpuTimer::IsLeaf(GpuTimer::GameHud));
    CHECK(!GpuTimer::IsLeaf(GpuTimer::Count));

    // ---- 別名の解決（大文字小文字を無視）----
    {
        GpuTimer::Scope s = GpuTimer::Total;
        CHECK(GpuTimer::FindScope("FogScatter", s) && s == GpuTimer::VolumetricFog);
        CHECK(GpuTimer::FindScope("RtReflect", s) && s == GpuTimer::RtReflection);
        CHECK(GpuTimer::FindScope("AsBuild", s) && s == GpuTimer::Raytracing);
        CHECK(GpuTimer::FindScope("volFog", s) && s == GpuTimer::VolumetricFog);
        CHECK(GpuTimer::FindScope("VFXSIM", s) && s == GpuTimer::VfxSim);
        CHECK(!GpuTimer::FindScope("noSuchScope", s));
        CHECK(!GpuTimer::FindScope(nullptr, s));
    }

    // ---- 葉の合計の規則 ----
    {
        float ms[GpuTimer::Count] = {};
        ms[GpuTimer::Total] = 10.0f;
        ms[GpuTimer::PostFX] = 3.0f;       // 子が全部 0 → 親自身を葉として数える
        ms[GpuTimer::MainScene] = 4.0f;
        ms[GpuTimer::Shadows] = 2.0f;
        CHECK(GpuTimer::LeafSumMs(ms) > 8.99f && GpuTimer::LeafSumMs(ms) < 9.01f);
        ms[GpuTimer::Upscale] = 1.0f;      // 子が計測された → PostFX は親、葉は Upscale
        ms[GpuTimer::PostFX] = 3.0f;
        CHECK(!GpuTimer::IsEffectiveLeaf(GpuTimer::PostFX, ms));
        CHECK(GpuTimer::IsEffectiveLeaf(GpuTimer::Upscale, ms));
        CHECK(GpuTimer::LeafSumMs(ms) > 6.99f && GpuTimer::LeafSumMs(ms) < 7.01f);
        CHECK(GpuTimer::LeafSumMs(ms) <= ms[GpuTimer::Total] + GpuTimer::kLeafSumToleranceMs);
        CHECK(!GpuTimer::IsEffectiveLeaf(GpuTimer::Total, ms));
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
