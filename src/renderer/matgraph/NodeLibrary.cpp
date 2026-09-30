#include "renderer/matgraph/NodeLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dx12e::matgraph
{

const char* const kCategoryOrder[] = {
    "定数", "パラメータ", "座標", "時間", "数学", "ベクトル", "テクスチャ", "色", "シェーディング", "制御", "出力",
};
const int kCategoryCount = static_cast<int>(sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]));

// ---------------------------------------------------------------------------
// PropView
// ---------------------------------------------------------------------------
const Json& PropView::Raw(const std::string& name) const
{
    static const Json kNull;
    auto it = m_props.find(name);
    if (it != m_props.end()) return it->second;
    if (const PropDecl* d = m_def.FindProp(name)) return d->defaultValue;
    return kNull;
}

float PropView::Float(const std::string& name) const
{
    const Json& j = Raw(name);
    return j.is_number() ? j.get<float>() : 0.0f;
}

void PropView::Vec(const std::string& name, float out[4]) const
{
    for (int k = 0; k < 4; ++k) out[k] = 0.0f;
    const Json& j = Raw(name);
    if (j.is_array())
        for (size_t k = 0; k < j.size() && k < 4; ++k)
            if (j[k].is_number()) out[k] = j[k].get<float>();
}

int PropView::Int(const std::string& name) const
{
    const Json& j = Raw(name);
    return j.is_number() ? static_cast<int>(j.get<double>()) : 0;
}

bool PropView::Bool(const std::string& name) const
{
    const Json& j = Raw(name);
    return j.is_boolean() ? j.get<bool>() : false;
}

std::string PropView::Str(const std::string& name) const
{
    const Json& j = Raw(name);
    return j.is_string() ? j.get<std::string>() : std::string();
}

void InferCtx::Error(const char* c, const std::string& pin, const std::string& msg, const std::string& hint) const
{
    if (!diags) return;
    Diagnostic d;
    d.severity = Severity::Error;
    d.code     = c;
    d.nodeId   = nodeId;
    d.pin      = pin;
    d.message  = msg;
    d.hint     = hint;
    diags->push_back(std::move(d));
}

// ---------------------------------------------------------------------------
// NodeDef の検索ヘルパ
// ---------------------------------------------------------------------------
int NodeDef::InputIndex(const std::string& name) const
{
    for (size_t k = 0; k < inputs.size(); ++k)
        if (inputs[k].name == name) return static_cast<int>(k);
    return -1;
}
const PinDecl* NodeDef::FindInput(const std::string& name) const
{
    const int k = InputIndex(name);
    return k < 0 ? nullptr : &inputs[static_cast<size_t>(k)];
}
int NodeDef::OutputIndex(const std::string& name) const
{
    for (size_t k = 0; k < outputs.size(); ++k)
        if (outputs[k].name == name) return static_cast<int>(k);
    return -1;
}
const OutPinDecl* NodeDef::FindOutput(const std::string& name) const
{
    const int k = OutputIndex(name);
    return k < 0 ? nullptr : &outputs[static_cast<size_t>(k)];
}
const PropDecl* NodeDef::FindProp(const std::string& name) const
{
    for (const PropDecl& p : props)
        if (p.name == name) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------
// NodeLibrary
// ---------------------------------------------------------------------------
void NodeLibrary::Register(NodeDef def)
{
    const std::string key = def.type;
    m_defs[key] = std::move(def);
}

const NodeDef* NodeLibrary::Find(const std::string& type) const
{
    auto it = m_defs.find(type);
    return it == m_defs.end() ? nullptr : &it->second;
}

static int CategoryRank(const std::string& c)
{
    for (int k = 0; k < kCategoryCount; ++k)
        if (c == kCategoryOrder[k]) return k;
    return kCategoryCount;   // 未知のカテゴリは最後
}

std::vector<const NodeDef*> NodeLibrary::List() const
{
    std::vector<const NodeDef*> v;
    v.reserve(m_defs.size());
    for (const auto& kv : m_defs) v.push_back(&kv.second);
    std::stable_sort(v.begin(), v.end(), [](const NodeDef* a, const NodeDef* b) {
        const int ra = CategoryRank(a->category), rb = CategoryRank(b->category);
        if (ra != rb) return ra < rb;
        return a->type < b->type;
    });
    return v;
}

static std::string Lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::vector<const NodeDef*> NodeLibrary::Search(const std::string& query) const
{
    const std::string q = Lower(query);
    std::vector<const NodeDef*> out;
    for (const NodeDef* d : List())
    {
        if (q.empty()) { out.push_back(d); continue; }
        const std::string hay = Lower(d->type + " " + d->displayName + " " + d->keywords + " " + d->description);
        if (hay.find(q) != std::string::npos) out.push_back(d);
    }
    return out;
}

const NodeLibrary& NodeLibrary::Builtin()
{
    static const NodeLibrary lib = [] {
        NodeLibrary l;
        RegisterBuiltinNodes(l);
        return l;
    }();
    return lib;
}

// ---------------------------------------------------------------------------
// プロパティ値の正規化
// ---------------------------------------------------------------------------
static Json FloatJson(double x)
{
    // float に丸めてから最短表記経由で double へ（0.6f が 0.6000000238… にならないように）
    const float f = static_cast<float>(x);
    return Json(std::strtod(FormatFloatShort(f).c_str(), nullptr));
}

bool NormalizeProp(const PropDecl& decl, const Json& in, Json& out)
{
    auto vecN = [&](size_t n, bool allowRgb) -> bool {
        if (!in.is_array()) return false;
        if (!(in.size() == n || (allowRgb && in.size() == 3))) return false;
        Json arr = Json::array();
        for (size_t k = 0; k < in.size(); ++k)
        {
            if (!in[k].is_number()) return false;
            arr.push_back(FloatJson(in[k].get<double>()));
        }
        if (allowRgb && in.size() == 3) arr.push_back(FloatJson(1.0));
        out = std::move(arr);
        return true;
    };
    switch (decl.type)
    {
    case PropType::Float:
        if (!in.is_number()) return false;
        out = FloatJson(in.get<double>());
        return true;
    case PropType::Vec2:  return vecN(2, false);
    case PropType::Vec3:  return vecN(3, false);
    case PropType::Vec4:  return vecN(4, false);
    case PropType::Color: return vecN(4, true);
    case PropType::Int:
        if (!in.is_number()) return false;
        out = Json(static_cast<int64_t>(std::llround(in.get<double>())));
        return true;
    case PropType::Bool:
        if (!in.is_boolean()) return false;
        out = in;
        return true;
    case PropType::String: case PropType::Text: case PropType::Texture:
        if (!in.is_string()) return false;
        out = in;
        return true;
    case PropType::Enum:
        if (!in.is_string()) return false;
        if (std::find(decl.enumValues.begin(), decl.enumValues.end(), in.get<std::string>()) == decl.enumValues.end())
            return false;
        out = in;
        return true;
    }
    return false;
}

} // namespace dx12e::matgraph
