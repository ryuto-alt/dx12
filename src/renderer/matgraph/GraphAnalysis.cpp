#include "renderer/matgraph/GraphAnalysis.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

namespace dx12e::matgraph
{

ValueType BuiltinType(const std::string& name)
{
    if (name == "uv") return ValueType::F2;
    if (name == "worldPos" || name == "vertexNormalWS" || name == "cameraPos") return ValueType::F3;
    if (name == "vertexColor") return ValueType::F4;
    if (name == "time") return ValueType::F1;
    return ValueType::Invalid;
}

namespace
{

class Analyzer
{
public:
    explicit Analyzer(const MaterialGraph& g) : m_g(g), m_lib(g.Library()) {}

    GraphAnalysis Run()
    {
        for (const auto& kv : m_g.Nodes())
            Resolve(kv.first);
        FindOutputAndOrder();
        CheckGlobal();
        Finish();
        return std::move(m_res);
    }

private:
    const MaterialGraph& m_g;
    const NodeLibrary&   m_lib;
    GraphAnalysis        m_res;
    std::map<NodeId, int> m_state;      // 0 = 未訪問 / 1 = 訪問中 / 2 = 完了
    std::vector<NodeId>  m_stack;

    void Diag(Severity sev, const char* c, const NodeId& node, const std::string& pin, std::string msg, std::string hint = {})
    {
        Diagnostic d;
        d.severity = sev; d.code = c; d.nodeId = node; d.pin = pin;
        d.message = std::move(msg); d.hint = std::move(hint);
        m_res.diags.push_back(std::move(d));
    }
    void Err(const char* c, const NodeId& node, const std::string& pin, std::string msg, std::string hint = {})
    {
        Diag(Severity::Error, c, node, pin, std::move(msg), std::move(hint));
    }

    void Resolve(const NodeId& id)
    {
        int& st = m_state[id];
        if (st != 0) return;
        st = 1;
        m_stack.push_back(id);
        ResolveNode(id);
        m_stack.pop_back();
        m_state[id] = 2;
    }

    static std::string Join(const std::vector<NodeId>& v, size_t from)
    {
        std::string s;
        for (size_t k = from; k < v.size(); ++k)
        {
            if (k > from) s += " → ";
            s += v[k];
        }
        return s;
    }

