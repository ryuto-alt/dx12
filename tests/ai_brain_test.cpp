// ゲーム AI（Brain）の純ロジックの単体テスト: 黒板 / 応答カーブ / ユーティリティ評価 /
// 知覚の判定 / 記憶 / シード付き乱数。ECS も Lua も物理も要らない（.cpp を直接ビルド）。
// 実行: ctest --output-on-failure -R AiBrainTests
//
// ここで守りたい不変条件:
//   ① 黒板: 型ごとに読み書きでき、同じ値の書き込みは版を進めない。列挙は辞書順（毎回同じ）
//   ② カーブ: 端点・反転・S 字の中点・段差・切り詰め・NaN
//   ③ 行動の得点: 考慮事項の積 × 重み。考慮事項が多くても痩せすぎない補正。無い入力は fallback
//   ④ 選択: 一番高い行動 / ヒステリシスで粘る / 超えたら切り替える / 最低継続 / クールダウン / 同点は先勝ち
//      と、その「理由」と内訳
//   ⑤ 視覚: 距離・視野角・気配（背後でも近ければ候補）
//   ⑥ 聴覚: 半径 × 聴力、壁越しは occlusion 倍、強さは距離で下がる
//   ⑦ 確認時間: confirmTime 秒見え続けて初めて awareness = 1、見えなくなれば下がる
//   ⑧ 記憶: 刺激が無ければ memoryTime 秒で忘れる。見つけている最中の位置は音で上書きしない
//   ⑨ 乱数: 同じ種なら同じ列、範囲の内側

#include "ai/AiRandom.h"
#include "ai/Blackboard.h"
#include "ai/Perception.h"
#include "ai/Utility.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::ai;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace
{
bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

Consideration Cons(const std::string& input, CurveType t, float lo = 0.0f, float hi = 1.0f, bool invert = false)
{
    Consideration c;
    c.input = input;
    c.lo = lo;
    c.hi = hi;
    c.curve = ResponseCurve::Make(t);
    c.curve.invert = invert;
    return c;
}
} // namespace

