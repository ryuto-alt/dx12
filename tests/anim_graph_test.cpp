// アニメーションイベント収集（animation/AnimEvent.h）と
// ステートマシンの遷移判定（animation/AnimGraphAsset.h）のテスト。
//
// 収集の境界は「開いた左・閉じた右」= (prev, cur] で統一している。
// ループでラップしたフレームだけ (prev, end] + [0, cur] の 2 区間を見る。
// これで足音がちょうど 1 周に 1 回だけ鳴る（二重発火も取りこぼしも無い）。
//
// 依存ゼロ（STL のみ）なのでスタンドアロン構成でも動く。
#include "animation/AnimEvent.h"
#include "animation/AnimGraphAsset.h"
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

// ===========================================================================
// ステートマシン（AnimGraphAsset.h の純関数）
// ===========================================================================

// Idle / Locomotion / Jump の 3 ステート。
//   Idle → Locomotion   speed > 0.1
//   Locomotion → Idle   speed <= 0.1
//   *    → Jump         トリガ "jump"（Any State）
//   Jump → Idle         grounded == true、exitTime 0.8、割り込み不可
static AnimLayerDef BuildLocomotionLayer()
{
    AnimLayerDef layer;
    layer.name = "Base";
    layer.defaultState = "Idle";
    layer.states.push_back({"Idle",       "idle", true, 1.0f, {}, 0});
    layer.states.push_back({"Locomotion", "walk", true, 1.0f, {}, 1});
    layer.states.push_back({"Jump",       "jump", false, 1.0f, {}, 2});

    {
        AnimTransitionDef tr;
        tr.from = "Idle"; tr.to = "Locomotion"; tr.duration = 0.15f;
        tr.conditions.push_back({"speed", AnimCondOp::Gt, 0.1f});
        layer.transitions.push_back(tr);
    }
    {
        AnimTransitionDef tr;
        tr.from = "Locomotion"; tr.to = "Idle"; tr.duration = 0.15f;
        tr.conditions.push_back({"speed", AnimCondOp::Le, 0.1f});
        layer.transitions.push_back(tr);
    }
    {
        AnimTransitionDef tr;
        tr.from = "*"; tr.to = "Jump"; tr.duration = 0.1f;
        tr.conditions.push_back({"jump", AnimCondOp::Trigger, 0.0f});
        layer.transitions.push_back(tr);
    }
    {
        AnimTransitionDef tr;
        tr.from = "Jump"; tr.to = "Idle"; tr.duration = 0.2f;
        tr.conditions.push_back({"grounded", AnimCondOp::IsTrue, 0.0f});
        tr.hasExitTime = true; tr.exitTime = 0.8f;
        tr.interruptible = false;
        layer.transitions.push_back(tr);
    }

    // ResolveAsset がやることを手で行う（この関数は Animation ライブラリ側にあり、
    // テストは純ヘッダだけを見たいのでここでは自前で解決する）
    for (auto& tr : layer.transitions)
    {
        tr._fromAny = (tr.from == "*");
        tr._from = tr._fromAny ? -1 : FindAnimState(layer, tr.from);
        tr._to   = FindAnimState(layer, tr.to);
    }
    return layer;
}

static AnimParamMap BuildParams()
{
    AnimParamMap p;
    p["speed"]    = {AnimParamType::Float,   0.0f, false};
    p["grounded"] = {AnimParamType::Bool,    0.0f, true};
    p["jump"]     = {AnimParamType::Trigger, 0.0f, false};
    return p;
}

static void TestFindState()
{
    const AnimLayerDef layer = BuildLocomotionLayer();
    CHECK(FindAnimState(layer, "Idle") == 0);
    CHECK(FindAnimState(layer, "Locomotion") == 1);
    CHECK(FindAnimState(layer, "Jump") == 2);
    CHECK(FindAnimState(layer, "Nope") == -1);
}