    void ResolveNode(const NodeId& id)
    {
        const Node* node = m_g.FindNode(id);
        NodeTypeInfo& info = m_res.nodes[id];
        info.resolved = true;
        const NodeDef* def = m_lib.Find(node->type);
        info.def = def;
        if (!def)
        {
            Err(code::kUnknownNode, id, "", "未知のノード型 \"" + node->type + "\" です（登録表にありません）",
                "このノードを削除するか、対応するバージョンのエンジンで開いてください");
            return;
        }

        const size_t nIn = def->inputs.size();
        info.inSource.assign(nIn, PinSource{});
        info.inTypes.assign(nIn, ValueType::Invalid);
        bool inputError = false;

        // 宣言に無い入力名（古いファイルなど）
        for (const auto& kv : node->inputs)
            if (!def->FindInput(kv.first))
                Diag(Severity::Warning, code::kUnknownPin, id, kv.first,
                     def->type + ": 入力ピン \"" + kv.first + "\" は存在しません（無視されます）");

        // プロパティの検証
        for (const auto& kv : node->props)
        {
            const PropDecl* pd = def->FindProp(kv.first);
            if (!pd) continue;
            Json tmp;
            if (!NormalizeProp(*pd, kv.second, tmp))
            {
                Err(code::kBadProp, id, "", def->type + ": プロパティ \"" + kv.first + "\" の値が不正です");
                inputError = true;
            }
        }

        // ---- 1. 各入力の出どころ ----
        for (size_t i = 0; i < nIn; ++i)
        {
            const PinDecl& pin = def->inputs[i];
            PinSource& src = info.inSource[i];
            const InputBinding* b = nullptr;
            if (auto it = node->inputs.find(pin.name); it != node->inputs.end()) b = &it->second;

            if (b && b->link)
            {
                src.kind = PinSource::Kind::Link;
                src.node = b->link->node;
                src.pin  = b->link->pin;
                if (pin.reserved)
                {
                    Err(code::kReservedPin, id, pin.name, def->type + ": " + pin.name + " は予約ピンです（接続できません）");
                    src.broken = true; inputError = true;
                    continue;
                }
                const Node* sn = m_g.FindNode(src.node);
                if (!sn)
                {
                    Err(code::kDanglingLink, id, pin.name,
                        def->type + ": " + pin.name + " の接続元ノード \"" + src.node + "\" が存在しません",
                        "接続をやり直してください");
                    src.broken = true; inputError = true;
                    continue;
                }
                // 循環（訪問中のノードへ戻った）
                if (m_state[src.node] == 1)
                {
                    size_t from = 0;
                    while (from < m_stack.size() && m_stack[from] != src.node) ++from;
                    Err(code::kCycle, id, pin.name, "循環しています: " + Join(m_stack, from) + " → " + src.node,
                        "この接続を外してください");
                    src.broken = true; inputError = true;
                    continue;
                }
                Resolve(src.node);
                const NodeTypeInfo* sinfo = m_res.Find(src.node);
                if (!sinfo || !sinfo->def) { src.broken = true; inputError = true; continue; }   // 未知ノードは別途診断済み
                src.outIndex = sinfo->def->OutputIndex(src.pin);
                if (src.outIndex < 0)
                {
                    Err(code::kUnknownPin, id, pin.name,
                        def->type + ": 接続元 " + src.node + "（" + sinfo->def->type + "）に出力ピン \"" + src.pin + "\" がありません");
                    src.broken = true; inputError = true;
                    continue;
                }
                src.srcType = sinfo->outTypes.size() > static_cast<size_t>(src.outIndex)
                                  ? sinfo->outTypes[static_cast<size_t>(src.outIndex)] : ValueType::Invalid;
                const OutPinDecl& od = sinfo->def->outputs[static_cast<size_t>(src.outIndex)];
                if (od.needDim > 0 && Dim(sinfo->result) > 0 && od.needDim > Dim(sinfo->result))
                {
                    Err(code::kMaskRange, id, pin.name,
                        def->type + ": " + pin.name + " に繋いだ " + src.node + "." + od.name + " は、入力が " +
                            TypeName(sinfo->result) + " のため存在しません");
                    src.broken = true; inputError = true;
                    continue;
                }
                if (src.srcType == ValueType::Invalid) { src.broken = true; inputError = true; }   // 上流のエラー（連鎖は出さない）
            }
            else if (b && b->literal)
            {
                if (pin.reserved)
                {
                    Err(code::kReservedPin, id, pin.name, def->type + ": " + pin.name + " は予約ピンです（値を指定できません）");
                    src.broken = true; inputError = true;
                    continue;
                }
                src.kind = PinSource::Kind::Literal;
                src.value = *b->literal;
                src.srcType = b->literal->type;
            }
            else if (def->isOutput)
            {
                // 出力ノードの未接続ピンは既定のサーフェス値のまま（何も出さない）
            }
            else if (pin.implicitTexture)
            {
                src.kind = PinSource::Kind::ImplicitTexture;
                src.srcType = ValueType::Tex2D;
            }
            else if (pin.hasDefault)
            {
                src.kind = PinSource::Kind::Default;
                src.value = pin.defaultValue;
                src.srcType = pin.defaultValue.type;
            }
            else if (!pin.defaultBuiltin.empty())
            {
                src.kind = PinSource::Kind::Builtin;
                src.builtin = pin.defaultBuiltin;
                src.srcType = BuiltinType(pin.defaultBuiltin);
            }
            else if (pin.required)
            {
                Err(code::kMissingInput, id, pin.name, def->type + ": 必須入力 " + pin.name + " が接続されていません",
                    pin.desc.empty() ? std::string() : pin.name + "（" + pin.desc + "）を接続してください");
                src.broken = true; inputError = true;
            }
        }

        // ---- 2. 多相グループの次元 ----
        info.polyDim[1] = info.polyDim[2] = info.polyDim[3] = 1;
        for (size_t i = 0; i < nIn; ++i)
        {
            const PinDecl& pin = def->inputs[i];
            const PinSource& src = info.inSource[i];
            if (pin.mode != PinMode::Poly || src.broken || src.kind == PinSource::Kind::None) continue;
            if (!IsNumeric(src.srcType)) continue;
            const int g = std::clamp(pin.polyGroup, 1, 3);
            info.polyDim[g] = std::max(info.polyDim[g], Dim(src.srcType));
        }

        // ---- 3. 各入力のキャスト検査と解決後の型 ----
        for (size_t i = 0; i < nIn; ++i)
        {
            const PinDecl& pin = def->inputs[i];
            PinSource& src = info.inSource[i];
            if (src.broken || src.kind == PinSource::Kind::None) continue;
            const std::string where = def->type + ": " + pin.name;

            if (pin.mode == PinMode::Fixed)
            {
                const CastResult cr = CheckCast(src.srcType, pin.type);
                if (!cr.ok)
                {
                    const bool expand = IsFloatVec(src.srcType) && IsFloatVec(pin.type);
                    Err(expand ? code::kExpandForbidden : code::kTypeMismatch, id, pin.name, where + " — " + cr.reason);
                    inputError = true;
                    continue;
                }
                if (cr.warn)
                    Diag(Severity::Warning, code::kTruncate, id, pin.name, where + " — " + cr.reason);
                info.inTypes[i] = pin.type;
            }
            else if (pin.mode == PinMode::Poly)
            {
                if (!IsNumeric(src.srcType))
                {
                    const CastResult cr = CheckCast(src.srcType, ValueType::F1);
                    Err(code::kTypeMismatch, id, pin.name, where + " — " + (cr.reason.empty() ? "数値が必要です" : cr.reason));
                    inputError = true;
                    continue;
                }
                const int g = std::clamp(pin.polyGroup, 1, 3);
                const int d = info.polyDim[g];
                const int sd = Dim(src.srcType);
                if (sd != 1 && sd != d)
                {
                    // 同じグループの入力を並べて説明する
                    std::string list;
                    for (size_t k = 0; k < nIn; ++k)
                    {
                        const PinDecl& q = def->inputs[k];
                        const PinSource& qs = info.inSource[k];
                        if (q.mode != PinMode::Poly || std::clamp(q.polyGroup, 1, 3) != g || qs.broken || qs.kind == PinSource::Kind::None) continue;
                        if (!list.empty()) list += ", ";
                        list += q.name + "=" + TypeName(FloatType(Dim(qs.srcType)));
                    }
                    Err(code::kDimMismatch, id, pin.name, def->type + ": " + list + "。次元が違います",
                        "どちらかを float（1 成分）にするか、ComponentMask / Append で次元をそろえてください");
                    inputError = true;
                    continue;
                }
                info.inTypes[i] = FloatType(d);
            }
            else   // Free
            {
                if (IsNumeric(src.srcType))
                    info.inTypes[i] = FloatType(Dim(src.srcType));
                else if (src.srcType == ValueType::Tex2D && pin.allowTexture)
                    info.inTypes[i] = ValueType::Tex2D;
                else
                {
                    const CastResult cr = CheckCast(src.srcType, ValueType::F1);
                    Err(code::kTypeMismatch, id, pin.name, where + " — " + (cr.reason.empty() ? "この型は受け取れません" : cr.reason));
                    inputError = true;
                }
            }
        }

        // ---- 4. 出力の型 ----
        const size_t nOut = def->outputs.size();
        info.outTypes.assign(nOut, ValueType::Invalid);
        if (inputError) return;   // 上流 / 入力にエラー → 型は決まらない（連鎖エラーを出さない）

        InferCtx ic;
        ic.def = def;
        PropView pv(*def, node->props);
        ic.props = &pv;
        ic.nodeId = id;
        ic.inTypes = info.inTypes;
        ic.inPresent.resize(nIn);
        for (size_t i = 0; i < nIn; ++i) ic.inPresent[i] = info.inSource[i].kind != PinSource::Kind::None;
        for (int g = 0; g < 4; ++g) ic.polyDim[g] = info.polyDim[g];
        ic.diags = &m_res.diags;
        // 既定の推論
        switch (def->resultMode)
        {
        case PinMode::Fixed: ic.result = def->resultType; break;
        default:             ic.result = FloatType(info.polyDim[std::clamp(def->resultGroup, 1, 3)]); break;
        }
        ic.outTypes.assign(nOut, ValueType::Invalid);
        auto fillOuts = [&]() {
            for (size_t k = 0; k < nOut; ++k)
            {
                const OutPinDecl& o = def->outputs[k];
                if (!o.swizzle.empty()) ic.outTypes[k] = FloatType(static_cast<int>(o.swizzle.size()));
                else if (o.mode == PinMode::Fixed) ic.outTypes[k] = o.type;
                else ic.outTypes[k] = ic.result;
            }
        };
        if (def->infer)
        {
            const size_t before = m_res.diags.size();
            def->infer(ic);
            for (size_t k = before; k < m_res.diags.size(); ++k)
                if (m_res.diags[k].severity == Severity::Error) inputError = true;
        }
        fillOuts();
        if (inputError || ic.result == ValueType::Invalid)
        {
            info.result = ValueType::Invalid;
            return;
        }
        info.result = ic.result;
        info.outTypes = ic.outTypes;

        // Custom の本文が空
        if (def->isCustom)
        {
            std::string code = pv.Str("code");
            code.erase(std::remove_if(code.begin(), code.end(), [](unsigned char c) { return std::isspace(c) != 0; }), code.end());
            if (code.empty())
            {
                Err(code::kCustomEmpty, id, "", "Custom: HLSL の本文が空です");
                info.result = ValueType::Invalid;
                info.outTypes.assign(nOut, ValueType::Invalid);
            }
        }
    }

