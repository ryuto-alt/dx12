// UnoFX の実行時窓口の試験（FU-E-19）: GPU 初期化の失敗が黙殺されず、統計（vfx_stats の元）へ出る。GPU 不要。
#include "ecs/Components.h"
#include "ecs/components/VfxComponent.h"
#include "renderer/vfx/VfxRuntime.h"
#include "renderer/vfx/VfxSystem.h"
#include <entt/entt.hpp>

#include <cstdio>

using namespace dx12e::vfx;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main()
{
    Runtime& rt = Runtime::Get();
    CHECK(!rt.GetStats().gpuFailed);
    CHECK(rt.DescribeGpuFailure().empty());

    Stats ok; ok.gpuReady = true;
    rt.PublishStats(std::move(ok));
    CHECK(rt.GetStats().gpuReady && !rt.GetStats().gpuFailed);

    rt.ReportGpuFailure("UnoFX: テスト用の失敗");
    CHECK(rt.GetStats().gpuFailed);
    CHECK(!rt.GetStats().gpuReady);
    CHECK(rt.GetStats().lastError == "UnoFX: テスト用の失敗");
    CHECK(rt.DescribeGpuFailure() == "UnoFX: テスト用の失敗");

    rt.ReportGpuFailure("");
    CHECK(rt.GetStats().gpuFailed);
    CHECK(!rt.DescribeGpuFailure().empty());   // 理由が空でも説明は空にならない

    // VfxSystem::Update の失敗経路（GPU 不要）: VfxComponent を持つエンティティがあり、コマンドリストが null なら EnsureGpu が最初の枝で失敗する。
    {
        entt::registry reg;
        const entt::entity e = reg.create();
        reg.emplace<dx12e::Transform>(e);
        reg.emplace<dx12e::VfxComponent>(e);
        Host host;
        host.user = &reg;
        host.registry = [](void* u) { return static_cast<entt::registry*>(u); };
        rt.SetHost(host);
        rt.PublishStats(Stats{});   // 失敗フラグを一旦消す

        VfxSystem sys;
        dx12e::StageCtx ctx;   // cmd == nullptr
        sys.Update(ctx);
        CHECK(rt.GetStats().gpuFailed);
        CHECK(!rt.GetStats().gpuReady);
        CHECK(!rt.GetStats().lastError.empty());
        CHECK(rt.GetStats().lastError.find("UnoFX") != std::string::npos);
        const std::string firstErr = rt.GetStats().lastError;

        rt.PublishStats(Stats{});   // 2 回目の Update が再報告しないことを見る（FailGpu は 1 回だけ）
        sys.Update(ctx);
        CHECK(!rt.GetStats().gpuFailed);
        CHECK(rt.GetStats().lastError.empty());
        CHECK(!firstErr.empty());
    }

    if (g_failures == 0) std::printf("VfxRuntimeTests: OK\n");
    return g_failures == 0 ? 0 : 1;
}
