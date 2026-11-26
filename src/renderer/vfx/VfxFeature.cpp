// VfxFeature（W1-BD4 / UnoFX）: 描画ステージのフックに載る IRenderFeature。
//   AfterDepthPrepass で粒子を進め（シミュレーションを描画パスから切り離す。主ビューだけ。副ビューは同じプールを読むだけ）、
//   AfterTranslucent で描く（BD-VFX の帯の先頭）。登録は FeatureList.inc の lane-B ブロック。
#include "renderer/RenderStageHooks.h"
#include "renderer/StagePriority.h"
#include "renderer/vfx/VfxSystem.h"

namespace dx12e
{
namespace
{
class VfxFeature final : public IRenderFeature
{
public:
    const char* Name() const override { return "UnoFX"; }
    StageMask   Stages() const override { return StageBit(Stage::AfterDepthPrepass) | StageBit(Stage::AfterTranslucent); }
    i32         Priority(Stage s) const override
    {
        return s == Stage::AfterDepthPrepass ? stage_priority::kAfterDepthPrepass_VfxSim.lo
                                             : stage_priority::kAfterTranslucent_Vfx.lo;
    }
    void Record(Stage s, StageCtx& ctx) override
    {
        if (s == Stage::AfterDepthPrepass) { if (ctx.primary) m_sys.Update(ctx); }
        else m_sys.Draw(ctx);
    }
    u64 VramBytes() const override { return m_sys.VramBytes(); }

private:
    vfx::VfxSystem m_sys;
};
} // namespace

std::unique_ptr<IRenderFeature> MakeVfxFeature() { return std::make_unique<VfxFeature>(); }

} // namespace dx12e