    void FindOutputAndOrder()
    {
        std::vector<NodeId> outputs;
        for (const auto& kv : m_g.Nodes())
        {
            const NodeTypeInfo* ni = m_res.Find(kv.first);
            if (ni && ni->def && ni->def->isOutput) outputs.push_back(kv.first);
        }
        if (!outputs.empty()) m_res.outputNode = outputs.front();
        for (size_t k = 1; k < outputs.size(); ++k)
            Err(code::kMultiOutput, outputs[k], "", "MaterialOutput が複数あります（1 個だけ置いてください）");
        if (outputs.empty())
            Err(code::kNoOutput, "", "", "MaterialOutput ノードがありません", "出力ノードを 1 個置いてください");

        // 出力ノードから入力方向へ後順 DFS
        std::set<NodeId> seen;
        std::vector<NodeId> post;
        std::function<void(const NodeId&)> dfs = [&](const NodeId& id) {
            if (!seen.insert(id).second) return;
            NodeTypeInfo* ni = nullptr;
            if (auto it = m_res.nodes.find(id); it != m_res.nodes.end()) ni = &it->second;
            if (!ni) return;
            ni->reachable = true;   // 未知のノードでも「出力から使われている」ことは分かる（診断を止める側に数える）
            if (!ni->def) return;
            for (size_t i = 0; i < ni->def->inputs.size(); ++i)
            {
                const PinSource& s = ni->inSource[i];
                if (s.kind == PinSource::Kind::Link && m_g.FindNode(s.node)) dfs(s.node);   // 上流のエラーでも辿る（到達可能性は型に依らない）
            }
            post.push_back(id);
        };
        if (!m_res.outputNode.empty()) dfs(m_res.outputNode);
        m_res.order = post;
    }

