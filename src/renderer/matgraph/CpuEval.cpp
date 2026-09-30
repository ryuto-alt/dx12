#include "renderer/matgraph/CpuEval.h"

#include <cstring>

namespace dx12e::matgraph
{
namespace
{

CpuVal FromValue(const Value& v)
{
    CpuVal r;
    r.type = v.type;
    switch (v.type)
    {
    case ValueType::Int:  r.v[0] = static_cast<float>(v.i); break;
    case ValueType::Bool: r.v[0] = v.b ? 1.0f : 0.0f; break;
    default: for (int k = 0; k < 4; ++k) r.v[k] = v.f[k]; break;
    }
    return r;
}

class OpEval : public EvalCtx
{
public:
    OpEval(const IrOp& op, const std::vector<CpuVal>& inVals, const std::vector<float>& rec, const EvalEnv& env,
           std::vector<Diagnostic>& diags)
        : m_op(op), m_in(inVals), m_rec(rec), m_env(env), m_diags(diags), m_pv(*op.def, op.props)
    {
    }

    bool Has(const std::string& pin) const override
    {
        const int i = m_op.def->InputIndex(pin);
        return i >= 0 && m_op.in[static_cast<size_t>(i)].kind != IrOperand::Kind::None;
    }
    const CpuVal& In(const std::string& pin) const override
    {
        static const CpuVal kZero;
        const int i = m_op.def->InputIndex(pin);
        return i < 0 ? kZero : m_in[static_cast<size_t>(i)];
    }
    CpuVal Slot(const std::string& prop) const override
    {
        const PropDecl* pd = m_op.def->FindProp(prop);
        CpuVal r;
        auto it = m_op.slotOf.find(prop);
        if (it != m_op.slotOf.end())
        {
            r.type = pd && pd->type == PropType::Float ? ValueType::F1
                   : pd && pd->type == PropType::Vec2 ? ValueType::F2
                   : pd && pd->type == PropType::Vec3 ? ValueType::F3 : ValueType::F4;
            for (int k = 0; k < 4; ++k) r.v[k] = m_rec[static_cast<size_t>(it->second) * 4 + static_cast<size_t>(k)];
            return r;
        }
        // inline
        if (!pd) return r;
        switch (pd->type)
        {
        case PropType::Float: r.type = ValueType::F1; r.v[0] = m_pv.Float(prop); break;
        case PropType::Vec2:  r.type = ValueType::F2; m_pv.Vec(prop, r.v); break;
        case PropType::Vec3:  r.type = ValueType::F3; m_pv.Vec(prop, r.v); break;
        default:              r.type = ValueType::F4; m_pv.Vec(prop, r.v); break;
        }
        return r;
    }
    const PropView& Props() const override { return m_pv; }
    const EvalEnv&  Env() const override { return m_env; }
    ValueType ResultType() const override { return m_op.type; }
    int TextureSlot(const std::string& pin) const override
    {
        const int i = m_op.def->InputIndex(pin);
        if (i < 0 || m_op.in[static_cast<size_t>(i)].kind != IrOperand::Kind::Op) return -1;
        return static_cast<int>(m_in[static_cast<size_t>(i)].v[0]);
    }
    void Fail(const std::string& why) override
    {
        if (failed) return;
        failed = true;
        Diagnostic d;
        d.severity = Severity::Error; d.code = code::kCpuUnsupported; d.nodeId = m_op.node; d.message = why;
        m_diags.push_back(std::move(d));
    }
    bool failed = false;

private:
    const IrOp&              m_op;
    const std::vector<CpuVal>& m_in;
    const std::vector<float>&  m_rec;
    const EvalEnv&           m_env;
    std::vector<Diagnostic>& m_diags;
    PropView                 m_pv;
};

CpuVal OperandValue(const IrOperand& o, const std::vector<CpuVal>& vals)
{
    if (o.kind == IrOperand::Kind::Literal) return FromValue(o.literal);
    CpuVal r;
    if (o.kind != IrOperand::Kind::Op) return r;
    const CpuVal& var = vals[static_cast<size_t>(o.op)];
    r.type = o.type;
    if (o.ncomps > 0)
        for (int k = 0; k < o.ncomps; ++k) r.v[k] = var.v[o.comps[k]];
    else
        for (int k = 0; k < 4; ++k) r.v[k] = var.v[k];
    return r;
}

} // namespace

CpuEvalResult EvaluateCpu(const CompileResult& r, const EvalEnv& env, const ParamOverrides* overrides)
{
    CpuEvalResult res;
    if (!r.ok) return res;

    static const ParamOverrides kNone;
    const std::vector<float> rec = PackParamRecord(r, overrides ? *overrides : kNone, nullptr);

    std::vector<CpuVal>& vals = res.opValues;
    vals.assign(r.ir.ops.size(), CpuVal{});
    bool allOk = true;

    for (const IrOp& op : r.ir.ops)
    {
        CpuVal& out = vals[static_cast<size_t>(op.id)];
        out.type = op.type;
        if (op.def == nullptr)
        {
            if (op.opcode == "implicitTex") { out.v[0] = static_cast<float>(op.implicitSlot); continue; }
            if (op.builtin == "uv")             { out.v[0] = env.uv[0]; out.v[1] = env.uv[1]; }
            else if (op.builtin == "worldPos")       for (int k = 0; k < 3; ++k) out.v[k] = env.worldPos[k];
            else if (op.builtin == "vertexNormalWS") for (int k = 0; k < 3; ++k) out.v[k] = env.vertexNormalWS[k];
            else if (op.builtin == "cameraPos")      for (int k = 0; k < 3; ++k) out.v[k] = env.cameraPos[k];
            else if (op.builtin == "vertexColor")    for (int k = 0; k < 4; ++k) out.v[k] = env.vertexColor[k];
            else if (op.builtin == "time")           out.v[0] = env.time;
            continue;
        }
        if (op.type == ValueType::Tex2D && !op.def->eval)
        {
            // TextureParameter: 値 = スロット番号
            if (!op.slotOf.empty()) out.v[0] = static_cast<float>(op.slotOf.begin()->second);
            continue;
        }
        if (!op.def->eval)
        {
            Diagnostic d;
            d.severity = Severity::Error; d.code = code::kCpuUnsupported; d.nodeId = op.node;
            d.message = op.def->type + ": CPU では評価できないノードです";
            res.diagnostics.push_back(std::move(d));
            allOk = false;
            continue;
        }
        std::vector<CpuVal> inVals;
        inVals.reserve(op.in.size());
        for (const IrOperand& o : op.in) inVals.push_back(OperandValue(o, vals));
        OpEval ctx(op, inVals, rec, env, res.diagnostics);
        CpuVal v = op.def->eval(ctx);
        v.type = op.type;
        out = v;
        if (ctx.failed) allOk = false;
    }

    // 出力
    SurfaceValues& s = res.surface;
    for (const IrOutput& o : r.ir.outputs)
    {
        const CpuVal v = OperandValue(o.value, vals);
        const std::string& f = o.field;
        if (f == "baseColor") for (int k = 0; k < 3; ++k) s.baseColor[k] = v.v[k];
        else if (f == "metallic") s.metallic = v.v[0];
        else if (f == "roughness") s.roughness = v.v[0];
        else if (f == "normalTS") { for (int k = 0; k < 3; ++k) s.normalTS[k] = v.v[k]; s.hasNormal = true; }
        else if (f == "emissive") for (int k = 0; k < 3; ++k) s.emissive[k] = v.v[k];
        else if (f == "ao") s.ao = v.v[0];
        else if (f == "opacity") s.opacity = v.v[0];
        else if (f == "opacityMask") s.opacityMask = v.v[0];
        else if (f == "subsurfaceColor") for (int k = 0; k < 3; ++k) s.subsurfaceColor[k] = v.v[k];
        else if (f == "subsurfaceOpacity") s.subsurfaceOpacity = v.v[0];
        else if (f == "clearCoat") s.clearCoat = v.v[0];
        else if (f == "clearCoatRoughness") s.clearCoatRoughness = v.v[0];
        else if (f == "anisotropy") s.anisotropy = v.v[0];
    }
    res.ok = allOk;
    return res;
}

} // namespace dx12e::matgraph
