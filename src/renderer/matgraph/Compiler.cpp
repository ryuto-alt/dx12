#include "renderer/matgraph/Compiler.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <set>

namespace dx12e::matgraph
{

bool CompileResult::HasErrors() const
{
    for (const Diagnostic& d : diagnostics)
        if (d.severity == Severity::Error && d.reachable) return true;
    return false;
}

const SlotInfo* CompileResult::FindSlotByName(const std::string& name) const
{
    for (const SlotInfo& s : slots)
        if (!s.name.empty() && s.name == name) return &s;
    return nullptr;
}

float SrgbToLinear(float c)
{
    c = std::min(std::max(c, 0.0f), 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

namespace
{

// ---------------------------------------------------------------------------
// 小道具
// ---------------------------------------------------------------------------
Value CastValue(const Value& v, ValueType to)
{
    float src[4] = {0, 0, 0, 0};
    int sd = 1;
    switch (v.type)
    {
    case ValueType::Int:  src[0] = static_cast<float>(v.i); break;
    case ValueType::Bool: src[0] = v.b ? 1.0f : 0.0f; break;
    default:
        sd = std::max(1, Dim(v.type));
        for (int k = 0; k < 4; ++k) src[k] = v.f[k];
        break;
    }
    Value r;
    r.type = to;
    if (to == ValueType::Int) { r.i = static_cast<int32_t>(src[0]); return r; }
    if (to == ValueType::Bool) { r.b = src[0] != 0.0f; return r; }
    const int m = Dim(to);
    for (int k = 0; k < m; ++k) r.f[k] = (sd == 1) ? src[0] : src[k];
    return r;
}

std::string Swizzle(const uint8_t* comps, int n)
{
    static const char kL[] = "xyzw";
    std::string s;
    for (int k = 0; k < n; ++k) s += kL[comps[k] & 3];
    return s;
}

std::string PropLiteral(const PropDecl& pd, const PropView& pv)
{
    switch (pd.type)
    {
    case PropType::Float: return FormatHlslFloat(pv.Float(pd.name));
    case PropType::Vec2: case PropType::Vec3: case PropType::Vec4: case PropType::Color:
    {
        const int n = (pd.type == PropType::Vec2) ? 2 : (pd.type == PropType::Vec3) ? 3 : 4;
        float v[4];
        pv.Vec(pd.name, v);
        return FormatHlslValue(Value::Vec(n, v));
    }
    case PropType::Int:  return std::to_string(pv.Int(pd.name));
    case PropType::Bool: return pv.Bool(pd.name) ? "true" : "false";
    default:             return pv.Str(pd.name);
    }
}

ValueType SlotValueType(PropType t)
{
    switch (t)
    {
    case PropType::Float: return ValueType::F1;
    case PropType::Vec2:  return ValueType::F2;
    case PropType::Vec3:  return ValueType::F3;
    case PropType::Vec4: case PropType::Color: return ValueType::F4;
    case PropType::Texture: return ValueType::Tex2D;
    default: return ValueType::Invalid;
    }
}

const char* BuiltinExpr(const std::string& name)
{
    if (name == "uv") return "mi.uv";
    if (name == "worldPos") return "mi.worldPos";
    if (name == "vertexNormalWS") return "mi.vertexNormalWS";
    if (name == "cameraPos") return "mi.cameraPos";
    if (name == "vertexColor") return "mi.vertexColor";
    if (name == "time") return "mi.time";
    return "0";
}

uint64_t Fnv1a64(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

std::string Hex16(uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
    return b;
}

// オペランドの HLSL 式
std::string OperandExpr(const IrOperand& o, const Ir& ir)
{
    switch (o.kind)
    {
    case IrOperand::Kind::Literal: return FormatHlslValue(o.literal);
    case IrOperand::Kind::Op:
    {
        const std::string var = "v" + std::to_string(o.op);
        const int vdim = Dim(ir.ops[static_cast<size_t>(o.op)].type);
        std::string sw;
        if (o.ncomps > 0)
        {
            bool identity = (o.ncomps == vdim);
            for (int k = 0; k < o.ncomps && identity; ++k) identity = (o.comps[k] == k);
            const bool scalarSingle = (vdim <= 1 && o.ncomps == 1);
            if (!identity && !scalarSingle) sw = "." + Swizzle(o.comps, o.ncomps);
        }
        if (o.conv == 1) return "((" + std::string(TypeName(o.type)) + ")" + var + ")";
        if (o.conv == 2) return "((int)" + var + ")";
        return var + sw;
    }
    default: return "0";
    }
}

std::string OperandKey(const IrOperand& o)
{
    switch (o.kind)
    {
    case IrOperand::Kind::Literal: return "L:" + std::string(TypeName(o.literal.type)) + ":" + FormatHlslValue(o.literal);
    case IrOperand::Kind::Op:
        return "O:" + std::to_string(o.op) + "." + Swizzle(o.comps, o.ncomps) + "#" + std::to_string(o.ncomps) + "c" +
               std::to_string(o.conv) + ":" + TypeName(o.type);
    default: return "_";
    }
}

bool ContainsReturn(const std::string& s)
{
    size_t p = 0;
    while ((p = s.find("return", p)) != std::string::npos)
    {
        const bool lb = (p == 0) || !(std::isalnum(static_cast<unsigned char>(s[p - 1])) || s[p - 1] == '_');
        const size_t e = p + 6;
        const bool rb = (e >= s.size()) || !(std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_');
        if (lb && rb) return true;
        p = e;
    }
    return false;
}

// ---------------------------------------------------------------------------
// EmitCtx 実装
// ---------------------------------------------------------------------------
class OpEmit : public EmitCtx
{
public:
    OpEmit(const IrOp& op, const Ir& ir) : m_op(op), m_ir(ir), m_pv(*op.def, op.props) {}

    bool Has(const std::string& pin) const override { return Operand(pin).kind != IrOperand::Kind::None; }
    std::string In(const std::string& pin) const override { return OperandExpr(Operand(pin), m_ir); }
    ValueType InType(const std::string& pin) const override { return Operand(pin).type; }
    bool InIsLiteral(const std::string& pin) const override { return Operand(pin).kind == IrOperand::Kind::Literal; }
    Value InLiteral(const std::string& pin) const override { return Operand(pin).literal; }
    ValueType ResultType() const override { return m_op.type; }
    const PropView& Props() const override { return m_pv; }

    std::string Slot(const std::string& prop) const override
    {
        auto it = m_op.slotOf.find(prop);
        const PropDecl* pd = m_op.def->FindProp(prop);
        if (it == m_op.slotOf.end())
            return pd ? PropLiteral(*pd, m_pv) : "0";   // inline
        const ValueType t = pd ? SlotValueType(pd->type) : ValueType::F1;
        if (t == ValueType::Tex2D) return "pool.Tex(" + std::to_string(it->second) + ")";
        return "pool.F" + std::to_string(Dim(t)) + "(" + std::to_string(it->second) + ")";
    }

    std::string Format(const std::string& tmpl) const override
    {
        std::string out;
        size_t i = 0;
        while (i < tmpl.size())
        {
            if (tmpl[i] != '{') { out += tmpl[i++]; continue; }
            const size_t e = tmpl.find('}', i);
            if (e == std::string::npos) { out += tmpl.substr(i); break; }
            const std::string key = tmpl.substr(i + 1, e - i - 1);
            if (key == "T") out += HlslTypeName(m_op.type);
            else if (key.rfind("P:", 0) == 0)
            {
                const PropDecl* pd = m_op.def->FindProp(key.substr(2));
                out += pd ? PropLiteral(*pd, m_pv) : "0";
            }
            else if (key.rfind("SLOT:", 0) == 0) out += Slot(key.substr(5));
            else out += In(key);
            i = e + 1;
        }
        return out;
    }

private:
    const IrOperand& Operand(const std::string& pin) const
    {
        static const IrOperand kNone;
        const int idx = m_op.def->InputIndex(pin);
        if (idx < 0 || static_cast<size_t>(idx) >= m_op.in.size()) return kNone;
        return m_op.in[static_cast<size_t>(idx)];
    }
    const IrOp&    m_op;
    const Ir&      m_ir;
    PropView       m_pv;
};

// ---------------------------------------------------------------------------
// IR 構築
// ---------------------------------------------------------------------------
class Builder
{
public:
    Builder(const MaterialGraph& g, const GraphAnalysis& an, CompileResult& out) : m_g(g), m_an(an), m_out(out) {}

    void Run() { RunOrder(m_an.order); }

    // 指定した順（後順 = トポロジカル順）でノードを IR にする。部分グラフ出力（previewNode）は上流だけの順を渡す
    void RunOrder(const std::vector<NodeId>& order)
    {
        for (const NodeId& id : order)
        {
            const Node* node = m_g.FindNode(id);
            const NodeTypeInfo* info = m_an.Find(id);
            const NodeDef* def = info->def;
            if (def->isOutput) { BuildOutputs(*node, *info); continue; }
            if (def->isTransparent)
            {
                m_alias[id] = MakeOperand(*node, *info, 0);
                continue;
            }
            BuildOp(*node, *info);
        }
    }

    // 部分グラフ出力: node の出力ピン outIndex を指すオペランド（RunOrder の後に呼ぶ）
    IrOperand PreviewOperand(const NodeId& node, int outIndex)
    {
        PinSource s;
        s.kind = PinSource::Kind::Link;
        s.node = node;
        s.outIndex = outIndex;
        return LinkBase(s);
    }

private:
    const MaterialGraph& m_g;
    const GraphAnalysis& m_an;
    CompileResult&       m_out;
    std::map<NodeId, int>       m_nodeOp;
    std::map<NodeId, IrOperand> m_alias;
    std::map<std::string, int>  m_cse;
    int m_customCount = 0;

    Ir& ir() { return m_out.ir; }

    int AllocSlot(SlotInfo si)
    {
        si.slot = static_cast<int>(m_out.slots.size());
        m_out.slots.push_back(std::move(si));
        return m_out.slots.back().slot;
    }

    int BuiltinOp(const std::string& name)
    {
        const std::string key = "builtin:" + name;
        auto it = m_cse.find(key);
        if (it != m_cse.end()) { ++m_out.stats.cseHits; return it->second; }
        IrOp op;
        op.id = static_cast<int>(ir().ops.size());
        op.node = "";
        op.opcode = key;
        op.builtin = name;
        op.type = BuiltinType(name);
        op.key = key;
        ir().ops.push_back(op);
        m_cse[key] = op.id;
        return op.id;
    }

    int ImplicitTexOp(const Node& node, const NodeDef& def)
    {
        const std::string key = "implicitTex|N=" + node.id;
        IrOp op;
        op.id = static_cast<int>(ir().ops.size());
        op.node = node.id;
        op.opcode = "implicitTex";
        op.type = ValueType::Tex2D;
        op.key = key;
        const PropView pv(def, node.props);
        SlotInfo si;
        si.kind = SlotKind::ImplicitTexture;
        si.type = ValueType::Tex2D;
        si.nodeId = node.id;
        si.prop = "texture";
        si.texturePath = pv.Str("texture");
        si.textureUsage = def.FindProp("usage") ? pv.Str("usage") : std::string("Color");
        op.implicitSlot = AllocSlot(std::move(si));
        ir().ops.push_back(op);
        m_cse[key] = op.id;
        return op.id;
    }

    // base（変数 + 成分選択）を型 T へキャストする
    IrOperand ApplyCast(IrOperand base, ValueType T)
    {
        const CastResult cr = CheckCast(base.srcType, T);
        base.type = T;
        const int m = Dim(T);
        switch (cr.kind)
        {
        case CastKind::Splat:
        {
            const uint8_t c0 = base.ncomps > 0 ? base.comps[0] : 0;
            for (int k = 0; k < 4; ++k) base.comps[k] = c0;
            base.ncomps = m;
            break;
        }
        case CastKind::Truncate:
            if (base.ncomps == 0)
                for (int k = 0; k < 4; ++k) base.comps[k] = static_cast<uint8_t>(k);
            base.ncomps = m;
            break;
        case CastKind::IntToFloat: case CastKind::BoolToFloat: base.conv = 1; base.ncomps = 0; break;
        case CastKind::BoolToInt: base.conv = 2; base.ncomps = 0; break;
        default: break;
        }
        return base;
    }

    IrOperand MakeOperand(const Node& node, const NodeTypeInfo& info, size_t i)
    {
        IrOperand o;
        const PinSource& s = info.inSource[i];
        const ValueType T = info.inTypes[i];
        if (s.kind == PinSource::Kind::None || T == ValueType::Invalid) return o;

        switch (s.kind)
        {
        case PinSource::Kind::Literal: case PinSource::Kind::Default:
            o.kind = IrOperand::Kind::Literal;
            o.srcType = s.srcType;
            o.type = T;
            o.literal = CastValue(s.value, T);
            return o;
        case PinSource::Kind::Builtin:
        {
            IrOperand b;
            b.kind = IrOperand::Kind::Op;
            b.op = BuiltinOp(s.builtin);
            b.srcType = ir().ops[static_cast<size_t>(b.op)].type;
            return ApplyCast(b, T);
        }
        case PinSource::Kind::ImplicitTexture:
        {
            IrOperand b;
            b.kind = IrOperand::Kind::Op;
            b.op = ImplicitTexOp(node, *info.def);
            b.srcType = ValueType::Tex2D;
            b.type = ValueType::Tex2D;
            return b;
        }
        case PinSource::Kind::Link:
            return ApplyCast(LinkBase(s), T);
        default: return o;
        }
    }

    // 接続元ノードの出力ピンを指すオペランド（キャスト前。srcType = 接続元の型）
    IrOperand LinkBase(const PinSource& s)
    {
        const NodeTypeInfo* sinfo = m_an.Find(s.node);
        IrOperand b;
        auto al = m_alias.find(s.node);
        if (al != m_alias.end())
            b = al->second;
        else
        {
            b.kind = IrOperand::Kind::Op;
            b.op = m_nodeOp.at(s.node);
            const std::string& swz = sinfo->def->outputs[static_cast<size_t>(s.outIndex)].swizzle;
            if (swz.empty())
            {
                b.srcType = ir().ops[static_cast<size_t>(b.op)].type;
                b.ncomps = 0;
            }
            else
            {
                b.ncomps = static_cast<int>(swz.size());
                for (int k = 0; k < b.ncomps; ++k) b.comps[k] = static_cast<uint8_t>(swz[static_cast<size_t>(k)] == 'x' ? 0 : swz[static_cast<size_t>(k)] == 'y' ? 1 : swz[static_cast<size_t>(k)] == 'z' ? 2 : 3);
                b.srcType = FloatType(b.ncomps);
            }
        }
        b.srcType = (al != m_alias.end()) ? b.type : b.srcType;   // Reroute は経由先の（キャスト後の）型がそのまま出力型
        return b;
    }

    void BuildOp(const Node& node, const NodeTypeInfo& info)
    {
        const NodeDef& def = *info.def;
        IrOp op;
        op.node = node.id;
        op.def = &def;
        op.opcode = def.type;
        op.type = info.result;
        for (const PropDecl& pd : def.props)
        {
            auto it = node.props.find(pd.name);
            op.props[pd.name] = it != node.props.end() ? it->second : pd.defaultValue;
        }
        const PropView pv(def, op.props);
        op.inlineConst = def.FindProp("inline") && pv.Bool("inline");
        for (size_t i = 0; i < def.inputs.size(); ++i) op.in.push_back(MakeOperand(node, info, i));

        // CSE キー
        std::string key = def.type + "|T=" + TypeName(op.type);
        bool slotBearing = false;
        for (const PropDecl& pd : def.props)
        {
            const Json& v = op.props[pd.name];
            if (pd.role == PropRole::Code) key += "|" + pd.name + "=" + v.dump();
            else if (pd.role == PropRole::SlotValue)
            {
                if (op.inlineConst) key += "|" + pd.name + "=" + v.dump();
                else if (pd.type != PropType::Texture || op.type == ValueType::Tex2D) slotBearing = true;
            }
        }
        if (def.noCse || slotBearing || def.isParameter) key += "|N=" + node.id;
        for (const IrOperand& o : op.in) key += "|" + OperandKey(o);
        op.key = key;

        auto hit = m_cse.find(key);
        if (hit != m_cse.end())
        {
            ++m_out.stats.cseHits;
            m_nodeOp[node.id] = hit->second;
            return;
        }

        // スロット割当（新規 Op のときだけ）
        for (const PropDecl& pd : def.props)
        {
            if (pd.role != PropRole::SlotValue || op.inlineConst) continue;
            const ValueType st = SlotValueType(pd.type);
            if (st == ValueType::Invalid) continue;
            if (st == ValueType::Tex2D && op.type != ValueType::Tex2D) continue;   // 暗黙テクスチャは別 Op
            SlotInfo si;
            si.type = st;
            si.nodeId = node.id;
            si.prop = pd.name;
            if (def.isParameter) si.kind = (st == ValueType::Tex2D) ? SlotKind::Texture : (st == ValueType::F1 ? SlotKind::Scalar : SlotKind::Vector);
            else si.kind = (pd.name == "value") ? SlotKind::Constant : SlotKind::NodeProp;
            if (def.isParameter)
            {
                si.name = pv.Str("name");
                si.group = pv.Str("group");
                si.priority = pv.Int("priority");
            }
            if (def.FindProp("range"))
            {
                float r[4];
                pv.Vec("range", r);
                si.rangeMin = r[0]; si.rangeMax = r[1]; si.hasRange = true;
            }
            if (def.FindProp("srgb")) si.srgb = pv.Bool("srgb");
            if (st == ValueType::Tex2D)
            {
                si.texturePath = pv.Str(pd.name);
                si.textureUsage = def.FindProp("sampler") ? pv.Str("sampler") : std::string("Color");
            }
            else if (pd.type == PropType::Float)
                si.value[0] = pv.Float(pd.name);
            else
                pv.Vec(pd.name, si.value);
            op.slotOf[pd.name] = AllocSlot(std::move(si));
        }
        if (def.isCustom) op.customIndex = m_customCount++;
        op.id = static_cast<int>(ir().ops.size());
        m_cse[key] = op.id;
        m_nodeOp[node.id] = op.id;
        ir().ops.push_back(std::move(op));
    }

    void BuildOutputs(const Node& node, const NodeTypeInfo& info)
    {
        const NodeDef& def = *info.def;
        for (size_t i = 0; i < def.inputs.size(); ++i)
        {
            const PinDecl& pin = def.inputs[i];
            const PinSource& s = info.inSource[i];
            if (pin.surfaceField.empty()) continue;
            if (s.kind != PinSource::Kind::Link && s.kind != PinSource::Kind::Literal) continue;
            IrOutput out;
            out.field = pin.surfaceField;
            out.pin = pin.name;
            out.value = MakeOperand(node, info, i);
            if (out.value.kind != IrOperand::Kind::None) ir().outputs.push_back(std::move(out));
        }
    }
};

// ---------------------------------------------------------------------------
// 部分グラフ出力（previewNode）: 対象ノードの上流だけを後順に並べ、上流のエラーを診断へ集める。
// ---------------------------------------------------------------------------
bool PreparePartial(const MaterialGraph& g, const GraphAnalysis& an, const CompileOptions& opt, CompileResult& r,
                    std::vector<NodeId>& order, int& outIndex)
{
    r.diagnostics.clear();
    auto fail = [&](const std::string& msg, const std::string& hint) {
        Diagnostic d;
        d.severity = Severity::Error;
        d.code = code::kPreviewTarget;
        d.nodeId = opt.previewNode;
        d.message = msg;
        d.hint = hint;
        r.diagnostics.push_back(std::move(d));
        return false;
    };
    const NodeTypeInfo* ti = an.Find(opt.previewNode);
    if (!ti || !ti->def) return fail("プレビューの対象ノード \"" + opt.previewNode + "\" が見つかりません", "");
    if (ti->def->isOutput) return fail("出力ノードはプレビューできません", "");
    if (ti->def->outputs.empty()) return fail(ti->def->type + ": 出力ピンが無いのでプレビューできません", "");
    outIndex = opt.previewPin.empty() ? 0 : ti->def->OutputIndex(opt.previewPin);
    if (outIndex < 0) return fail(ti->def->type + ": 出力ピン \"" + opt.previewPin + "\" がありません", "");

    // 対象の上流を後順 DFS（GraphAnalysis の出力からの順と同じ規則。上流のエラーでも辿る）
    std::set<NodeId> seen;
    std::function<void(const NodeId&)> dfs = [&](const NodeId& id) {
        if (!seen.insert(id).second) return;
        const NodeTypeInfo* ni = an.Find(id);
        if (!ni || !ni->def) return;
        for (size_t i = 0; i < ni->def->inputs.size() && i < ni->inSource.size(); ++i)
        {
            const PinSource& s = ni->inSource[i];
            if (s.kind == PinSource::Kind::Link && g.FindNode(s.node)) dfs(s.node);
        }
        order.push_back(id);
    };
    dfs(opt.previewNode);

    bool bad = false;
    for (const Diagnostic& d : an.diags)
    {
        if (d.nodeId.empty() || !seen.count(d.nodeId)) continue;
        if (d.severity == Severity::Info) continue;
        Diagnostic c = d;
        c.reachable = true;
        if (c.severity == Severity::Error) bad = true;
        r.diagnostics.push_back(std::move(c));
    }
    if (bad) return false;
    const ValueType ot = ti->outTypes.size() > static_cast<size_t>(outIndex) ? ti->outTypes[static_cast<size_t>(outIndex)] : ValueType::Invalid;
    if (ot == ValueType::Invalid) return fail(ti->def->type + ": 出力の型が決まらないのでプレビューできません", "上流の接続を確かめてください");
    if (ot != ValueType::Tex2D && !IsFloatVec(ot)) return fail(ti->def->type + ": この型（" + TypeName(ot) + "）はプレビューできません", "");
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// CompileGraph
// ---------------------------------------------------------------------------
CompileResult CompileGraph(const MaterialGraph& g, const CompileOptions& opt)
{
    CompileResult r;
    const GraphAnalysis& an = g.Analysis();
    r.settings = g.Settings();
    r.stats.nodesTotal = static_cast<int>(g.Nodes().size());

    const bool partial = !opt.previewNode.empty();
    std::vector<NodeId> partialOrder;
    int previewOut = 0;
    if (partial)
    {
        if (!PreparePartial(g, an, opt, r, partialOrder, previewOut)) return r;
        r.stats.nodesReachable = static_cast<int>(partialOrder.size());
    }
    else
    {
        r.diagnostics = an.diags;
        r.stats.nodesReachable = static_cast<int>(an.order.size());
        if (an.hasErrors || an.outputNode.empty()) return r;
    }

    Builder builder(g, an, r);
    if (partial)
    {
        builder.RunOrder(partialOrder);
        IrOperand pv = builder.PreviewOperand(opt.previewNode, previewOut);
        if (pv.kind != IrOperand::Kind::None)
        {
            IrOutput out;
            out.field = "preview";
            out.pin = opt.previewPin;
            out.value = pv;
            r.previewType = pv.srcType;
            r.ir.outputs.push_back(std::move(out));
        }
    }
    else
        builder.Run();
    r.slotCount = static_cast<int>(r.slots.size());
    r.stats.ops = static_cast<int>(r.ir.ops.size());
    r.stats.slots = r.slotCount;
    for (const SlotInfo& s : r.slots)
        if (s.type == ValueType::Tex2D) ++r.stats.textures;

    // ---- HLSL 本体を組む（行番号を記録しながら）----
    std::vector<std::string> B;
    std::vector<std::pair<std::pair<int, int>, NodeId>> spans;   // 本体内の [first,last]（0 始まり）→ node
    auto add = [&](const std::string& s) { B.push_back(s); };
    auto lineDirective = [&](const NodeId& n) {
        if (opt.emitLineDirectives && !n.empty()) add("#line 1 \"node:" + n + "\"");
    };

    add(opt.includeLine);
    add("");

    // 補助関数（使われたノードだけ・名前順）
    std::map<std::string, std::string> helpers;
    for (const IrOp& op : r.ir.ops)
        if (op.def && !op.def->helperName.empty()) helpers[op.def->helperName] = op.def->helperHlsl;
    for (const auto& kv : helpers)
    {
        std::string text = kv.second;
        while (!text.empty() && text.back() == '\n') text.pop_back();
        size_t hp = 0;
        while (hp <= text.size())
        {
            const size_t he = text.find('\n', hp);
            add(text.substr(hp, he == std::string::npos ? std::string::npos : he - hp));
            if (he == std::string::npos) break;
            hp = he + 1;
        }
        add("");
    }

    // Custom ノードの関数
    for (const IrOp& op : r.ir.ops)
    {
        if (!op.def || !op.def->isCustom) continue;
        const PropView pv(*op.def, op.props);
        std::string sig = std::string(HlslTypeName(op.type)) + " MG_Custom" + std::to_string(op.customIndex) + "(";
        bool first = true;
        for (size_t i = 0; i < op.def->inputs.size(); ++i)
        {
            if (op.in[i].kind == IrOperand::Kind::None) continue;
            if (!first) sig += ", ";
            first = false;
            sig += std::string(HlslTypeName(op.in[i].type)) + " " + op.def->inputs[i].name;
        }
        sig += ")";
        const int start = static_cast<int>(B.size());
        add(sig);
        add("{");
        lineDirective(op.node);
        std::string code = pv.Str("code");
        if (ContainsReturn(code))
        {
            // 行ごとにそのまま（行番号がノードの本文の行に一致する）
            size_t p = 0;
            while (p <= code.size())
            {
                const size_t e = code.find('\n', p);
                std::string line = code.substr(p, e == std::string::npos ? std::string::npos : e - p);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                add("    " + line);
                if (e == std::string::npos) break;
                p = e + 1;
            }
        }
        else
            add("    return (" + code + ");");
        add("}");
        spans.push_back({{start, static_cast<int>(B.size()) - 1}, op.node});
        add("");
    }

    add("void UnoMatEval(UnoMatInput mi, UnoMatPool pool, out UnoSurface s)");
    add("{");
    add("    s = UnoSurfaceDefault();");
    for (const IrOp& op : r.ir.ops)
    {
        const int start = static_cast<int>(B.size());
        if (!op.node.empty()) lineDirective(op.node);
        std::string expr;
        if (op.def == nullptr)
        {
            if (op.opcode == "implicitTex") expr = "pool.Tex(" + std::to_string(op.implicitSlot) + ")";
            else expr = BuiltinExpr(op.builtin);
        }
        else if (op.def->isCustom)
        {
            expr = "MG_Custom" + std::to_string(op.customIndex) + "(";
            bool first = true;
            for (size_t i = 0; i < op.def->inputs.size(); ++i)
            {
                if (op.in[i].kind == IrOperand::Kind::None) continue;
                if (!first) expr += ", ";
                first = false;
                expr += OperandExpr(op.in[i], r.ir);
            }
            expr += ")";
        }
        else
        {
            OpEmit ctx(op, r.ir);
            expr = op.def->emit ? op.def->emit(ctx) : ctx.Format(op.def->hlsl);
        }
        add("    " + std::string(HlslTypeName(op.type)) + " v" + std::to_string(op.id) + " = " + expr + ";");
        if (!op.node.empty()) spans.push_back({{start, static_cast<int>(B.size()) - 1}, op.node});
    }
    if (!r.ir.outputs.empty())
    {
        const int start = static_cast<int>(B.size());
        lineDirective(partial ? opt.previewNode : an.outputNode);
        for (const IrOutput& o : r.ir.outputs)
        {
            if (o.field == "preview")
            {
                // 表示色（Compiler.h の CompileOptions::previewNode と CpuEval の "preview" が同じ写像）
                const std::string e = OperandExpr(o.value, r.ir);
                switch (r.previewType)
                {
                case ValueType::F1: add("    s.baseColor = float3(" + e + ", " + e + ", " + e + ");"); break;
                case ValueType::F2: add("    s.baseColor = float3(" + e + ", 0.0);"); break;
                case ValueType::F3: add("    s.baseColor = " + e + ";"); break;
                case ValueType::F4:
                    add("    s.baseColor = (" + e + ").rgb;");
                    add("    s.opacity = (" + e + ").a;");
                    break;
                case ValueType::Tex2D: add("    s.baseColor = UnoSample(" + e + ", UNO_SAMP_LINEAR_CLAMP, mi.uv).rgb;"); break;
                default: break;
                }
                continue;
            }
            add("    s." + o.field + " = " + OperandExpr(o.value, r.ir) + ";");
            if (o.field == "normalTS") add("    s.hasNormal = true;");
        }
        spans.push_back({{start, static_cast<int>(B.size()) - 1}, partial ? opt.previewNode : an.outputNode});
    }
    add("}");

    std::string body;
    for (const std::string& l : B) { body += l; body += '\n'; }
    r.hash = Fnv1a64(body);

    std::string head;
    head += "// @sm " + opt.shaderModel + "\n";
    head += "// GENERATED by Uno MaterialGraph. DO NOT EDIT.";
    if (!opt.sourceName.empty()) head += "  source=" + opt.sourceName;
    head += "  graph=" + Hex16(r.hash) + "  codegen=" + std::to_string(kCodegenVersion) + "\n";
    r.hlsl = head + body;
    const int headLines = 2;
    for (const auto& sp : spans)
        r.sourceMap.push_back({sp.first.first + headLines + 1, sp.first.second + headLines + 1, sp.second});
    r.stats.hlslLines = static_cast<int>(B.size()) + headLines;
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// DXC ログの逆引き
// ---------------------------------------------------------------------------
std::vector<Diagnostic> MapDxcLog(const std::string& log)
{
    std::vector<Diagnostic> out;
    static const std::regex kMapped(R"(node:([A-Za-z0-9_]+):(\d+):(\d+):\s*(error|warning|fatal error):\s*(.*))");
    static const std::regex kPlain(R"(:(\d+):(\d+):\s*(error|warning|fatal error):\s*(.*))");
    size_t p = 0;
    while (p < log.size())
    {
        size_t e = log.find('\n', p);
        std::string line = log.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = (e == std::string::npos) ? log.size() : e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        Diagnostic d;
        if (std::regex_search(line, m, kMapped))
        {
            d.nodeId = m[1].str();
            d.line = std::atoi(m[2].str().c_str());
            d.severity = m[4].str() == "warning" ? Severity::Warning : Severity::Error;
            d.message = m[5].str();
        }
        else if (std::regex_search(line, m, kPlain))
        {
            d.line = std::atoi(m[1].str().c_str());
            d.severity = m[3].str() == "warning" ? Severity::Warning : Severity::Error;
            d.message = m[4].str();
        }
        else
            continue;
        d.code = code::kDxc;
        out.push_back(std::move(d));
    }
    return out;
}

// ---------------------------------------------------------------------------
// パラメータレコード
// ---------------------------------------------------------------------------
std::vector<float> PackParamRecord(const CompileResult& r, const ParamOverrides& overrides, const TextureResolver& tex)
{
    std::vector<float> rec(static_cast<size_t>(r.slotCount) * 4, 0.0f);
    for (const SlotInfo& s : r.slots)
    {
        float* dst = &rec[static_cast<size_t>(s.slot) * 4];
        const ParamOverride* ov = nullptr;
        if (!s.name.empty())
        {
            auto it = overrides.find(s.name);
            if (it != overrides.end()) ov = &it->second;
        }
        if (s.type == ValueType::Tex2D)
        {
            const std::string& path = (ov && ov->isTexture) ? ov->texture : s.texturePath;
            TextureBinding tb;
            if (tex) tb = tex(path, s.textureUsage);
            uint32_t bits = tb.srvIndex;
            std::memcpy(&dst[0], &bits, sizeof(bits));
            dst[1] = tb.width  > 0 ? 1.0f / tb.width : 0.0f;
            dst[2] = tb.height > 0 ? 1.0f / tb.height : 0.0f;
            dst[3] = 0.0f;
            continue;
        }
        float v[4];
        for (int k = 0; k < 4; ++k) v[k] = (ov && !ov->isTexture) ? ov->v[k] : s.value[k];
        if (s.srgb)
            for (int k = 0; k < 3; ++k) v[k] = SrgbToLinear(v[k]);
        for (int k = 0; k < 4; ++k) dst[k] = v[k];
    }
    return rec;
}

} // namespace dx12e::matgraph