    void CheckGlobal()
    {
        // パラメータ名の重複 / 空（到達できるものだけ）
        std::map<std::string, NodeId> names;
        for (const NodeId& id : m_res.order)
        {
            const NodeTypeInfo* ni = m_res.Find(id);
            if (!ni->def->isParameter) continue;
            PropView pv(*ni->def, m_g.FindNode(id)->props);
            const std::string nm = pv.Str("name");
            if (nm.empty())
            {
                Err(code::kDupParam, id, "", ni->def->type + ": パラメータ名が空です");
                continue;
            }
            auto ins = names.emplace(nm, id);
            if (!ins.second)
                Err(code::kDupParam, id, "", "パラメータ名 \"" + nm + "\" が " + ins.first->second + " と重複しています",
                    "パラメータ名はグラフ内で一意にしてください");
        }
    }

    void Finish()
    {
        for (Diagnostic& d : m_res.diags)
        {
            if (d.nodeId.empty()) { d.reachable = true; continue; }
            const NodeTypeInfo* ni = m_res.Find(d.nodeId);
            d.reachable = ni ? ni->reachable : true;
            if (d.code == code::kMultiOutput) d.reachable = true;
        }
        // 出力に繋がらないノード（情報）
        for (const auto& kv : m_res.nodes)
        {
            if (kv.second.reachable || !kv.second.def) continue;
            Diagnostic d;
            d.severity = Severity::Info; d.code = code::kDeadNode; d.nodeId = kv.first; d.reachable = false;
            d.message = kv.second.def->type + ": 出力に繋がっていないため、生成されません";
            m_res.diags.push_back(std::move(d));
        }
        m_res.hasErrors = false;
        for (const Diagnostic& d : m_res.diags)
            if (d.severity == Severity::Error && d.reachable) m_res.hasErrors = true;
    }
};

} // namespace

GraphAnalysis AnalyzeGraph(const MaterialGraph& g)
{
    return Analyzer(g).Run();
}

std::vector<ParamDecl> CollectParameters(const MaterialGraph& g)
{
    const GraphAnalysis& an = g.Analysis();
    std::vector<ParamDecl> out;
    for (const auto& kv : g.Nodes())
    {
        const NodeDef* def = g.DefOf(kv.first);
        if (!def || !def->isParameter) continue;
        const PropView pv(*def, kv.second.props);
        ParamDecl d;
        d.nodeId = kv.first;
        d.nodeType = def->type;
        d.name = pv.Str("name");
        d.type = def->resultType;
        d.defaultValue = pv.Raw("default");
        d.group = pv.Str("group");
        d.priority = pv.Int("priority");
        if (def->FindProp("sampler")) d.usage = pv.Str("sampler");
        const NodeTypeInfo* ni = an.Find(kv.first);
        d.reachable = ni && ni->reachable;
        out.push_back(std::move(d));
    }
    std::stable_sort(out.begin(), out.end(), [](const ParamDecl& a, const ParamDecl& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        if (a.group != b.group) return a.group < b.group;
        return a.name < b.name;
    });
    return out;
}

} // namespace dx12e::matgraph
