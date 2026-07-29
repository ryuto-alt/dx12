#include "animation/AnimGraphAsset.h"

#include <nlohmann/json.hpp>
#include <algorithm>

using nlohmann::json;

namespace dx12e
{

namespace
{

AnimParamType ParseParamType(const std::string& s)
{
    if (s == "bool")    return AnimParamType::Bool;
    if (s == "trigger") return AnimParamType::Trigger;
    return AnimParamType::Float;
}

const char* ParamTypeName(AnimParamType t)
{
    switch (t)
    {
    case AnimParamType::Bool:    return "bool";
    case AnimParamType::Trigger: return "trigger";
    default:                     return "float";
    }
}

// 条件演算子。未知の文字列は "gt" 扱い（黙って落とすより見えるバグにする）。
AnimCondOp ParseCondOp(const std::string& s)
{
    if (s == "lt")      return AnimCondOp::Lt;
    if (s == "ge")      return AnimCondOp::Ge;
    if (s == "le")      return AnimCondOp::Le;
    if (s == "eq")      return AnimCondOp::Eq;
    if (s == "ne")      return AnimCondOp::Ne;
    if (s == "isTrue")  return AnimCondOp::IsTrue;
    if (s == "isFalse") return AnimCondOp::IsFalse;
    if (s == "trigger") return AnimCondOp::Trigger;
    return AnimCondOp::Gt;
}

const char* CondOpName(AnimCondOp op)
{
    switch (op)
    {
    case AnimCondOp::Lt:      return "lt";
    case AnimCondOp::Ge:      return "ge";
    case AnimCondOp::Le:      return "le";
    case AnimCondOp::Eq:      return "eq";
    case AnimCondOp::Ne:      return "ne";
    case AnimCondOp::IsTrue:  return "isTrue";
    case AnimCondOp::IsFalse: return "isFalse";
    case AnimCondOp::Trigger: return "trigger";
    default:                  return "gt";
    }
}

void ParseConditions(const json& j, std::vector<AnimCondition>& out)
{
    if (!j.is_array()) return;
    for (const auto& cj : j)
    {
        if (!cj.is_object()) continue;
        AnimCondition c;
        c.param = cj.value("param", std::string());
        c.op    = ParseCondOp(cj.value("op", std::string("gt")));
        c.value = cj.value("value", 0.0f);
        if (!c.param.empty()) out.push_back(std::move(c));
    }
}

void ParseBlendTree(const json& j, AnimBlendTree& out)
{
    if (!j.is_object()) return;
    const std::string type = j.value("type", std::string("1d"));
    if      (type == "1d")      out.type = AnimBlendTreeType::OneD;
    else if (type == "2d")      out.type = AnimBlendTreeType::TwoD;        // Freeform Cartesian
    else if (type == "2dPolar") out.type = AnimBlendTreeType::TwoDPolar;   // Freeform Directional
    else return;                                                          // 未知の type は無視

    const bool is2D = (out.type != AnimBlendTreeType::OneD);
    out.param      = j.value("param", std::string());
    out.paramY     = j.value("paramY", std::string());
    out.polarAlpha = j.value("polarAlpha", 2.0f);
    out.syncPhase  = j.value("syncPhase", true);
    out.speedMatch = j.value("speedMatch", false);
    // speedMatch は「param の値＝そのクリップ本来の移動速度」を前提にした 1D 専用の補正。
    // 2D はどの軸が速度なのかツリー次第で決まらないので黙って無視する。
    if (is2D) out.speedMatch = false;

    if (j.contains("samples") && j["samples"].is_array())
    {
        for (const auto& sj : j["samples"])
        {
            if (!sj.is_object()) continue;
            AnimBlendSample1D s;
            s.value  = sj.value("value",  0.0f);
            s.valueY = sj.value("valueY", 0.0f);
            s.clip   = sj.value("clip", std::string());
            s.speed  = sj.value("speed", 1.0f);
            if (!s.clip.empty()) out.samples.push_back(std::move(s));
        }
    }
    // 1D だけソートする。Eval1DBlend が「value 昇順」を前提にしているため。
    // 2D の Gradient Band は並び順に依存しないので、記述順を保って読みやすさを優先する。
    if (!is2D)
        std::stable_sort(out.samples.begin(), out.samples.end(),
                         [](const AnimBlendSample1D& a, const AnimBlendSample1D& b) { return a.value < b.value; });

    if (out.samples.empty()) out.type = AnimBlendTreeType::None;
    // 2D なのに Y 軸パラメータが無いと、全サンプルが y=0 の直線上で評価されて
    // 1D と区別が付かない結果になる。黙って変な絵を出すより 1D として扱う方がまし。
    if (is2D && out.paramY.empty()) out.type = AnimBlendTreeType::OneD;
}

void ParseLayer(const json& j, AnimLayerDef& out)
{
    out.name   = j.value("name", std::string("Base"));
    out.weight = j.value("weight", 1.0f);
    out.blend  = (j.value("blend", std::string("override")) == "additive")
               ? AnimLayerBlend::Additive : AnimLayerBlend::Override;
    out.referenceClip = j.value("referenceClip", std::string());
    out.defaultState  = j.value("defaultState", std::string());

    if (j.contains("mask") && j["mask"].is_object())
    {
        const json& mj = j["mask"];
        out.hasMask = true;
        out.mask.includeChildren = mj.value("includeChildren", true);
        out.mask.weight = mj.value("weight", 1.0f);
        if (mj.contains("include") && mj["include"].is_array())
            for (const auto& n : mj["include"])
                if (n.is_string()) out.mask.include.push_back(n.get<std::string>());
        if (out.mask.include.empty()) out.hasMask = false;
    }

    if (j.contains("states") && j["states"].is_array())
    {
        for (const auto& sj : j["states"])
        {
            if (!sj.is_object()) continue;
            AnimStateDef st;
            st.name  = sj.value("name", std::string());
            st.clip  = sj.value("clip", std::string());
            st.loop  = sj.value("loop", true);
            st.speed = sj.value("speed", 1.0f);
            if (sj.contains("blendTree")) ParseBlendTree(sj["blendTree"], st.blendTree);
            if (!st.name.empty()) out.states.push_back(std::move(st));
        }
    }

    if (j.contains("transitions") && j["transitions"].is_array())
    {
        for (const auto& tj : j["transitions"])
        {
            if (!tj.is_object()) continue;
            AnimTransitionDef tr;
            tr.from     = tj.value("from", std::string());
            tr.to       = tj.value("to", std::string());
            tr.duration = tj.value("duration", 0.2f);
            tr.interruptible = tj.value("interruptible", true);
            if (tj.contains("exitTime") && tj["exitTime"].is_number())
            {
                tr.hasExitTime = true;
                tr.exitTime    = tj["exitTime"].get<float>();
            }
            if (tj.contains("conditions")) ParseConditions(tj["conditions"], tr.conditions);
            if (!tr.to.empty()) out.transitions.push_back(std::move(tr));
        }
    }

    if (out.defaultState.empty() && !out.states.empty())
        out.defaultState = out.states.front().name;
}

json BlendTreeToJson(const AnimBlendTree& bt)
{
    json j;
    const bool is2D = (bt.type == AnimBlendTreeType::TwoD || bt.type == AnimBlendTreeType::TwoDPolar);
    j["type"]       = (bt.type == AnimBlendTreeType::TwoDPolar) ? "2dPolar"
                    : (bt.type == AnimBlendTreeType::TwoD)      ? "2d" : "1d";
    j["param"]      = bt.param;
    j["syncPhase"]  = bt.syncPhase;
    j["speedMatch"] = bt.speedMatch;
    if (is2D)
    {
        j["paramY"] = bt.paramY;
        if (bt.type == AnimBlendTreeType::TwoDPolar) j["polarAlpha"] = bt.polarAlpha;
    }
    json samples = json::array();
    for (const auto& s : bt.samples)
    {
        json sj{{"value", s.value}, {"clip", s.clip}, {"speed", s.speed}};
        if (is2D) sj["valueY"] = s.valueY;
        samples.push_back(std::move(sj));
    }
    j["samples"] = std::move(samples);
    return j;
}

} // namespace

// ---------------------------------------------------------------------------
bool ParseAnimGraphAsset(const std::vector<uint8_t>& jsonBytes, AnimGraphAsset& out, std::string& err)
{
    out = AnimGraphAsset{};
    json j;
    try
    {
        j = json::parse(jsonBytes.begin(), jsonBytes.end(), nullptr, true, true);  // 末尾コメント許可
    }
    catch (const std::exception& e)
    {
        err = e.what();
        return false;
    }
    if (!j.is_object()) { err = "root is not an object"; return false; }

    out.version = j.value("version", 1);

    if (j.contains("parameters") && j["parameters"].is_array())
    {
        for (const auto& pj : j["parameters"])
        {
            if (!pj.is_object()) continue;
            AnimParamDef p;
            p.name = pj.value("name", std::string());
            p.type = ParseParamType(pj.value("type", std::string("float")));
            if (pj.contains("default"))
            {
                if (pj["default"].is_boolean()) p.defBool = pj["default"].get<bool>();
                else if (pj["default"].is_number()) p.defFloat = pj["default"].get<float>();
            }
            if (!p.name.empty()) out.parameters.push_back(std::move(p));
        }
    }

    if (j.contains("clipEvents") && j["clipEvents"].is_object())
    {
        for (auto it = j["clipEvents"].begin(); it != j["clipEvents"].end(); ++it)
        {
            if (!it.value().is_array()) continue;
            AnimGraphClipEvents ce;
            ce.clip = it.key();
            for (const auto& ej : it.value())
            {
                if (!ej.is_object()) continue;
                AnimEvent ev;
                ev.time        = ej.value("time", 0.0f);
                ev.name        = ej.value("name", std::string());
                ev.stringParam = ej.value("string", std::string());
                ev.floatParam  = ej.value("float", 0.0f);
                if (!ev.name.empty()) ce.events.push_back(std::move(ev));
            }
            std::stable_sort(ce.events.begin(), ce.events.end(),
                             [](const AnimEvent& a, const AnimEvent& b) { return a.time < b.time; });
            if (!ce.events.empty()) out.clipEvents.push_back(std::move(ce));
        }
    }

    if (j.contains("extraClips") && j["extraClips"].is_array())
    {
        for (const auto& ej : j["extraClips"])
        {
            if (!ej.is_object()) continue;
            AnimGraphExtraClip ec;
            ec.path = ej.value("path", std::string());
            ec.name = ej.value("name", std::string());
            if (!ec.path.empty()) out.extraClips.push_back(std::move(ec));
        }
    }

    if (j.contains("layers") && j["layers"].is_array())
    {
        for (const auto& lj : j["layers"])
        {
            if (!lj.is_object()) continue;
            AnimLayerDef layer;
            ParseLayer(lj, layer);
            out.layers.push_back(std::move(layer));
        }
    }

    if (out.layers.empty()) { err = "no layers"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
std::string SerializeAnimGraphAsset(const AnimGraphAsset& asset)
{
    json j;
    j["version"] = asset.version;

    json params = json::array();
    for (const auto& p : asset.parameters)
    {
        json pj;
        pj["name"] = p.name;
        pj["type"] = ParamTypeName(p.type);
        if (p.type == AnimParamType::Float)     pj["default"] = p.defFloat;
        else if (p.type == AnimParamType::Bool) pj["default"] = p.defBool;
        params.push_back(std::move(pj));
    }
    j["parameters"] = std::move(params);

    if (!asset.clipEvents.empty())
    {
        json ce = json::object();
        for (const auto& c : asset.clipEvents)
        {
            json arr = json::array();
            for (const auto& e : c.events)
                arr.push_back({{"time", e.time}, {"name", e.name},
                               {"string", e.stringParam}, {"float", e.floatParam}});
            ce[c.clip] = std::move(arr);
        }
        j["clipEvents"] = std::move(ce);
    }

    if (!asset.extraClips.empty())
    {
        json arr = json::array();
        for (const auto& e : asset.extraClips)
            arr.push_back({{"path", e.path}, {"name", e.name}});
        j["extraClips"] = std::move(arr);
    }

    json layers = json::array();
    for (const auto& l : asset.layers)
    {
        json lj;
        lj["name"]   = l.name;
        lj["weight"] = l.weight;
        lj["blend"]  = (l.blend == AnimLayerBlend::Additive) ? "additive" : "override";
        if (!l.referenceClip.empty()) lj["referenceClip"] = l.referenceClip;
        lj["defaultState"] = l.defaultState;
        if (l.hasMask)
        {
            json mj;
            mj["include"] = l.mask.include;
            mj["includeChildren"] = l.mask.includeChildren;
            mj["weight"] = l.mask.weight;
            lj["mask"] = std::move(mj);
        }

        json states = json::array();
        for (const auto& s : l.states)
        {
            json sj;
            sj["name"]  = s.name;
            sj["loop"]  = s.loop;
            sj["speed"] = s.speed;
            if (s.blendTree.type != AnimBlendTreeType::None) sj["blendTree"] = BlendTreeToJson(s.blendTree);
            else                                             sj["clip"] = s.clip;
            states.push_back(std::move(sj));
        }
        lj["states"] = std::move(states);

        json trs = json::array();
        for (const auto& t : l.transitions)
        {
            json tj;
            tj["from"]     = t.from;
            tj["to"]       = t.to;
            tj["duration"] = t.duration;
            tj["interruptible"] = t.interruptible;
            if (t.hasExitTime) tj["exitTime"] = t.exitTime;
            json cs = json::array();
            for (const auto& c : t.conditions)
                cs.push_back({{"param", c.param}, {"op", CondOpName(c.op)}, {"value", c.value}});
            tj["conditions"] = std::move(cs);
            trs.push_back(std::move(tj));
        }
        lj["transitions"] = std::move(trs);
        layers.push_back(std::move(lj));
    }
    j["layers"] = std::move(layers);

    return j.dump(2);
}

} // namespace dx12e