static void TestConditionOps()
{
    AnimParamMap p;
    p["v"] = {AnimParamType::Float, 5.0f, false};
    p["b"] = {AnimParamType::Bool, 0.0f, true};

    CHECK(EvalCondition({"v", AnimCondOp::Gt, 4.0f}, p));
    CHECK(!EvalCondition({"v", AnimCondOp::Gt, 5.0f}, p));
    CHECK(EvalCondition({"v", AnimCondOp::Ge, 5.0f}, p));
    CHECK(EvalCondition({"v", AnimCondOp::Lt, 6.0f}, p));
    CHECK(EvalCondition({"v", AnimCondOp::Le, 5.0f}, p));
    CHECK(EvalCondition({"v", AnimCondOp::Eq, 5.0f}, p));
    CHECK(EvalCondition({"v", AnimCondOp::Ne, 4.0f}, p));
    CHECK(EvalCondition({"b", AnimCondOp::IsTrue, 0.0f}, p));
    CHECK(!EvalCondition({"b", AnimCondOp::IsFalse, 0.0f}, p));

    // 存在しないパラメータは黙って不成立（エラーにしない = 既存 Lua API の流儀）
    CHECK(!EvalCondition({"missing", AnimCondOp::Gt, -1.0f}, p));

    // 条件 0 件は true（exitTime だけで遷移する形）
    CHECK(EvalConditions({}, p));
}

static void TestTransitionFiresOnlyWhenConditionsMet()
{
    const AnimLayerDef layer = BuildLocomotionLayer();
    AnimParamMap p = BuildParams();

    // speed=0 → Idle から動かない
    CHECK(PickTransition(layer, 0, 0.5f, false, true, p) == -1);

    // speed=3 → Idle → Locomotion（遷移 0）
    p["speed"].f = 3.0f;
    CHECK(PickTransition(layer, 0, 0.5f, false, true, p) == 0);

    // Locomotion に居るときは戻らない
    CHECK(PickTransition(layer, 1, 0.5f, false, true, p) == -1);

    // speed が落ちたら Locomotion → Idle（遷移 1）
    p["speed"].f = 0.0f;
    CHECK(PickTransition(layer, 1, 0.5f, false, true, p) == 1);
}

static void TestExitTime()
{
    const AnimLayerDef layer = BuildLocomotionLayer();
    AnimParamMap p = BuildParams();
    p["grounded"].b = true;

    // Jump → Idle は exitTime 0.8。手前では遷移しない
    CHECK(PickTransition(layer, 2, 0.0f, false, true, p) == -1);
    CHECK(PickTransition(layer, 2, 0.79f, false, true, p) == -1);
    CHECK(PickTransition(layer, 2, 0.80f, false, true, p) == 3);
    CHECK(PickTransition(layer, 2, 1.00f, false, true, p) == 3);

    // 条件（grounded）が偽なら exitTime を過ぎても遷移しない
    p["grounded"].b = false;
    CHECK(PickTransition(layer, 2, 1.00f, false, true, p) == -1);
}

static void TestAnyStateTransition()
{
    const AnimLayerDef layer = BuildLocomotionLayer();
    AnimParamMap p = BuildParams();
    p["jump"].b = true;
    p["speed"].f = 3.0f;   // Locomotion → Idle が競合しないようにしておく

    // どのステートからでも Jump へ飛べる（遷移 2）
    // ※ Idle からは遷移 0（Idle→Locomotion, speed>0.1）も成立するが、
    //   宣言順で 0 が先なのでそちらが勝つ。ここでは Any State だけを見たいので
    //   Idle は speed を落として評価する。
    p["speed"].f = 0.0f;
    CHECK(PickTransition(layer, 0, 0.0f, false, true, p) == 2);
    p["speed"].f = 3.0f;
    CHECK(PickTransition(layer, 1, 0.0f, false, true, p) == 2);
    // ただし Jump 自身からは飛ばない（Any State は自分自身へは行かない）
    CHECK(PickTransition(layer, 2, 0.0f, false, true, p) == -1);
}

