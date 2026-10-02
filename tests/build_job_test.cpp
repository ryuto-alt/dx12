// ゲームビルドの裏ジョブの進捗モデル(core/BuildJob.h・ヘッダのみ)のテスト。GPU もエンジン本体も不要。
//   ・段が進むと全体の進みが単調に増える（段内の done/total も効く）
//   ・不定の段（total=0）は段の始まりの値
//   ・Finish は終端状態を最後に書く / 成功なら 1.0
//   ・キャンセル要求は Begin で下ろされる（前回の要求を持ち越さない）
#include "core/BuildJob.h"

#include <cstdio>

using namespace dx12e;

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)

int main()
{
    BuildProgress p;
    CHECK(p.state.load() == BuildProgress::Idle);
    CHECK(p.Fraction() == 0.0f);

    p.cancelRequested = true;
    p.Begin("C:/out");
    CHECK(p.state.load() == BuildProgress::Running);
    CHECK(!p.cancelRequested.load());                 // 前回の中止要求を持ち越さない

    float prev = -1.0f;
    for (int s = 1; s <= BuildProgress::kStageCount; ++s)
    {
        p.SetStage(s, {}, s == 3 ? 100 : 0);
        const float f0 = p.Fraction();
        CHECK(f0 >= prev);                            // 段が進んでも戻らない
        prev = f0;
        if (s == 3)
        {
            p.done = 50;
            const float mid = p.Fraction();
            CHECK(mid > f0);                          // 段内の進みが効く（確定した段は割合で動く）
            p.done = 100;
            CHECK(p.Fraction() <= BuildProgress::StageEndPct(3) / 100.0f + 1e-6f);
            prev = p.Fraction();
        }
    }
    CHECK(p.Fraction() < 1.0f);                       // 完了するまで 1.0 にならない

    // 不定の段（テクスチャの事前生成）は段の始まりの値のまま
    p.SetStage(4, "x", 0);
    CHECK(p.Fraction() == BuildProgress::StageStartPct(4) / 100.0f);

    p.Finish(BuildProgress::Succeeded);
    CHECK(p.state.load() == BuildProgress::Succeeded);
    CHECK(p.Fraction() == 1.0f);
    CHECK(p.ElapsedSec() >= 0.0);

    p.Begin("C:/out2");
    p.Finish(BuildProgress::Failed, "壊れた");
    CHECK(p.state.load() == BuildProgress::Failed);
    { std::lock_guard<std::mutex> lk(p.mu); CHECK(p.error == "壊れた"); CHECK(p.outputDir == "C:/out2"); }

    std::printf("build_job: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
