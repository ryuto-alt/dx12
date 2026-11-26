// FeatureRegistry へ描画機能を登録する（W05-25）。
//   登録表は FeatureList.inc。各レーンは自分のブロックの中に UNO_FEATURE(Type) を 1 行足す。
//   Type ごとに、そのレーンの .cpp が `std::unique_ptr<IRenderFeature> Make<Type>();` を定義する
//   （ここでは型のヘッダを include しないので、レーンの機能を足してもこの TU の依存は増えない）。
#include "renderer/RenderStageHooks.h"
#include "renderer/StagePriority.h"
#if defined(UNO_TESTING)
#include "core/Logger.h"
#include "renderer/FrameConstants.h"
#endif

#include <cstdlib>
#include <memory>

namespace dx12e
{

// ---- 1 回目: ファクトリの宣言 ----
#define UNO_FEATURE(Type) std::unique_ptr<IRenderFeature> Make##Type();
#define X(Type, ...)      UNO_FEATURE(Type)
#include "renderer/FeatureList.inc"
#undef X
#undef UNO_FEATURE

#if defined(UNO_TESTING)
namespace
{
// 試験用: AfterTranslucent で sceneRT の左上近くに 4x4 の矩形（マゼンタ）を描く。
// 全ゴールデンへ影響しないよう、UNO_TESTING かつ環境変数 UNO_TEST_STAGE_RECT=1 のときだけ登録する。
class TestRectFeature final : public IRenderFeature
{
public:
    const char* Name() const override { return "TestRect"; }
    StageMask   Stages() const override { return StageBit(Stage::AfterTranslucent); }
    i32         Priority(Stage) const override { return stage_priority::kAfterTranslucent_C.lo; }
    void        Record(Stage, StageCtx& ctx) override
    {
        if (!ctx.cmd || !ctx.primary || ctx.sceneRtv.ptr == 0) return;
        const float color[4] = {1.0f, 0.0f, 1.0f, 1.0f};
        const D3D12_RECT rect{16, 16, 20, 20};
        ctx.cmd->ClearRenderTargetView(ctx.sceneRtv, color, 1, &rect);
    }
};

// 試験用（UNO_TEST_STAGE_RECT=2）: ステージ経路の配線を実機で通す。
//   BeforeShadows: 速度プリパスを要求 / AfterDepthPrepass: depth を NON_PIXEL へ遷移・灯を 1 つ足す・ミップバイアスを書く /
//   BeforeForwardOpaque: perFrameCpu から灯数を読む / PostPreBloom: sceneColor を COPY_SOURCE へ遷移（呼び出し側が戻す）。
//   各ステージの観測値を、最初の数フレームだけログへ出す。
class TestWiringFeature final : public IRenderFeature
{
public:
    const char* Name() const override { return "TestWiring"; }
    StageMask   Stages() const override
    {
        return StageBit(Stage::BeforeShadows) | StageBit(Stage::AfterDepthPrepass) |
               StageBit(Stage::BeforeForwardOpaque) | StageBit(Stage::PostPreBloom);
    }
    i32         Priority(Stage) const override { return stage_priority::kDefault_C.lo; }
    void        Record(Stage st, StageCtx& ctx) override
    {
        if (!ctx.primary) return;
        const bool log = m_logged < 6;
        switch (st)
        {
        case Stage::BeforeShadows:
            ctx.RequestVelocityPrepass();
            break;
        case Stage::AfterDepthPrepass:
        {
            const bool tr = ctx.RequireDepth(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ClusterLight l{};
            l.position = {0.0f, 3.0f, 0.0f}; l.range = 8.0f; l.intensity = 1.0f;
            if (ctx.AppendClusterLights) ctx.AppendClusterLights(ctx.self, &l, 1);
            ctx.SetTexMipBias(0.0f);
            if (log) Logger::Info("[TestWiring] AfterDepthPrepass depth={} transitioned={} velocitySrv={} hiZSrv={} pfVa={}",
                                  ctx.depth != nullptr, tr, ctx.velocitySrv.ptr != 0, ctx.hiZSrv.ptr != 0, ctx.perFrameGpuVa != 0);
            break;
        }
        case Stage::BeforeForwardOpaque:
            if (log) Logger::Info("[TestWiring] BeforeForwardOpaque lights(clusterExtra.x)={} pfVa={} sceneRtv={}",
                                  ctx.perFrameCpu ? ctx.perFrameCpu->clusterExtra.x : -1.0f, ctx.perFrameGpuVa != 0, ctx.sceneRtv.ptr != 0);
            break;
        case Stage::PostPreBloom:
        {
            const bool tr = ctx.RequireSceneColor(D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (log) Logger::Info("[TestWiring] PostPreBloom sceneColor={} transitioned={} pfVa={} pfCpuNull={}",
                                  ctx.sceneColor != nullptr, tr, ctx.perFrameGpuVa != 0, ctx.perFrameCpu == nullptr);
            ++m_logged;
            break;
        }
        default: break;
        }
    }
private:
    u32 m_logged = 0;
};
} // namespace
#endif

void RegisterAllFeatures(FeatureRegistry& reg)
{
    (void)reg;   // 登録表が空のとき
    // ---- 2 回目: 登録 ----
#define UNO_FEATURE(Type) reg.Add(Make##Type());
#define X(Type, ...)      UNO_FEATURE(Type)
#include "renderer/FeatureList.inc"
#undef X
#undef UNO_FEATURE

#if defined(UNO_TESTING)
    char*  e   = nullptr;
    size_t len = 0;
    if (_dupenv_s(&e, &len, "UNO_TEST_STAGE_RECT") == 0 && e)
    {
        if (e[0] == '1') reg.Add(std::make_unique<TestRectFeature>());
        if (e[0] == '2') reg.Add(std::make_unique<TestWiringFeature>());
        free(e);
    }
#endif
}

} // namespace dx12e