// 複数の遷移が同時に成立したら「宣言順で先のもの」が勝つ（＝データ側で優先度を決められる）
static void TestDeclarationOrderWins()
{
    const AnimLayerDef layer = BuildLocomotionLayer();
    AnimParamMap p = BuildParams();
    p["speed"].f = 0.0f;   // Locomotion → Idle が成立
    p["jump"].b  = true;   // Any State → Jump も成立

    // 宣言順では Locomotion→Idle（添字 1）が Any→Jump（添字 2）より先
    CHECK(PickTransition(layer, 1, 0.5f, false, true, p) == 1);

    // 「トリガを必ず勝たせたい」なら .animfsm 側で Any State の遷移を先に書く。
    AnimLayerDef reordered = layer;
    std::swap(reordered.transitions[1], reordered.transitions[2]);
    CHECK(PickTransition(reordered, 1, 0.5f, false, true, p) == 1);   // 入れ替え後は添字 1 が Any→Jump
    CHECK(reordered.transitions[1].to == "Jump");
}

static void TestTriggerConsumedOnce()
{
    const AnimLayerDef layer = BuildLocomotionLayer();
    AnimParamMap p = BuildParams();
    p["jump"].b = true;

    const i32 pick = PickTransition(layer, 0, 0.0f, false, true, p);
    CHECK(pick == 2);
    ConsumeTriggers(layer.transitions[static_cast<size_t>(pick)], p);
    CHECK(!p["jump"].b);

    // 消費済みなので 2 回目は発火しない
    CHECK(PickTransition(layer, 0, 0.0f, false, true, p) == -1);

    // ConsumeTriggers は float/bool パラメータには触らない
    p["speed"].f = 3.0f;
    const i32 pick2 = PickTransition(layer, 0, 0.0f, false, true, p);
    CHECK(pick2 == 0);
    ConsumeTriggers(layer.transitions[static_cast<size_t>(pick2)], p);
    CHECK(p["speed"].f == 3.0f);
}

static void TestInterruptible()
{
    const AnimLayerDef layer = BuildLocomotionLayer();
    AnimParamMap p = BuildParams();
    p["jump"].b = true;

    // 割り込み可の遷移が進行中なら、新しい遷移を選べる
    CHECK(PickTransition(layer, 0, 0.0f, /*inTransition*/ true, /*interruptible*/ true, p) == 2);
    // 割り込み不可なら何も選ばない
    CHECK(PickTransition(layer, 0, 0.0f, /*inTransition*/ true, /*interruptible*/ false, p) == -1);
}

static void TestUnresolvedTransitionIgnored()
{
    AnimLayerDef layer;
    layer.states.push_back({"A", "a", true, 1.0f, {}, 0});
    AnimTransitionDef tr;
    tr.from = "A"; tr.to = "DoesNotExist";
    layer.transitions.push_back(tr);
    layer.transitions[0]._from = FindAnimState(layer, "A");
    layer.transitions[0]._to   = FindAnimState(layer, "DoesNotExist");   // -1

    AnimParamMap p;
    // 解決できなかった遷移は黙って無視される（クラッシュも無限ループもしない）
    CHECK(PickTransition(layer, 0, 1.0f, false, true, p) == -1);
}

static void TestInitAnimParams()
{
    AnimGraphAsset asset;
    asset.parameters.push_back({"speed", AnimParamType::Float, 2.5f, false});
    asset.parameters.push_back({"grounded", AnimParamType::Bool, 0.0f, true});
    asset.parameters.push_back({"jump", AnimParamType::Trigger, 0.0f, true});  // 既定 true でも…

    AnimParamMap p;
    InitAnimParams(asset, p);
    CHECK(p.size() == 3);
    CHECK(p["speed"].f == 2.5f);
    CHECK(p["grounded"].b == true);
    CHECK(p["jump"].b == false);   // …トリガは必ず false から始まる（1 フレーム目の暴発防止）
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

    TestFindState();
    TestConditionOps();
    TestTransitionFiresOnlyWhenConditionsMet();
    TestExitTime();
    TestAnyStateTransition();
    TestDeclarationOrderWins();
    TestTriggerConsumedOnce();
    TestInterruptible();
    TestUnresolvedTransitionIgnored();
    TestInitAnimParams();

    std::printf("AnimGraphTests: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