int main()
{
    // ================= ① 黒板 =================
    {
        Blackboard bb;
        bb.SetNumber("hp", 42.0);
        bb.SetBool("alert", true);
        bb.SetString("mode", "chase");
        bb.SetVec3("pos", 1.0f, 2.0f, 3.0f);
        bb.SetEntity("target", 7);
        CHECK(bb.Size() == 5);
        CHECK(bb.Number("hp", 0.0) == 42.0);
        CHECK(bb.Number("alert", 0.0) == 1.0);          // 真偽は数値として 1/0
        bool ok = true;
        CHECK(bb.Number("mode", -1.0, &ok) == -1.0 && !ok);   // 文字列は数値ではない
        CHECK(bb.Bool("alert", false));
        BbVec3 p;
        CHECK(bb.Vec3("pos", p) && p.x == 1.0f && p.z == 3.0f);
        CHECK(TypeOf(*bb.Find("target")) == BbType::Entity);
        const u64 rev = bb.Revision();
        bb.SetNumber("hp", 42.0);                        // 同じ値 → 版は進まない
        CHECK(bb.Revision() == rev);
        bb.SetNumber("hp", 41.0);
        CHECK(bb.Revision() == rev + 1);
        bb.Set("hp", BbValue{});                         // 値なし = 消す
        CHECK(!bb.Has("hp"));
        // 列挙は辞書順
        std::string order;
        for (const auto& kv : bb.All()) order += kv.first + ",";
        CHECK(order == "alert,mode,pos,target,");
    }

    // ================= ② カーブ =================
    {
        ResponseCurve lin = ResponseCurve::Make(CurveType::Linear);
        CHECK(Near(lin.Evaluate(0.0f), 0.0) && Near(lin.Evaluate(1.0f), 1.0) && Near(lin.Evaluate(0.25f), 0.25));
        lin.invert = true;
        CHECK(Near(lin.Evaluate(0.25f), 0.75));
        CHECK(Near(lin.Evaluate(2.0f), 0.0));            // x は 0..1 に切り詰める
        const ResponseCurve lg = ResponseCurve::Make(CurveType::Logistic);
        CHECK(Near(lg.Evaluate(0.5f), 0.5, 1e-3));
        CHECK(lg.Evaluate(0.1f) < 0.05f && lg.Evaluate(0.9f) > 0.95f);
        const ResponseCurve st = ResponseCurve::Make(CurveType::Step);
        CHECK(st.Evaluate(0.49f) == 0.0f && st.Evaluate(0.5f) == 1.0f);
        const ResponseCurve q = ResponseCurve::Make(CurveType::Quadratic);
        CHECK(Near(q.Evaluate(0.5f), 0.25));
        const ResponseCurve sm = ResponseCurve::Make(CurveType::Smooth);
        CHECK(Near(sm.Evaluate(0.5f), 0.5) && sm.Evaluate(0.1f) < 0.1f);
        CHECK(lin.Evaluate(std::numeric_limits<float>::quiet_NaN()) >= 0.0f);
        CurveType t;
        CHECK(ParseCurveType("logistic", t) && t == CurveType::Logistic);
        CHECK(ParseCurveType("bool", t) && t == CurveType::Step);
        CHECK(!ParseCurveType("nope", t));
    }

    // ================= ③ 行動の得点 =================
    {
        Blackboard bb;
        bb.SetNumber("a", 0.5);
        bb.SetNumber("b", 0.5);
        ActionDef one;
        one.name = "one";
        one.considerations = { Cons("a", CurveType::Linear) };
        CHECK(Near(ScoreAction(one, bb, nullptr), 0.5));
        ActionDef two;
        two.name = "two";
        two.weight = 2.0f;
        two.considerations = { Cons("a", CurveType::Linear), Cons("b", CurveType::Linear) };
        ActionEval ev;
        const float s2 = ScoreAction(two, bb, &ev);
        // 補正なしなら 0.25*2 = 0.5。補正（1 - 1/n = 0.5）で各 0.5 → 0.625 → 積 0.390625 → ×2
        CHECK(Near(s2, 0.78125));
        CHECK(ev.considerations.size() == 2);
        CHECK(Near(ev.considerations[0].x, 0.5) && Near(ev.considerations[0].score, 0.5));
        // 範囲の正規化と、無い入力の fallback
        ActionDef rng;
        rng.name = "range";
        Consideration c = Cons("dist", CurveType::Linear, 0.0f, 20.0f, true);
        c.fallback = 20.0f;
        rng.considerations = { c };
        ActionEval ev2;
        CHECK(Near(ScoreAction(rng, bb, &ev2), 0.0));    // 無い → 20m 扱い → 反転で 0
        CHECK(ev2.considerations[0].missing);
        bb.SetNumber("dist", 5.0);
        CHECK(Near(ScoreAction(rng, bb, nullptr), 0.75));
        // 考慮事項なし = 重みそのもの
        ActionDef idle;
        idle.name = "idle";
        idle.weight = 0.2f;
        CHECK(Near(ScoreAction(idle, bb, nullptr), 0.2));
    }

    // ================= ④ 選択 =================
    {
        Blackboard bb;
        std::vector<ActionDef> defs(3);
        defs[0].name = "wander"; defs[0].weight = 0.3f;
        defs[1].name = "chase";  defs[1].considerations = { Cons("seen", CurveType::Step) };
        defs[2].name = "search"; defs[2].considerations = { Cons("known", CurveType::Linear) };
        std::vector<double> cd(defs.size(), -1e9);
        SelectInput in;
        in.hysteresis = 0.1f;
        in.minCommit = 0.0f;
        in.cooldownUntil = &cd;

        // 何も分からない → 徘徊
        Decision d = SelectAction(defs, bb, in);
        CHECK(d.chosen == 0 && d.reason == DecisionReason::Best);
        CHECK(d.actions.size() == 3 && Near(d.actions[0].final, 0.3));

        // 見つけた → 追跡
        bb.SetBool("seen", true);
        in.current = 0;
        d = SelectAction(defs, bb, in);
        CHECK(d.chosen == 1);

        // 追跡中、search が 1.05 でも粘りで追跡のまま（理由 = hysteresis）
        bb.SetBool("seen", true);
        defs[2].weight = 1.05f;
        bb.SetNumber("known", 1.0);
        in.current = 1;
        d = SelectAction(defs, bb, in);
        CHECK(d.chosen == 1 && d.reason == DecisionReason::Hysteresis);
        CHECK(Near(d.actions[1].bonus, 0.1) && Near(d.actions[1].final, 1.1));
        // 差が粘りを超えたら切り替える
        defs[2].weight = 1.2f;
        d = SelectAction(defs, bb, in);
        CHECK(d.chosen == 2 && d.reason == DecisionReason::Best);

        // 最低継続時間の内側なら続ける（理由 = commit）
        in.minCommit = 1.0f;
        in.now = 10.5; in.enteredAt = 10.0;
        d = SelectAction(defs, bb, in);
        CHECK(d.chosen == 1 && d.reason == DecisionReason::Commit);
        // done を返した行動は最低継続でも粘らない
        in.currentDone = true;
        d = SelectAction(defs, bb, in);
        CHECK(d.chosen == 2);
        in.currentDone = false;
        in.minCommit = 0.0f;

        // クールダウン中は 0 点
        cd[2] = 100.0;
        in.now = 50.0;
        d = SelectAction(defs, bb, in);
        CHECK(d.actions[2].cooldown && d.actions[2].final == 0.0f);
        CHECK(d.chosen == 1);
        cd[2] = -1e9;

        // 同点は先に定義した方
        std::vector<ActionDef> tie(2);
        tie[0].name = "a"; tie[1].name = "b";
        SelectInput in2;
        d = SelectAction(tie, bb, in2);
        CHECK(d.chosen == 0);
        // 全部 0 点 → 選べない
        tie[0].weight = 0.0f; tie[1].weight = 0.0f;
        d = SelectAction(tie, bb, in2);
        CHECK(d.chosen == -1 && d.reason == DecisionReason::None);
        // 定義なし
        d = SelectAction({}, bb, in2);
        CHECK(d.reason == DecisionReason::NoActions);
        CHECK(std::string(DecisionReasonName(DecisionReason::Hysteresis)) == "hysteresis");
    }

    // ================= ⑤ 視覚 =================
    {
        const float eye[3] = { 0, 1.6f, 0 };
        // 前方（yaw 0 = +z）10m
        const float ahead[3] = { 0, 1.0f, 10.0f };
        ViewCheck v = CheckView(eye, 0.0f, ahead, 20.0f, 140.0f, 2.0f);
        CHECK(v.inRange && v.inFov && !v.isNear && v.Candidate());
        CHECK(Near(v.dist, 10.0, 1e-3) && Near(v.angleDeg, 0.0, 1e-2));
        // 真後ろ 10m → 視野外
        const float behind[3] = { 0, 1.0f, -10.0f };
        v = CheckView(eye, 0.0f, behind, 20.0f, 140.0f, 2.0f);
        CHECK(!v.inFov && !v.Candidate());
        // 真後ろ 1.5m → 気配で候補
        const float close[3] = { 0, 1.0f, -1.5f };
        v = CheckView(eye, 0.0f, close, 20.0f, 140.0f, 2.0f);
        CHECK(v.isNear && v.Candidate());
        // 範囲外
        const float farPt[3] = { 0, 1.0f, 30.0f };
        v = CheckView(eye, 0.0f, farPt, 20.0f, 140.0f, 2.0f);
        CHECK(!v.inRange && !v.Candidate());
        // 向きを 90 度回すと +x が正面
        const float right[3] = { 10.0f, 1.0f, 0.0f };
        v = CheckView(eye, 90.0f, right, 20.0f, 60.0f, 0.0f);
        CHECK(v.inFov && Near(v.angleDeg, 0.0, 1e-2));
        // 視野角の境目（全角 60 → 半角 30）
        const float edge[3] = { std::sin(0.5236f + 0.02f) * 10.0f, 1.0f, std::cos(0.5236f + 0.02f) * 10.0f };
        v = CheckView(eye, 0.0f, edge, 20.0f, 60.0f, 0.0f);
        CHECK(!v.inFov);
    }

    // ================= ⑥ 聴覚 =================
    {
        CHECK(Near(HearingReach(10.0f, 1.0f, false, 0.5f), 10.0));
        CHECK(Near(HearingReach(10.0f, 1.5f, false, 0.5f), 15.0));
        CHECK(Near(HearingReach(10.0f, 1.0f, true, 0.5f), 5.0));     // 壁越しは半分
        float loud = -1.0f;
        CHECK(Hears(4.0f, 8.0f, &loud) && Near(loud, 0.5));
        CHECK(!Hears(9.0f, 8.0f, &loud) && loud == 0.0f);
        // 6m 先の半径 10 の音: 開けていれば聞こえ、壁越し（×0.5）なら聞こえない
        CHECK(Hears(6.0f, HearingReach(10.0f, 1.0f, false, 0.5f), nullptr));
        CHECK(!Hears(6.0f, HearingReach(10.0f, 1.0f, true, 0.5f), nullptr));
    }

    // ================= ⑦ 確認時間 =================
    {
        const float dt = 1.0f / 60.0f;
        float a = 0.0f;
        int steps = 0;
        while (a < 1.0f && steps < 1000) { a = StepAwareness(a, true, dt, 0.3f, 0.5f); ++steps; }
        CHECK(steps == 18 || steps == 19);                 // 0.3 秒 = 18 ステップ（丸め誤差で 19）
        // 途中で見失うと下がる（0.5/秒）
        a = 0.6f;
        for (int i = 0; i < 60; ++i) a = StepAwareness(a, false, dt, 0.3f, 0.5f);
        CHECK(Near(a, 0.1, 1e-3));
        // 確認時間 0 は即座に 1
        CHECK(StepAwareness(0.0f, true, dt, 0.0f, 0.5f) == 1.0f);
    }

    // ================= ⑧ 記憶 =================
    {
        TargetMemory m;
        const float p1[3] = { 1, 0, 2 };
        RememberSeen(m, p1, 10.0);
        CHECK(m.known && m.lastSeen == 10.0 && m.knownPos[2] == 2.0f);
        // 見つけている最中の位置は音で上書きしない
        m.seen = true;
        const float p2[3] = { 9, 0, 9 };
        RememberHeard(m, p2, 11.0);
        CHECK(m.knownPos[0] == 1.0f && m.lastHeard == 11.0);
        // 見失ったら、音で居場所を更新する
        m.seen = false;
        RememberHeard(m, p2, 12.0);
        CHECK(m.knownPos[0] == 9.0f);
        // 刺激が無ければ memoryTime で忘れる（見つけている間は忘れない）
        CHECK(!ForgetIfStale(m, 20.0, 10.0f));
        CHECK(ForgetIfStale(m, 22.5, 10.0f) && !m.known);
        m.seen = true; m.known = true; m.lastStimulus = 0.0;
        CHECK(!ForgetIfStale(m, 100.0, 10.0f));
    }

    // ================= ⑨ 乱数 =================
    {
        Rng a(42), b(42), c(43);
        bool same = true, differ = false;
        for (int i = 0; i < 100; ++i)
        {
            const u64 x = a.Next(), y = b.Next(), z = c.Next();
            same = same && (x == y);
            differ = differ || (x != z);
        }
        CHECK(same && differ);
        Rng r(7);
        bool inRange = true;
        for (int i = 0; i < 1000; ++i)
        {
            const double u = r.Uniform();
            const long long k = static_cast<long long>(r.Int(-2, 3));
            inRange = inRange && u >= 0.0 && u < 1.0 && k >= -2 && k <= 3;
        }
        CHECK(inRange);
    }

    std::printf("ai_brain_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
