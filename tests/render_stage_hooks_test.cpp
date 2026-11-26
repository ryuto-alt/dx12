// ステージフック / FeatureRegistry の検査（W05-25）。
// 順序、優先度の帯、登録 0 件なら何も呼ばれない、ステージの選別、OnResize の通知。
#include "renderer/RenderStageHooks.h"
#include "renderer/StagePriority.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace dx12e;

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::string g_log;
static int g_resizeCalls = 0;

class Probe final : public IRenderFeature
{
public:
    Probe(const char* name, StageMask mask, i32 prio) : m_name(name), m_mask(mask), m_prio(prio) {}
    const char* Name() const override { return m_name; }
    StageMask   Stages() const override { return m_mask; }
    i32         Priority(Stage) const override { return m_prio; }
    void        OnResize(u32, u32, u32, u32) override { ++g_resizeCalls; }
    void        Record(Stage s, StageCtx& ctx) override
    {
        g_log += m_name;
        g_log += '@';
        g_log += StageName(s);
        g_log += ';';
        if (ctx.cmd) g_log += "CMD;";   // cmd が null のままでも落ちないこと（ここでは触らない）
    }
    u64 VramBytes() const override { return 100; }
private:
    const char* m_name;
    StageMask   m_mask;
    i32         m_prio;
};

int main()
{
    // ステージ数と名前。
    CHECK(kStageCount == 14);
    for (u32 i = 0; i < kStageCount; ++i) CHECK(std::strcmp(StageName(static_cast<Stage>(i)), "?") != 0);

    // 登録 0 件: 全ステージが Empty で、Run しても何も起きない。
    {
        FeatureRegistry reg;
        StageCtx ctx;
        g_log.clear();
        CHECK(reg.Empty());
        for (u32 i = 0; i < kStageCount; ++i)
        {
            CHECK(reg.Empty(static_cast<Stage>(i)));
            reg.Run(static_cast<Stage>(i), ctx);
        }
        CHECK(g_log.empty());
        CHECK(reg.TotalVramBytes() == 0);
    }

    // 順序: 優先度の昇順、同じ優先度は登録順。ステージの選別。
    {
        using namespace stage_priority;
        FeatureRegistry reg;
        reg.Add(std::make_unique<Probe>("B_bdb", StageBit(Stage::AfterTranslucent), kAfterTranslucent_BdB.lo));
        reg.Add(std::make_unique<Probe>("V_vfx", StageBit(Stage::AfterTranslucent) | StageBit(Stage::AfterDepthPrepass),
                                        kAfterTranslucent_Vfx.lo));
        reg.Add(std::make_unique<Probe>("C_glass", StageBit(Stage::AfterTranslucent), kAfterTranslucent_C.lo));
        reg.Add(std::make_unique<Probe>("V2_vfx", StageBit(Stage::AfterTranslucent), kAfterTranslucent_Vfx.lo));
        CHECK(reg.Count() == 4);
        CHECK(!reg.Empty(Stage::AfterTranslucent));
        CHECK(!reg.Empty(Stage::AfterDepthPrepass));
        CHECK(reg.Empty(Stage::BeforeShadows));

        StageCtx ctx;
        g_log.clear();
        reg.Run(Stage::AfterTranslucent, ctx);
        CHECK(g_log == "C_glass@AfterTranslucent;V_vfx@AfterTranslucent;V2_vfx@AfterTranslucent;B_bdb@AfterTranslucent;");
        g_log.clear();
        reg.Run(Stage::AfterDepthPrepass, ctx);
        CHECK(g_log == "V_vfx@AfterDepthPrepass;");
        g_log.clear();
        reg.Run(Stage::BeforeShadows, ctx);   // 空のステージは何もしない
        CHECK(g_log.empty());
        CHECK(reg.TotalVramBytes() == 400);

        auto order = reg.OrderOf(Stage::AfterTranslucent);
        CHECK(order.size() == 4);

        // 帯の検査: C < Vfx < BdB が重ならない。
        CHECK(kAfterTranslucent_C.hi < kAfterTranslucent_Vfx.lo);
        CHECK(kAfterTranslucent_Vfx.hi < kAfterTranslucent_BdB.lo);
        CHECK(kAfterDepthPrepass_C.hi < kAfterDepthPrepass_VfxSim.lo);
        CHECK(kAfterTranslucent_Vfx.Contains(kFirstVfx));
        CHECK(!kAfterTranslucent_C.Contains(kFirstVfx));

        // 解像度の通知: 初回は必ず、同じ値なら 2 回目は通知しない、変われば通知する。
        g_resizeCalls = 0;
        reg.SyncResize(1280, 720, 1920, 1080);
        CHECK(g_resizeCalls == 4);
        reg.SyncResize(1280, 720, 1920, 1080);
        CHECK(g_resizeCalls == 4);
        reg.SyncResize(1280, 720, 2560, 1440);
        CHECK(g_resizeCalls == 8);
    }

    // StageCtx の既定は全部「未提供」（ptr==0 / null）で、呼び出しが安全。
    {
        StageCtx ctx;
        CHECK(ctx.cmd == nullptr && ctx.hiZSrv.ptr == 0 && ctx.velocitySrv.ptr == 0 && ctx.perFrameGpuVa == 0);
        ctx.SetTexMipBias(-1.0f);       // 未結線でも落ちない
        ctx.RequestVelocityPrepass();   // 同上
        bool req = false;
        ctx.velocityPrepassRequest = &req;
        ctx.RequestVelocityPrepass();
        CHECK(req);
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
