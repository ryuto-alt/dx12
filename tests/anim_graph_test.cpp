// アニメーションイベント収集（animation/AnimEvent.h）のテスト。
//
// 収集の境界は「開いた左・閉じた右」= (prev, cur] で統一している。
// ループでラップしたフレームだけ (prev, end] + [0, cur] の 2 区間を見る。
// これで足音がちょうど 1 周に 1 回だけ鳴る（二重発火も取りこぼしも無い）。
//
// 依存ゼロ（STL のみ）なのでスタンドアロン構成でも動く。
#include "animation/AnimEvent.h"
#include "animation/AnimationClip.h"

#include <cstdio>
#include <vector>

using namespace dx12e;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// walk クリップ相当: 0.25 秒に左足、0.75 秒に右足（duration 1.0 秒）
static std::vector<AnimEvent> BuildWalkEvents()
{
    std::vector<AnimEvent> ev;
    ev.push_back({0.25f, "footstep", "left", 0.0f});
    ev.push_back({0.75f, "footstep", "right", 0.0f});
    return ev;
}

// 1 周ぶん 60fps で回すとイベントがちょうど 1 回ずつ
static void TestOneEventPerLoop()
{
    const std::vector<AnimEvent> ev = BuildWalkEvents();
    const float duration = 1.0f;
    const float dt = 1.0f / 60.0f;

    int leftCount = 0, rightCount = 0;
    float prev = 0.0f;
    std::vector<u32> hits;

    // ちょうど 3 周ぶん進める
    const int steps = static_cast<int>(3.0f * duration / dt);
    for (int i = 0; i < steps; ++i)
    {
        float cur = prev + dt;
        bool wrapped = false;
        if (cur >= duration) { cur -= duration; wrapped = true; }

        hits.clear();
        CollectAnimEvents(ev, prev, cur, wrapped, hits);
        for (u32 h : hits)
        {
            if (ev[h].stringParam == "left")  ++leftCount;
            if (ev[h].stringParam == "right") ++rightCount;
        }
        prev = cur;
    }

    // 3 周で 3 回ずつ（3.0/dt = 180 ステップちょうどなので端数の取りこぼしは無い）
    CHECK(leftCount == 3);
    CHECK(rightCount == 3);
}

// ラップをまたぐ 1 フレームで、境界の両側のイベントが 1 回ずつ出る
static void TestWrapAcrossBoundary()
{
    std::vector<AnimEvent> ev;
    ev.push_back({0.05f, "early", "", 0.0f});
    ev.push_back({0.95f, "late",  "", 0.0f});

    std::vector<u32> hits;
    // prev=0.90 → cur=0.06（duration 1.0 をまたいだ）。
    // 末尾区間 (0.90, 1.0] の "late"(0.95) と、先頭区間 [0, 0.06] の "early"(0.05) の両方が出る。
    CollectAnimEvents(ev, 0.90f, 0.06f, true, hits);
    CHECK(hits.size() == 2);

    // まだ 0.05 に届いていないラップフレームでは "late" だけ
    hits.clear();
    CollectAnimEvents(ev, 0.90f, 0.02f, true, hits);
    CHECK(hits.size() == 1);
    CHECK(ev[hits[0]].name == "late");

    // 逆に「またいでいない」普通のフレームでは 0 件
    hits.clear();
    CollectAnimEvents(ev, 0.10f, 0.20f, false, hits);
    CHECK(hits.empty());
}

// prev == cur（時間が進んでいない）では発火しない
static void TestNoAdvanceNoFire()
{
    const std::vector<AnimEvent> ev = BuildWalkEvents();
    std::vector<u32> hits;
    CollectAnimEvents(ev, 0.25f, 0.25f, false, hits);
    CHECK(hits.empty());

    CollectAnimEvents(ev, 0.0f, 0.0f, false, hits);
    CHECK(hits.empty());
}

// 境界は「開いた左・閉じた右」: ちょうど cur に乗ったイベントは出る、prev に乗ったものは出ない
static void TestHalfOpenInterval()
{
    std::vector<AnimEvent> ev;
    ev.push_back({0.5f, "mark", "", 0.0f});

    std::vector<u32> hits;
    CollectAnimEvents(ev, 0.4f, 0.5f, false, hits);   // cur にちょうど乗る → 出る
    CHECK(hits.size() == 1);

    hits.clear();
    CollectAnimEvents(ev, 0.5f, 0.6f, false, hits);   // prev にちょうど乗る → 出ない
    CHECK(hits.empty());

    // 連続する 2 フレームで合計ちょうど 1 回（二重発火しない）
    hits.clear();
    CollectAnimEvents(ev, 0.45f, 0.50f, false, hits);
    CollectAnimEvents(ev, 0.50f, 0.55f, false, hits);
    CHECK(hits.size() == 1);
}

// 1 フレームに複数イベントが入ったら時刻順に出る
static void TestMultipleEventsOrdered()
{
    std::vector<AnimEvent> ev;
    ev.push_back({0.10f, "a", "", 0.0f});
    ev.push_back({0.20f, "b", "", 0.0f});
    ev.push_back({0.30f, "c", "", 0.0f});

    std::vector<u32> hits;
    CollectAnimEvents(ev, 0.0f, 0.5f, false, hits);
    CHECK(hits.size() == 3);
    CHECK(hits[0] == 0 && hits[1] == 1 && hits[2] == 2);
}

// 空のイベント列で落ちない
static void TestEmpty()
{
    std::vector<AnimEvent> ev;
    std::vector<u32> hits;
    CollectAnimEvents(ev, 0.0f, 1.0f, true, hits);
    CHECK(hits.empty());
}

// AnimationClip::AddEvent は時刻昇順を保つ / NormalizeToSeconds がイベント時刻も畳む
static void TestClipEventStorage()
{
    AnimationClip clip;
    clip.SetTicksPerSecond(1000.0f);   // glTF 由来（ミリ秒）
    clip.SetDuration(1000.0f);
    clip.AddEvent({750.0f, "footstep", "right", 0.0f});
    clip.AddEvent({250.0f, "footstep", "left", 0.0f});   // わざと逆順で入れる

    CHECK(clip.GetEvents().size() == 2);
    CHECK(clip.GetEvents()[0].stringParam == "left");    // 昇順に並ぶ
    CHECK(clip.GetEvents()[1].stringParam == "right");

    clip.NormalizeToSeconds();
    CHECK(clip.GetDuration() > 0.999f && clip.GetDuration() < 1.001f);
    CHECK(clip.GetEvents()[0].time > 0.249f && clip.GetEvents()[0].time < 0.251f);
    CHECK(clip.GetEvents()[1].time > 0.749f && clip.GetEvents()[1].time < 0.751f);

    clip.ClearEvents();
    CHECK(clip.GetEvents().empty());
}

int main()
{
    TestOneEventPerLoop();
    TestWrapAcrossBoundary();
    TestNoAdvanceNoFire();
    TestHalfOpenInterval();
    TestMultipleEventsOrdered();
    TestEmpty();
    TestClipEventStorage();

    std::printf("AnimGraphTests: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
