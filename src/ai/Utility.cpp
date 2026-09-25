#include "ai/Utility.h"

#include <algorithm>
#include <cmath>

namespace dx12e
{
namespace ai
{

bool ParseCurveType(const std::string& s, CurveType& out)
{
    if (s == "linear")                      { out = CurveType::Linear; return true; }
    if (s == "quadratic" || s == "poly")    { out = CurveType::Quadratic; return true; }
    if (s == "logistic" || s == "sigmoid")  { out = CurveType::Logistic; return true; }
    if (s == "step" || s == "bool")         { out = CurveType::Step; return true; }
    if (s == "inverse")                     { out = CurveType::Inverse; return true; }
    if (s == "smooth" || s == "smoothstep") { out = CurveType::Smooth; return true; }
    return false;
}

const char* CurveTypeName(CurveType t)
{
    switch (t)
    {
    case CurveType::Linear:    return "linear";
    case CurveType::Quadratic: return "quadratic";
    case CurveType::Logistic:  return "logistic";
    case CurveType::Step:      return "step";
    case CurveType::Inverse:   return "inverse";
    case CurveType::Smooth:    return "smooth";
    }
    return "linear";
}

ResponseCurve ResponseCurve::Make(CurveType t)
{
    ResponseCurve r;
    r.type = t;
    switch (t)
    {
    case CurveType::Quadratic: r.k = 2.0f; break;
    case CurveType::Logistic:  r.k = 10.0f; r.c = 0.5f; break;
    case CurveType::Step:      r.c = 0.5f; break;
    default: break;
    }
    return r;
}

f32 ResponseCurve::Evaluate(f32 x) const
{
    if (!(x == x)) x = 0.0f;   // NaN
    x = std::clamp(x, 0.0f, 1.0f);
    f32 y = 0.0f;
    switch (type)
    {
    case CurveType::Linear:
        y = m * (x - c) + b;
        break;
    case CurveType::Quadratic:
    {
        const f32 u = x - c;
        const f32 p = std::pow(std::fabs(u), k);
        y = m * (u < 0.0f ? -p : p) + b;
        break;
    }
    case CurveType::Logistic:
        y = m / (1.0f + std::exp(-k * (x - c))) + b;
        break;
    case CurveType::Step:
        y = (x >= c) ? 1.0f : 0.0f;
        break;
    case CurveType::Inverse:
        y = 1.0f - x;
        break;
    case CurveType::Smooth:
        y = x * x * (3.0f - 2.0f * x);
        break;
    }
    if (invert) y = 1.0f - y;
    if (!(y == y)) y = 0.0f;
    return std::clamp(y, 0.0f, 1.0f);
}

const char* DecisionReasonName(DecisionReason r)
{
    switch (r)
    {
    case DecisionReason::None:       return "none";
    case DecisionReason::Best:       return "best";
    case DecisionReason::Hysteresis: return "hysteresis";
    case DecisionReason::Commit:     return "commit";
    case DecisionReason::Forced:     return "forced";
    case DecisionReason::NoActions:  return "noActions";
    }
    return "none";
}

f32 ScoreAction(const ActionDef& a, const Blackboard& bb, ActionEval* out)
{
    if (out)
    {
        out->name = a.name;
        out->weight = a.weight;
        out->considerations.clear();
    }
    const size_t n = a.considerations.size();
    // 考慮事項が多いほど積が痩せるのを打ち消す補正（IAUS の compensation factor）
    const f32 modification = (n > 0) ? 1.0f - 1.0f / static_cast<f32>(n) : 0.0f;
    f32 product = 1.0f;
    for (const Consideration& c : a.considerations)
    {
        bool ok = false;
        const f64 raw = bb.Number(c.input, static_cast<f64>(c.fallback), &ok);
        f32 x = 0.0f;
        if (c.hi != c.lo) x = static_cast<f32>((raw - c.lo) / (static_cast<f64>(c.hi) - c.lo));
        else x = (raw >= c.lo) ? 1.0f : 0.0f;
        x = std::clamp(x, 0.0f, 1.0f);
        const f32 s = c.curve.Evaluate(x);
        const f32 makeUp = (1.0f - s) * modification;
        product *= s + makeUp * s;
        if (out)
        {
            ConsiderationEval ce;
            ce.name = c.name.empty() ? c.input : c.name;
            ce.input = c.input;
            ce.raw = raw;
            ce.x = x;
            ce.score = s;
            ce.missing = !ok;
            out->considerations.push_back(std::move(ce));
        }
    }
    const f32 score = product * a.weight;
    if (out)
    {
        out->product = product;
        out->score = score;
    }
    return score;
}

Decision SelectAction(const std::vector<ActionDef>& defs, const Blackboard& bb, const SelectInput& in)
{
    Decision d;
    d.previous = in.current;
    d.time = in.now;
    if (defs.empty())
    {
        d.reason = DecisionReason::NoActions;
        return d;
    }
    const i32 n = static_cast<i32>(defs.size());
    const bool curValid = in.current >= 0 && in.current < n;
    d.actions.resize(defs.size());

    i32 best = -1, bestNoBonus = -1;
    f32 bestVal = 0.0f, bestNoBonusVal = 0.0f;
    for (i32 i = 0; i < n; ++i)
    {
        ActionEval& ae = d.actions[static_cast<size_t>(i)];
        const f32 score = ScoreAction(defs[static_cast<size_t>(i)], bb, &ae);
        const bool cd = in.cooldownUntil && static_cast<size_t>(i) < in.cooldownUntil->size() &&
                        (*in.cooldownUntil)[static_cast<size_t>(i)] > in.now;
        ae.cooldown = cd;
        ae.bonus = (i == in.current && !in.currentDone && !cd) ? in.hysteresis : 0.0f;
        ae.final = cd ? 0.0f : score + ae.bonus;
        const f32 plain = cd ? 0.0f : score;
        // 同点は先に定義した方（strict >）
        if (ae.final > bestVal) { bestVal = ae.final; best = i; }
        if (plain > bestNoBonusVal) { bestNoBonusVal = plain; bestNoBonus = i; }
    }

    // 最低継続時間の内側なら今の行動を続ける（done を返した / クールダウン中は除く）
    if (curValid && !in.currentDone && !d.actions[static_cast<size_t>(in.current)].cooldown)
    {
        const f32 commit = (std::max)(in.minCommit, defs[static_cast<size_t>(in.current)].minDuration);
        if (in.now - in.enteredAt < static_cast<f64>(commit))
        {
            d.chosen = in.current;
            d.reason = DecisionReason::Commit;
            return d;
        }
    }
    if (best < 0)
    {
        d.chosen = -1;
        d.reason = DecisionReason::None;
        return d;
    }
    d.chosen = best;
    d.reason = (best == in.current && bestNoBonus != in.current) ? DecisionReason::Hysteresis
                                                                 : DecisionReason::Best;
    return d;
}

} // namespace ai
} // namespace dx12e
