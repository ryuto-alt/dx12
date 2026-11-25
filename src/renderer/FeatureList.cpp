// FeatureRegistry へ描画機能を登録する（W05-25）。
//   登録表は FeatureList.inc。各レーンは自分のブロックの中に UNO_FEATURE(Type) を 1 行足す。
//   Type ごとに、そのレーンの .cpp が `std::unique_ptr<IRenderFeature> Make<Type>();` を定義する
//   （ここでは型のヘッダを include しないので、レーンの機能を足してもこの TU の依存は増えない）。
#include "renderer/RenderStageHooks.h"
#include "renderer/StagePriority.h"

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
        free(e);
    }
#endif
}

} // namespace dx12e
