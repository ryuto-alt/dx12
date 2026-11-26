// GpuTimer のスコープ表の検査（W05-26）。GPU は使わない（属性表と純関数だけ）。
// 64 ビットのマスク、葉と親の判定、別名の解決、既存 22 本の名前と番号の不変、葉の合計の規則。
#include "graphics/GpuTimer.h"

#include <cmath>
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

    // ---- 葉の合計の規則: 構造上の葉は自分、親は self = 親 − Σ子（下限 0）----
    {
        float ms[GpuTimer::Count] = {};
        ms[GpuTimer::Total] = 10.0f;
        ms[GpuTimer::PostFX] = 3.0f;       // 子が全部 0 → postFx.self = 3
        ms[GpuTimer::MainScene] = 4.0f;
        ms[GpuTimer::Shadows] = 2.0f;      // 子が全部 0 → shadows.self = 2
        // total.self = 10 - (shadows 2 + prepass 0 + ... + mainScene 4 + postFx 3 ...) = 1。合計は Total に一致する。
        CHECK(std::fabs(GpuTimer::LeafSumMs(ms) - 10.0f) < 0.001f);
        ms[GpuTimer::Upscale] = 1.0f;      // 子が計測された → postFx.self = 2、upscale = 1
        CHECK(std::fabs(GpuTimer::LeafMs(GpuTimer::PostFX, ms) - 2.0f) < 0.001f);
        CHECK(std::fabs(GpuTimer::LeafMs(GpuTimer::Upscale, ms) - 1.0f) < 0.001f);
        CHECK(std::fabs(GpuTimer::LeafSumMs(ms) - 10.0f) < 0.001f);   // 親の自己時間が落ちない
        CHECK(GpuTimer::LeafSumMs(ms) <= ms[GpuTimer::Total] + GpuTimer::kLeafSumToleranceMs);
        // 子が親より大きい（重なり）→ 親 self は 0 に切られ、合計が Total を超えて検出できる
        ms[GpuTimer::Taa] = 5.0f;
        CHECK(GpuTimer::LeafMs(GpuTimer::PostFX, ms) == 0.0f);
        CHECK(GpuTimer::LeafSumMs(ms) > ms[GpuTimer::Total] + GpuTimer::kLeafSumToleranceMs);
        // LeafEntries: 親は .self、葉はそのまま。件数は Count（除外が無ければ全スコープ）
        GpuTimer::LeafEntry le[GpuTimer::kMaxScopes];
        CHECK(GpuTimer::LeafEntries(ms, 0, le) == GpuTimer::Count);
    }

    // ---- 複数ビュー（multiMask）のスコープは入れ子の検査と葉から外す ----
    {
        // Clouds（Particles の子）が複数ビューで書かれた。除外とその時間は親 Particles の self に残る。
        float ms[GpuTimer::Count] = {};
        ms[GpuTimer::Total] = 8.0f;
        ms[GpuTimer::Particles] = 3.0f;
        ms[GpuTimer::Clouds] = 2.0f;
        ms[GpuTimer::MainScene] = 5.0f;
        const u64 multi = 1ull << GpuTimer::Clouds;
        CHECK(GpuTimer::ExpandExcludeMask(multi) == multi);
        CHECK(GpuTimer::ExpandExcludeMask(1ull) == 0);   // Total は除外できない
        CHECK(std::fabs(GpuTimer::LeafMs(GpuTimer::Particles, ms, multi) - 3.0f) < 0.001f);
        CHECK(GpuTimer::LeafMs(GpuTimer::Clouds, ms, multi) == 0.0f);
        CHECK(std::fabs(GpuTimer::LeafSumMs(ms, multi) - 8.0f) < 0.001f);
        // 親が除外されたら子孫も除外（親の self に時間が残るので二重に数えない）
        const u64 exP = GpuTimer::ExpandExcludeMask(1ull << GpuTimer::Particles);
        CHECK((exP & (1ull << GpuTimer::Clouds)) != 0);
        CHECK(std::fabs(GpuTimer::LeafSumMs(ms, 1ull << GpuTimer::Particles) - 8.0f) < 0.001f);
        GpuTimer::LeafEntry le[GpuTimer::kMaxScopes];
        const u32 n = GpuTimer::LeafEntries(ms, multi, le);
        CHECK(n == GpuTimer::Count - 1);
        for (u32 i = 0; i < n; ++i) CHECK(le[i].scope != GpuTimer::Clouds);
    }

    // ---- 1 フレームの検査（CheckFrame: タイムスタンプとマスクだけで動く純関数）----
    {
        constexpr double kFreq = 1000.0;   // 1 tick = 1 ms
        u64 b[GpuTimer::Count] = {}, e[GpuTimer::Count] = {};
        float ms[GpuTimer::Count] = {};
        auto setScope = [&](GpuTimer::Scope s, u64 begin, u64 end) { b[s] = begin; e[s] = end; };
        const u64 W = (1ull << GpuTimer::Total) | (1ull << GpuTimer::Shadows) | (1ull << GpuTimer::VgShadow) |
                      (1ull << GpuTimer::PostFX) | (1ull << GpuTimer::Upscale) | (1ull << GpuTimer::Taa) |
                      (1ull << GpuTimer::Particles) | (1ull << GpuTimer::Clouds);

        // 正常: 子が親の中、葉の合計 == Total。
        setScope(GpuTimer::Total, 0, 100);
        setScope(GpuTimer::Shadows, 10, 30);    setScope(GpuTimer::VgShadow, 12, 20);
        setScope(GpuTimer::Particles, 40, 50);  setScope(GpuTimer::Clouds, 42, 46);
        setScope(GpuTimer::PostFX, 60, 90);     setScope(GpuTimer::Upscale, 62, 70);  setScope(GpuTimer::Taa, 70, 80);
        auto r = GpuTimer::CheckFrame(b, e, W, 0, kFreq, ms);
        CHECK(r.nestViolations == 0 && r.nestViolatedMask == 0);
        CHECK(std::fabs(ms[GpuTimer::Total] - 100.0f) < 0.001f);
        CHECK(std::fabs(ms[GpuTimer::VgShadow] - 8.0f) < 0.001f);
        CHECK(std::fabs(r.leafSumMs - 100.0f) < 0.01f && !r.leafSumExceeded);

        // 子が親の外に出る（VgShadow が Shadows の終わりを越える）→ 逸脱が 1 件、そのビットが立つ。
        setScope(GpuTimer::VgShadow, 12, 35);
        r = GpuTimer::CheckFrame(b, e, W, 0, kFreq, ms);
        CHECK(r.nestViolations == 1 && r.nestViolatedMask == (1ull << GpuTimer::VgShadow));
        setScope(GpuTimer::VgShadow, 5, 20);   // 親の始まりより前
        r = GpuTimer::CheckFrame(b, e, W, 0, kFreq, ms);
        CHECK(r.nestViolations == 1);
        setScope(GpuTimer::VgShadow, 12, 20);

        // 二重書き込み（複数ビュー）: Clouds が Particles の外に出ても、multiMask に入っていれば誤検知しない。
        setScope(GpuTimer::Clouds, 95, 99);
        r = GpuTimer::CheckFrame(b, e, W, 0, kFreq, ms);
        CHECK(r.nestViolations == 1 && r.nestViolatedMask == (1ull << GpuTimer::Clouds));
        r = GpuTimer::CheckFrame(b, e, W, 1ull << GpuTimer::Clouds, kFreq, ms);
        CHECK(r.nestViolations == 0);
        CHECK(std::fabs(r.leafSumMs - 100.0f) < 0.01f && !r.leafSumExceeded);   // Clouds の時間は particles.self に残る
        setScope(GpuTimer::Clouds, 42, 46);

        // 葉の合計の超過: 兄弟（Upscale と Taa）が重なる → 葉の合計 > Total + 0.1ms。
        setScope(GpuTimer::Taa, 62, 90);   // PostFX(60..90) の中だが Upscale(62..70) と重なる
        r = GpuTimer::CheckFrame(b, e, W, 0, kFreq, ms);
        CHECK(r.nestViolations == 0);
        CHECK(r.leafSumMs > r.totalMs + GpuTimer::kLeafSumToleranceMs && r.leafSumExceeded);
        setScope(GpuTimer::Taa, 70, 80);

        // 書かれていないスコープは 0 ms、親が書かれていなければ検査しない。
        r = GpuTimer::CheckFrame(b, e, W & ~(1ull << GpuTimer::Shadows), 0, kFreq, ms);
        CHECK(r.nestViolations == 0 && ms[GpuTimer::Shadows] == 0.0f);
        // end <= begin は 0 ms（Resolve 前の取りこぼし）
        setScope(GpuTimer::Taa, 80, 80);
        (void)GpuTimer::CheckFrame(b, e, W, 0, kFreq, ms);
        CHECK(ms[GpuTimer::Taa] == 0.0f);
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
