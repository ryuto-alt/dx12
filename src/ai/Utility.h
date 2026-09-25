#pragma once

// ============================================================================
// ユーティリティ AI の評価器（純ロジック。ECS も Lua も知らない）
// ============================================================================
//   行動（Action）ごとに「考慮事項（Consideration）」を並べ、
//     入力（黒板の数値）→ [lo, hi] で 0..1 へ正規化 → 応答カーブ → 0..1 の得点
//   を掛け合わせて行動の得点にする（Dave Mark の IAUS と同じ。考慮事項が多いほど
//   得点が痩せる問題を「補正係数」で打ち消す）。一番高い行動を選ぶ。
//   ★ヒステリシス: 今の行動にだけ hysteresis を足す（僅差で行ったり来たりしない）
//   ★最低継続時間: 選んでから minCommit 秒は、その行動が done を返さない限り続ける
//   ★クールダウン: done を返した行動は cooldown 秒は 0 点
//   ★同点は定義の早い方（決定論）。選んだ理由と内訳を Decision に全部残す（MCP / デバッグ表示）
// ============================================================================

#include <string>
#include <vector>

#include "ai/Blackboard.h"

namespace dx12e
{
namespace ai
{

enum class CurveType : u8
{
    Linear = 0,   // y = m * (x - c) + b
    Quadratic,    // y = m * (x - c)^k + b（符号は保つ）
    Logistic,     // y = m / (1 + e^(-k (x - c))) + b   既定 k=10, c=0.5（なだらかな S 字）
    Step,         // y = x >= c ? 1 : 0                 既定 c=0.5
    Inverse,      // y = 1 - x
    Smooth,       // y = smoothstep(0, 1, x)
};
bool        ParseCurveType(const std::string& s, CurveType& out);
const char* CurveTypeName(CurveType t);

struct ResponseCurve
{
    CurveType type = CurveType::Linear;
    f32 m = 1.0f, k = 1.0f, b = 0.0f, c = 0.0f;
    bool invert = false;

    // 型ごとの既定の係数（logistic の k=10, c=0.5 など）
    static ResponseCurve Make(CurveType t);
    // x は 0..1 に切り詰めてから評価。結果も 0..1 に切り詰める（NaN は 0）
    f32 Evaluate(f32 x) const;
};

struct Consideration
{
    std::string name;       // 表示用（省略時は input）
    std::string input;      // 黒板のキー（数値 or 真偽）
    f32 lo = 0.0f, hi = 1.0f;   // 入力をこの範囲で 0..1 へ
    f32 fallback = 0.0f;    // 黒板に無い時の入力値
    ResponseCurve curve;
};

struct ActionDef
{
    std::string name;
    f32 weight = 1.0f;
    std::vector<Consideration> considerations;
    f32 cooldown = 0.0f;      // done を返した後、この秒数は選ばない
    f32 minDuration = 0.0f;   // 選んだらこの秒数は続ける（Brain 全体の minCommit と大きい方）
};

struct ConsiderationEval
{
    std::string name, input;
    f64 raw = 0.0;      // 黒板の値
    f32 x = 0.0f;       // 正規化後
    f32 score = 0.0f;   // カーブの出力
    bool missing = false;
};

struct ActionEval
{
    std::string name;
    f32 weight = 1.0f;
    f32 product = 1.0f;   // 補正後の考慮事項の積
    f32 score = 0.0f;     // product * weight
    f32 bonus = 0.0f;     // ヒステリシスの加点
    f32 final = 0.0f;     // 選択に使った値
    bool cooldown = false;
    std::vector<ConsiderationEval> considerations;
};

enum class DecisionReason : u8
{
    None = 0,     // 選べる行動が無い（全部 0 点）
    Best,         // 一番高い
    Hysteresis,   // 加点が無ければ別の行動だったが、今の行動を続けた
    Commit,       // 最低継続時間の内側なので続けた
    Forced,       // スクリプトが force した
    NoActions,    // 行動が 1 つも定義されていない
};
const char* DecisionReasonName(DecisionReason r);

struct Decision
{
    i32 chosen = -1;
    i32 previous = -1;
    f64 time = 0.0;
    DecisionReason reason = DecisionReason::None;
    std::vector<ActionEval> actions;
};

// 1 つの行動の得点（out に内訳）
f32 ScoreAction(const ActionDef& a, const Blackboard& bb, ActionEval* out);

struct SelectInput
{
    i32 current = -1;          // 今の行動
    bool currentDone = false;  // 今の行動が done を返した
    f64 now = 0.0;
    f64 enteredAt = 0.0;       // 今の行動に入った時刻
    f32 hysteresis = 0.1f;
    f32 minCommit = 0.0f;
    const std::vector<f64>* cooldownUntil = nullptr;   // 行動ごとの「この時刻まで選ばない」
};

Decision SelectAction(const std::vector<ActionDef>& defs, const Blackboard& bb, const SelectInput& in);

} // namespace ai
} // namespace dx12e
