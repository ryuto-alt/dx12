#include "editor/matgraph/MatGraphModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dx12e::mg
{

namespace
{

// ---------------------------------------------------------------------------
// 表示の対応表
// ---------------------------------------------------------------------------
std::string Leaf(const std::string& path)
{
    const size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

// プロパティ名 → 行ラベル（英語の短い名前。値欄の左に出る）
std::string PropLabel(const std::string& name)
{
    if (name == "samplerState") return "Sampler";
    if (name == "outputType") return "Type";
    if (name.empty()) return name;
    std::string s = name;
    if (s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

bool IsColorPin(const mat::PinDecl& d)
{
    return d.name.find("Color") != std::string::npos || d.name == "Emissive";
}

ng::ValueKind KindForPin(const mat::PinDecl& d)
{
    switch (d.mode)
    {
    case mat::PinMode::Poly: return ng::ValueKind::Float;
    case mat::PinMode::Free: return ng::ValueKind::None;
    case mat::PinMode::Fixed:
        switch (d.type)
        {
        case mat::ValueType::F1: return ng::ValueKind::Float;
        case mat::ValueType::F2: return ng::ValueKind::Vec2;
        case mat::ValueType::F3: return IsColorPin(d) ? ng::ValueKind::Color : ng::ValueKind::Vec3;
        case mat::ValueType::F4: return ng::ValueKind::Color;
        case mat::ValueType::Bool: return ng::ValueKind::Bool;
        default: return ng::ValueKind::None;
        }
    }
    return ng::ValueKind::None;
}

void RangeFor(const std::string& pin, float& mn, float& mx, float& speed)
{
    static const char* const k01[] = {"Metallic", "Roughness", "AmbientOcclusion", "Opacity", "OpacityMask", "Alpha", "Fraction",
                                      "Specular", "ClearCoat", "ClearCoatRoughness", "SubsurfaceOpacity"};
    for (const char* n : k01)
        if (pin == n) { mn = 0.0f; mx = 1.0f; speed = 0.005f; return; }
}

// プロパティが値欄の行（ノード本体に出る）になるか。文字列・テクスチャ・メタ情報（範囲 / グループ / 表示順 / sRGB）と inline は詳細パネルだけ。
bool IsInlineProp(const mat::PropDecl& pd)
{
    switch (pd.type)
    {
    case mat::PropType::String: case mat::PropType::Text: case mat::PropType::Texture: return false;
    default: break;
    }
    if (pd.name == "inline") return false;
    return pd.role != mat::PropRole::Meta;
}

ng::ValueKind KindForProp(const mat::NodeDef& def, const mat::PropDecl& pd)
{
    switch (pd.type)
    {
    case mat::PropType::Float: case mat::PropType::Int: return ng::ValueKind::Float;
    case mat::PropType::Vec2: return ng::ValueKind::Vec2;
    case mat::PropType::Vec3: return def.type == "Float3" ? ng::ValueKind::Color : ng::ValueKind::Vec3;
    case mat::PropType::Vec4: return ng::ValueKind::Vec4;
    case mat::PropType::Color: return ng::ValueKind::Color;
    case mat::PropType::Bool: return ng::ValueKind::Bool;
    case mat::PropType::Enum: return ng::ValueKind::Enum;
    default: return ng::ValueKind::None;
    }
}

ng::PinValue JsonToPin(const mat::Json& j, ng::ValueKind kind, const mat::PropDecl& pd)
{
    ng::PinValue v;
    v.kind = kind;
    if (kind == ng::ValueKind::Color) v.f[3] = 1.0f;
    if (j.is_number()) v.f[0] = j.get<float>();
    else if (j.is_boolean()) v.f[0] = j.get<bool>() ? 1.0f : 0.0f;
    else if (j.is_array())
        for (size_t k = 0; k < j.size() && k < 4; ++k)
            if (j[k].is_number()) v.f[k] = j[k].get<float>();
    if (j.is_string() && kind == ng::ValueKind::Enum)
    {
        const std::string s = j.get<std::string>();
        for (size_t k = 0; k < pd.enumValues.size(); ++k)
            if (pd.enumValues[k] == s) { v.f[0] = static_cast<float>(k); break; }
    }
    return v;
}

mat::Json PinToJson(const ng::PinValue& v, const mat::PropDecl& pd)
{
    using PT = mat::PropType;
    switch (pd.type)
    {
    case PT::Float: return mat::Json(static_cast<double>(v.f[0]));
    case PT::Int: return mat::Json(static_cast<int64_t>(std::llround(static_cast<double>(v.f[0]))));
    case PT::Bool: return mat::Json(v.f[0] != 0.0f);
    case PT::Vec2: return mat::Json::array({static_cast<double>(v.f[0]), static_cast<double>(v.f[1])});
    case PT::Vec3: return mat::Json::array({static_cast<double>(v.f[0]), static_cast<double>(v.f[1]), static_cast<double>(v.f[2])});
    case PT::Vec4: case PT::Color:
        return mat::Json::array({static_cast<double>(v.f[0]), static_cast<double>(v.f[1]), static_cast<double>(v.f[2]), static_cast<double>(v.f[3])});
    case PT::Enum:
    {
        const int i = static_cast<int>(std::lround(static_cast<double>(v.f[0])));
        if (i >= 0 && static_cast<size_t>(i) < pd.enumValues.size()) return mat::Json(pd.enumValues[static_cast<size_t>(i)]);
        return mat::Json();
    }
    default: return mat::Json();
    }
}

// リテラルの JSON 表現（extra 用。型を保つ）: ["F3", x, y, z] / ["Int", 3] / ["Bool", true]
mat::Json LiteralToJson(const mat::Value& v)
{
    using VT = mat::ValueType;
    switch (v.type)
    {
    case VT::Int:  return mat::Json::array({"Int", static_cast<int64_t>(v.i)});
    case VT::Bool: return mat::Json::array({"Bool", v.b});
    default:
    {
        mat::Json a = mat::Json::array();
        a.push_back(mat::TypeName(v.type));
        for (int k = 0; k < mat::Dim(v.type); ++k) a.push_back(static_cast<double>(v.f[k]));
        return a;
    }
    }
}

bool LiteralFromJson(const mat::Json& j, mat::Value& out)
{
    if (!j.is_array() || j.empty() || !j[0].is_string()) return false;
    const std::string t = j[0].get<std::string>();
    if (t == "Int") { if (j.size() < 2 || !j[1].is_number()) return false; out = mat::Value::Int32(j[1].get<int32_t>()); return true; }
    if (t == "Bool") { if (j.size() < 2 || !j[1].is_boolean()) return false; out = mat::Value::Bool1(j[1].get<bool>()); return true; }
    mat::ValueType vt;
    if (!mat::ParseTypeName(t, vt) || !mat::IsFloatVec(vt)) return false;
    float f[4] = {0, 0, 0, 0};
    for (int k = 0; k < mat::Dim(vt) && static_cast<size_t>(k + 1) < j.size(); ++k)
        if (j[static_cast<size_t>(k + 1)].is_number()) f[k] = j[static_cast<size_t>(k + 1)].get<float>();
    out = mat::Value::Vec(mat::Dim(vt), f);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 公開の対応表
// ---------------------------------------------------------------------------
ng::PinType PinTypeOf(mat::ValueType t)
{
    switch (t)
    {
    case mat::ValueType::F1: return kPtF1;
    case mat::ValueType::F2: return kPtF2;
    case mat::ValueType::F3: return kPtF3;
    case mat::ValueType::F4: return kPtF4;
    case mat::ValueType::Tex2D: return kPtTex;
    case mat::ValueType::Bool: return kPtBool;
    case mat::ValueType::Int: return kPtInt;
    default: return kPtOther;
    }
}

int CategorySlotOf(const std::string& c)
{
    if (c == "定数") return 0;
    if (c == "パラメータ") return 1;
    if (c == "テクスチャ") return 2;
    if (c == "座標") return 3;
    if (c == "数学") return 4;
    if (c == "ベクトル") return 5;
    if (c == "色") return 6;
    if (c == "時間" || c == "シェーディング") return 7;
    if (c == "制御") return 8;
    if (c == "出力") return 9;
    return 8;
}

const char* CategoryLabel(int slot)
{
    static const char* const k[] = {"定数", "パラメータ", "テクスチャ", "座標", "数学", "ベクトル", "色", "時間・シェーディング", "制御", "出力"};
    return (slot >= 0 && slot < 10) ? k[slot] : "?";
}

char HotkeyOf(const std::string& t)
{
    struct H { const char* type; char key; };
    static const H kTable[] = {
        {"Float", '1'}, {"Float2", '2'}, {"Float3", '3'}, {"Float4", '4'},
        {"ScalarParameter", 'S'}, {"VectorParameter", 'V'}, {"TextureSample", 'T'}, {"TexCoord", 'U'},
        {"Add", 'A'}, {"Multiply", 'M'}, {"Lerp", 'L'}, {"Divide", 'D'}, {"Normalize", 'N'}, {"OneMinus", 'O'}, {"Power", 'E'},
        {"Saturate", 'Q'}, {"Fresnel", 'R'}, {"Time", 'I'},
    };
    for (const H& h : kTable)
        if (t == h.type) return h.key;
    return 0;
}

ng::PinValue ToPinValue(const mat::Value& v, ng::ValueKind kind)
{
    ng::PinValue p;
    p.kind = kind;
    const int n = ng::ValueComponentCount(kind);
    if (kind == ng::ValueKind::Bool) { p.f[0] = v.b ? 1.0f : 0.0f; return p; }
    if (kind == ng::ValueKind::Color) p.f[3] = 1.0f;
    const bool splat = v.type == mat::ValueType::F1;
    const float base = v.type == mat::ValueType::Int ? static_cast<float>(v.i) : v.f[0];
    for (int k = 0; k < n && k < 4; ++k)
    {
        if (kind == ng::ValueKind::Color && k == 3 && mat::Dim(v.type) < 4) break;   // RGB のリテラルはアルファ 1
        p.f[k] = (splat || v.type == mat::ValueType::Int) ? base : v.f[k];
    }
    return p;
}

mat::Value ToLiteral(const ng::PinValue& v, ng::ValueKind kind, mat::ValueType pinType)
{
    switch (kind)
    {
    case ng::ValueKind::Bool: return mat::Value::Bool1(v.f[0] != 0.0f);
    case ng::ValueKind::Vec2: return mat::Value::Float2(v.f[0], v.f[1]);
    case ng::ValueKind::Vec3: return mat::Value::Float3(v.f[0], v.f[1], v.f[2]);
    case ng::ValueKind::Vec4: return mat::Value::Float4(v.f[0], v.f[1], v.f[2], v.f[3]);
    case ng::ValueKind::Color:
        if (pinType == mat::ValueType::F3) return mat::Value::Float3(v.f[0], v.f[1], v.f[2]);
        return mat::Value::Float4(v.f[0], v.f[1], v.f[2], v.f[3]);
    default: return mat::Value::Float(v.f[0]);
    }
}

// ---------------------------------------------------------------------------
// 構築
// ---------------------------------------------------------------------------
MatGraphModel::MatGraphModel(mat::MaterialGraph* graph) : m_g(graph)
{
    m_pinTypes[kPtF1]     = {"float",        0, ng::PinShape::Circle};
    m_pinTypes[kPtF2]     = {"float2",       1, ng::PinShape::CircleLine1};
    m_pinTypes[kPtF3]     = {"float3",       2, ng::PinShape::CircleLine2};
    m_pinTypes[kPtF4]     = {"float4",       3, ng::PinShape::Diamond};
    m_pinTypes[kPtTex]    = {"texture2D",    4, ng::PinShape::RoundedSquare};
    m_pinTypes[kPtBool]   = {"bool",         5, ng::PinShape::Square};
    m_pinTypes[kPtAnyNum] = {"数値（多相）", 6, ng::PinShape::Circle};
    m_pinTypes[kPtWild]   = {"任意",         7, ng::PinShape::Pentagon};
    m_pinTypes[kPtInt]    = {"int",          8, ng::PinShape::Triangle};
    m_pinTypes[kPtOther]  = {"その他",       7, ng::PinShape::Circle};
    BuildCatalog();
    m_listener = m_g->AddListener([this](const mat::GraphChange& c) { OnChange(c); });
}

MatGraphModel::~MatGraphModel()
{
    if (m_g && m_listener) m_g->RemoveListener(m_listener);
}

void MatGraphModel::BuildCatalog()
{
    const mat::NodeLibrary& lib = m_g->Library();
    const std::vector<const mat::NodeDef*> defs = lib.List();
    m_types.clear();
    m_infos.clear();
    m_types.reserve(defs.size());
    m_infos.reserve(defs.size());
    for (const mat::NodeDef* def : defs)
    {
        ng::NodeTypeDesc t;
        TypeInfo info;
        info.def = def;
        t.id = def->type;
        t.title = def->displayName;
        t.category = def->category;
        t.categorySlot = CategorySlotOf(def->category);
        t.keywords = def->keywords + " " + def->type + " " + def->category;
        t.description = def->description;
        t.hotkey = HotkeyOf(def->type);
        t.isReroute = def->isTransparent;
        t.hasLabel = def->isParameter || def->type == "TextureSample" || def->type == "NormalMap" || def->isCustom;

        // 入力ピン（予約ピンは出さない。接続できないので UI に要らない）
        for (size_t k = 0; k < def->inputs.size(); ++k)
        {
            const mat::PinDecl& d = def->inputs[k];
            if (d.reserved) continue;
            ng::PinDesc p;
            p.name = d.name;
            p.desc = d.desc;
            switch (d.mode)
            {
            case mat::PinMode::Fixed: p.type = PinTypeOf(d.type); break;
            case mat::PinMode::Poly:  p.type = kPtAnyNum; break;
            case mat::PinMode::Free:  p.type = d.allowTexture ? kPtWild : kPtAnyNum; break;
            }
            const ng::ValueKind kind = KindForPin(d);
            // 値欄: 既定値があり、組み込み入力（uv など）でも暗黙テクスチャでもないピン
            if (kind != ng::ValueKind::None && d.hasDefault && d.defaultBuiltin.empty() && !d.implicitTexture)
            {
                p.valueKind = kind;
                p.defaultValue = ToPinValue(d.defaultValue, kind);
                RangeFor(d.name, p.rangeMin, p.rangeMax, p.dragSpeed);
                if (kind == ng::ValueKind::Vec3 || kind == ng::ValueKind::Vec2) p.dragSpeed = 0.01f;
            }
            t.inputs.push_back(std::move(p));
            info.inDef.push_back(static_cast<int>(k));
            info.inProp.emplace_back();
        }
        // プロパティ行（値欄だけの行）
        for (const mat::PropDecl& pd : def->props)
        {
            if (!IsInlineProp(pd)) continue;
            const ng::ValueKind kind = KindForProp(*def, pd);
            if (kind == ng::ValueKind::None) continue;
            ng::PinDesc p;
            p.name = PropLabel(pd.name);
            p.desc = pd.desc;
            p.type = kPtF1;
            p.valueKind = kind;
            p.propertyOnly = true;
            p.defaultValue = JsonToPin(pd.defaultValue, kind, pd);
            p.enumOptions = pd.enumValues;
            if (pd.minValue != 0.0f || pd.maxValue != 0.0f) { p.rangeMin = pd.minValue; p.rangeMax = pd.maxValue; }
            if (pd.type == mat::PropType::Int) p.dragSpeed = 0.1f;
            t.inputs.push_back(std::move(p));
            info.inDef.push_back(-1);
            info.inProp.push_back(pd.name);
        }
        for (size_t k = 0; k < def->outputs.size(); ++k)
        {
            const mat::OutPinDecl& o = def->outputs[k];
            ng::PinDesc p;
            p.name = o.name;
            p.desc = o.desc;
            p.type = o.mode == mat::PinMode::Poly ? static_cast<int>(kPtAnyNum) : static_cast<int>(PinTypeOf(o.type));
            t.outputs.push_back(std::move(p));
            info.outDef.push_back(static_cast<int>(k));
        }
        if (def->isTransparent)
        {
            // リルートの出力は入力の型をそのまま引き継ぐ（Wildcard で描く）
            for (ng::PinDesc& p : t.outputs) { p.name.clear(); p.type = kPtWild; }
            for (ng::PinDesc& p : t.inputs) { p.name.clear(); p.type = kPtWild; }
        }
        m_typeIndex[t.id] = static_cast<int>(m_types.size());
        m_types.push_back(std::move(t));
        m_infos.push_back(std::move(info));
    }
}

const ng::NodeTypeDesc* MatGraphModel::FindNodeType(const std::string& id) const
{
    auto it = m_typeIndex.find(id);
    return it == m_typeIndex.end() ? nullptr : &m_types[static_cast<size_t>(it->second)];
}

void MatGraphModel::SetNodePreview(const std::string& typeId, bool on)
{
    auto it = m_typeIndex.find(typeId);
    if (it != m_typeIndex.end()) m_types[static_cast<size_t>(it->second)].hasPreview = on;
}

const ng::PinTypeDesc& MatGraphModel::PinTypeInfo(ng::PinType t) const
{
    return m_pinTypes[(t >= 0 && t < kPtCount) ? t : kPtOther];
}

const ng::NodeTypeDesc* MatGraphModel::DescOfType(const std::string& type) const
{
    if (const ng::NodeTypeDesc* d = FindNodeType(type)) return d;
    auto it = m_unknownIndex.find(type);
    if (it != m_unknownIndex.end()) return &m_unknown[static_cast<size_t>(it->second)];
    ng::NodeTypeDesc t;
    t.id = type;
    t.title = type;
    t.category = "不明";
    t.categorySlot = 8;
    t.description = "このエンジンに登録されていないノード型です（ファイルには保持されます。診断にエラーが出ます）";
    m_unknownIndex[type] = static_cast<int>(m_unknown.size());
    m_unknown.push_back(std::move(t));
    m_unknownInfo.emplace_back();   // def = nullptr / ピンなし
    return &m_unknown.back();
}

const MatGraphModel::TypeInfo* MatGraphModel::InfoFor(const ng::NodeTypeDesc* t) const
{
    if (!t) return nullptr;
    if (!m_types.empty() && t >= m_types.data() && t < m_types.data() + m_types.size())
        return &m_infos[static_cast<size_t>(t - m_types.data())];
    for (size_t k = 0; k < m_unknown.size(); ++k)
        if (&m_unknown[k] == t) return &m_unknownInfo[k];
    return nullptr;
}

bool MatGraphModel::CanConnectTypes(ng::PinType from, ng::PinType to) const
{
    if (from == kPtOther || to == kPtOther) return false;
    if (from == kPtWild || to == kPtWild) return true;
    auto numeric = [](ng::PinType t) { return t == kPtF1 || t == kPtF2 || t == kPtF3 || t == kPtF4 || t == kPtInt || t == kPtBool || t == kPtAnyNum; };
    auto toVt = [](ng::PinType t)
    {
        switch (t)
        {
        case kPtF1: return mat::ValueType::F1;
        case kPtF2: return mat::ValueType::F2;
        case kPtF3: return mat::ValueType::F3;
        case kPtF4: return mat::ValueType::F4;
        case kPtTex: return mat::ValueType::Tex2D;
        case kPtBool: return mat::ValueType::Bool;
        case kPtInt: return mat::ValueType::Int;
        default: return mat::ValueType::Invalid;
        }
    };
    if (from == kPtAnyNum || to == kPtAnyNum) return numeric(from) && numeric(to);
    return mat::CheckCast(toVt(from), toVt(to)).ok;
}

// ---------------------------------------------------------------------------
// ID
// ---------------------------------------------------------------------------
ng::NodeId MatGraphModel::Assign(const std::string& sid, ng::NodeId wanted) const
{
    auto it = m_toInt.find(sid);
    if (it != m_toInt.end()) return it->second;
    ng::NodeId id = wanted;
    if (id == 0 || m_toStr.count(id)) id = m_nextInt;
    m_toInt[sid] = id;
    m_toStr[id] = sid;
    if (id >= m_nextInt) m_nextInt = id + 1;
    return id;
}

ng::NodeId MatGraphModel::IntOf(const std::string& id) const
{
    if (id.empty()) return 0;
    return Assign(id, 0);
}

ng::NodeId MatGraphModel::FindInt(const std::string& id) const
{
    auto it = m_toInt.find(id);
    return it == m_toInt.end() ? 0 : it->second;
}

std::string MatGraphModel::StrOf(ng::NodeId id) const
{
    auto it = m_toStr.find(id);
    return it == m_toStr.end() ? std::string() : it->second;
}

void MatGraphModel::ResetMapping()
{
    m_toInt.clear();
    m_toStr.clear();
    m_nextInt = 1;
    m_entries.clear();
    m_dirty.clear();
    m_shadow.clear();
    m_structDirty = true;
    m_canCache.clear();
    m_canRev = ~0ull;
    ++m_tick;
}

// ---------------------------------------------------------------------------
// 変更通知（G1 の MaterialGraph から）
// ---------------------------------------------------------------------------
void MatGraphModel::OnChange(const mat::GraphChange& c)
{
    using CK = mat::ChangeKind;
    switch (c.kind)
    {
    case CK::NodeMoved:
    {
        auto it = m_entries.find(FindInt(c.node));
        if (it != m_entries.end())
            if (const mat::Node* n = m_g->FindNode(c.node)) it->second.data.pos = ng::Vec2(n->x, n->y);
        return;
    }
    case CK::CommentChanged: return;
    case CK::NodeAdded:
        ++m_tick; m_structDirty = true; m_dirty.insert(c.node);
        return;
    case CK::NodeRemoved:
        ++m_tick; m_structDirty = true;
        if (const ng::NodeId i = FindInt(c.node)) m_entries.erase(i);
        m_dirty.erase(c.node);
        return;
    case CK::LinkChanged:
        ++m_tick; m_structDirty = true; m_dirty.insert(c.node);
        return;
    case CK::PropChanged: case CK::LiteralChanged:
        ++m_tick; m_dirty.insert(c.node);
        // パラメータ名は、繋がった先のノードの名前欄（TextureSample の Tex 名）に出る
        if (c.kind == CK::PropChanged && c.pin == "name")
            for (const auto& nk : m_g->Nodes())
                for (const auto& ik : nk.second.inputs)
                    if (ik.second.link && ik.second.link->node == c.node) m_dirty.insert(nk.first);
        return;
    case CK::SettingsChanged: case CK::MetaChanged:
        ++m_tick;
        return;
    case CK::Reset:
        // グラフの入れ替え（読み込み・全消去）。ID 表もキャッシュも捨てる（履歴は呼び出し側が消す）
        m_toInt.clear(); m_toStr.clear(); m_nextInt = 1;
        m_entries.clear(); m_dirty.clear(); m_shadow.clear();
        m_structDirty = true;
        ++m_tick;
        return;
    }
}

// ---------------------------------------------------------------------------
// 読み取り
// ---------------------------------------------------------------------------
void MatGraphModel::RebuildStructure() const
{
    if (!m_structDirty) return;
    m_ids.clear();
    m_edges.clear();
    m_down.clear();
    m_up.clear();
    for (const auto& kv : m_g->Nodes()) m_ids.push_back(IntOf(kv.first));
    std::sort(m_ids.begin(), m_ids.end());

    for (const auto& kv : m_g->Nodes())
    {
        const mat::Node& tn = kv.second;
        const ng::NodeTypeDesc* td = DescOfType(tn.type);
        const TypeInfo* ti = InfoFor(td);
        if (!ti || !ti->def) continue;
        for (const auto& ik : tn.inputs)
        {
            if (!ik.second.link) continue;
            const mat::PinRef& l = ik.second.link.value();
            const mat::Node* fn = m_g->FindNode(l.node);
            if (!fn) continue;
            const ng::NodeTypeDesc* fd = DescOfType(fn->type);
            const TypeInfo* fi = InfoFor(fd);
            if (!fi || !fi->def) continue;
            const int oDef = fi->def->OutputIndex(l.pin);
            const int iDef = ti->def->InputIndex(ik.first);
            if (oDef < 0 || iDef < 0) continue;
            int oUi = -1, iUi = -1;
            for (size_t k = 0; k < fi->outDef.size(); ++k) if (fi->outDef[k] == oDef) { oUi = static_cast<int>(k); break; }
            for (size_t k = 0; k < ti->inDef.size(); ++k) if (ti->inDef[k] == iDef) { iUi = static_cast<int>(k); break; }
            if (oUi < 0 || iUi < 0) continue;
            ng::Edge e;
            e.from = ng::PinRef{IntOf(l.node), static_cast<uint16_t>(oUi), true};
            e.to   = ng::PinRef{IntOf(kv.first), static_cast<uint16_t>(iUi), false};
            m_edges.push_back(e);
        }
    }
    std::sort(m_edges.begin(), m_edges.end(), [](const ng::Edge& a, const ng::Edge& b) { return a.to < b.to; });
    for (const ng::Edge& e : m_edges)
    {
        auto& d = m_down[e.from.node];
        if (std::find(d.begin(), d.end(), e.to.node) == d.end()) d.push_back(e.to.node);
        auto& u = m_up[e.to.node];
        if (std::find(u.begin(), u.end(), e.from.node) == u.end()) u.push_back(e.from.node);
    }
    m_structDirty = false;
}

const std::vector<ng::NodeId>& MatGraphModel::Nodes() const { RebuildStructure(); return m_ids; }
const std::vector<ng::Edge>& MatGraphModel::Edges() const { RebuildStructure(); return m_edges; }

const ng::Edge* MatGraphModel::FindEdgeTo(ng::PinRef input) const
{
    RebuildStructure();
    ng::Edge key;
    key.to = input;
    auto it = std::lower_bound(m_edges.begin(), m_edges.end(), key, [](const ng::Edge& a, const ng::Edge& b) { return a.to < b.to; });
    if (it != m_edges.end() && it->to == input) return &*it;
    return nullptr;
}

void MatGraphModel::Downstream(ng::NodeId n, std::vector<ng::NodeId>& out) const
{
    RebuildStructure();
    auto it = m_down.find(n);
    if (it != m_down.end()) out.insert(out.end(), it->second.begin(), it->second.end());
}

void MatGraphModel::Upstream(ng::NodeId n, std::vector<ng::NodeId>& out) const
{
    RebuildStructure();
    auto it = m_up.find(n);
    if (it != m_up.end()) out.insert(out.end(), it->second.begin(), it->second.end());
}

const mat::NodeDef* MatGraphModel::DefOf(ng::NodeId id) const
{
    const std::string s = StrOf(id);
    return s.empty() ? nullptr : m_g->DefOf(s);
}

const mat::GraphAnalysis& MatGraphModel::Analysis() const
{
    if (m_anTick != m_tick)
    {
        m_an = mat::AnalyzeGraph(*m_g);
        m_anTick = m_tick;
    }
    return m_an;
}

MatGraphModel::PinName MatGraphModel::NameOf(ng::PinRef pin) const
{
    PinName r;
    const std::string sid = StrOf(pin.node);
    const mat::Node* n = sid.empty() ? nullptr : m_g->FindNode(sid);
    if (!n) return r;
    const TypeInfo* ti = InfoFor(DescOfType(n->type));
    if (!ti || !ti->def) return r;
    if (pin.output)
    {
        if (pin.index >= ti->outDef.size()) return r;
        r.pin = ti->def->outputs[static_cast<size_t>(ti->outDef[pin.index])].name;
    }
    else
    {
        if (pin.index >= ti->inDef.size() || ti->inDef[pin.index] < 0) return r;
        r.pin = ti->def->inputs[static_cast<size_t>(ti->inDef[pin.index])].name;
    }
    r.node = sid;
    r.ok = true;
    return r;
}

ng::PinRef MatGraphModel::PinOf(const std::string& node, const std::string& pinName, bool output) const
{
    const mat::Node* n = m_g->FindNode(node);
    if (!n) return ng::PinRef{};
    const TypeInfo* ti = InfoFor(DescOfType(n->type));
    if (!ti || !ti->def) return ng::PinRef{};
    if (output)
    {
        const int d = ti->def->OutputIndex(pinName);
        for (size_t k = 0; d >= 0 && k < ti->outDef.size(); ++k)
            if (ti->outDef[k] == d) return ng::PinRef{IntOf(node), static_cast<uint16_t>(k), true};
    }
    else
    {
        const int d = ti->def->InputIndex(pinName);
        for (size_t k = 0; d >= 0 && k < ti->inDef.size(); ++k)
            if (ti->inDef[k] == d) return ng::PinRef{IntOf(node), static_cast<uint16_t>(k), false};
    }
    return ng::PinRef{};
}

const mat::PinDecl* MatGraphModel::InputDecl(const ng::NodeTypeDesc& t, size_t ui) const
{
    const TypeInfo* ti = InfoFor(&t);
    if (!ti || !ti->def || ui >= ti->inDef.size() || ti->inDef[ui] < 0) return nullptr;
    return &ti->def->inputs[static_cast<size_t>(ti->inDef[ui])];
}

std::string MatGraphModel::PropRowName(const ng::NodeTypeDesc& t, size_t ui) const
{
    const TypeInfo* ti = InfoFor(&t);
    if (!ti || ui >= ti->inProp.size()) return {};
    return ti->inProp[ui];
}

ng::PinType MatGraphModel::ResolvedPinType(ng::PinRef pin) const
{
    const std::string sid = StrOf(pin.node);
    const mat::Node* n = sid.empty() ? nullptr : m_g->FindNode(sid);
    if (!n) return kPtOther;
    const ng::NodeTypeDesc* td = DescOfType(n->type);
    const TypeInfo* ti = InfoFor(td);
    if (!ti) return kPtOther;
    const std::vector<ng::PinDesc>& pins = pin.output ? td->outputs : td->inputs;
    if (pin.index >= pins.size()) return kPtOther;
    const ng::PinType declared = pins[pin.index].type;
    if (!ti->def) return declared;
    if (declared != kPtAnyNum && declared != kPtWild) return declared;   // 型が固定（推論不要）

    const mat::NodeTypeInfo* info = Analysis().Find(sid);
    mat::ValueType t = mat::ValueType::Invalid;
    if (info)
    {
        if (pin.output)
        {
            const size_t d = static_cast<size_t>(ti->outDef[pin.index]);
            if (d < info->outTypes.size()) t = info->outTypes[d];
        }
        else if (ti->inDef[pin.index] >= 0)
        {
            const size_t d = static_cast<size_t>(ti->inDef[pin.index]);
            if (d < info->inTypes.size()) t = info->inTypes[d];
        }
    }
    if (t == mat::ValueType::Invalid) return declared;
    return PinTypeOf(t);
}

ng::ConnectCheck MatGraphModel::CanConnect(ng::PinRef out, ng::PinRef in) const
{
    ng::ConnectCheck r;
    if (!out.output || in.output) { r.reason = "出力ピンから入力ピンへ繋いでください"; return r; }
    if (out.node == in.node) { r.reason = "同じノードには繋げません"; return r; }
    const PinName a = NameOf(out), b = NameOf(in);
    if (!a.ok)
    {
        r.reason = "ピンが存在しません";
        return r;
    }
    if (!b.ok)
    {
        // プロパティ行 / 範囲外 / 未知のノード
        const std::string sid = StrOf(in.node);
        const mat::Node* n = sid.empty() ? nullptr : m_g->FindNode(sid);
        const ng::NodeTypeDesc* td = n ? DescOfType(n->type) : nullptr;
        r.reason = (td && in.index < td->inputs.size() && td->inputs[in.index].propertyOnly)
                       ? "この項目はワイヤを繋げません（値だけの欄）" : "ピンが存在しません";
        return r;
    }
    if (m_canRev != m_g->Version()) { m_canCache.clear(); m_canRev = m_g->Version(); }
    const uint64_t key = out.Key() * 0x9E3779B97F4A7C15ull ^ (in.Key() + 0x7F4A7C15ull);
    auto it = m_canCache.find(key);
    if (it != m_canCache.end()) return it->second;

    const mat::ConnectCheck c = m_g->CanConnect(a.node, a.pin, b.node, b.pin);
    r.ok = c.ok();
    r.reason = c.message;
    if (r.ok)
    {
        const ng::Edge* ex = FindEdgeTo(in);
        r.replaces = ex && !(ex->from == out);
    }
    m_canCache[key] = r;
    return r;
}

// ---------------------------------------------------------------------------
// ノードのスナップショット
// ---------------------------------------------------------------------------
std::string MatGraphModel::BuildExtra(const mat::Node& n) const
{
    const std::string prefix = n.id + "\n";
    bool anyShadow = false;
    for (auto it = m_shadow.lower_bound(prefix); it != m_shadow.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) { anyShadow = true; break; }
    if (n.props.empty() && !anyShadow)
    {
        bool anyLit = false;
        for (const auto& ik : n.inputs) if (ik.second.literal) { anyLit = true; break; }
        if (!anyLit) return {};
    }
    mat::Json j = mat::Json::object();
    mat::Json p = mat::Json::object();
    for (const auto& kv : n.props) p[kv.first] = kv.second;
    if (!p.empty()) j["p"] = std::move(p);
    mat::Json lit = mat::Json::object();
    for (const auto& ik : n.inputs)
        if (ik.second.literal) lit[ik.first] = LiteralToJson(*ik.second.literal);
    if (!lit.empty()) j["i"] = std::move(lit);
    mat::Json sh = mat::Json::object();
    for (auto it = m_shadow.lower_bound(prefix); it != m_shadow.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
        sh[it->first.substr(prefix.size())] = LiteralToJson(it->second);
    if (!sh.empty()) j["s"] = std::move(sh);
    return j.dump();
}

void MatGraphModel::FillEntry(const std::string& sid, Entry& e) const
{
    const mat::Node* gn = m_g->FindNode(sid);
    if (!gn) { e.valid = false; return; }
    ng::NodeData& d = e.data;
    d = ng::NodeData{};
    d.id = IntOf(sid);
    d.type = gn->type;
    d.pos = ng::Vec2(gn->x, gn->y);
    d.desc = DescOfType(gn->type);
    const TypeInfo* ti = InfoFor(d.desc);
    d.values.reserve(d.desc->inputs.size());
    for (size_t i = 0; i < d.desc->inputs.size(); ++i)
    {
        const ng::PinDesc& pd = d.desc->inputs[i];
        ng::PinValue v = pd.defaultValue;
        v.kind = pd.valueKind;
        if (ti && ti->def && pd.valueKind != ng::ValueKind::None)
        {
            if (ti->inDef[i] >= 0)
            {
                const mat::PinDecl& decl = ti->def->inputs[static_cast<size_t>(ti->inDef[i])];
                auto b = gn->inputs.find(decl.name);
                if (b != gn->inputs.end() && b->second.literal) v = ToPinValue(*b->second.literal, pd.valueKind);
            }
            else
            {
                const mat::PropDecl* pdc = ti->def->FindProp(ti->inProp[i]);
                auto pv = gn->props.find(ti->inProp[i]);
                if (pdc && pv != gn->props.end()) v = JsonToPin(pv->second, pd.valueKind, *pdc);
            }
        }
        d.values.push_back(v);
    }
    d.extra = BuildExtra(*gn);

    // 名前欄
    if (d.desc->hasLabel && ti && ti->def)
    {
        auto str = [&](const char* key) -> std::string
        {
            auto it = gn->props.find(key);
            return (it != gn->props.end() && it->second.is_string()) ? it->second.get<std::string>() : std::string();
        };
        if (ti->def->isParameter) d.label = str("name");
        else if (ti->def->isCustom)
        {
            std::string code = str("code");
            const size_t nl = code.find_first_of("\r\n");
            if (nl != std::string::npos) code.resize(nl);
            if (code.size() > 40) code.resize(40);
            d.label = code;
        }
        else
        {
            // Tex が繋がっているときは、繋がっている先の名前（TextureParameter の名前）を出す。未接続ならこのノードのテクスチャ名
            auto tin = gn->inputs.find("Tex");
            const bool linked = tin != gn->inputs.end() && tin->second.link;
            const std::string tex = str("texture");
            if (linked)
            {
                const mat::Node* src = m_g->FindNode(tin->second.link->node);
                const mat::NodeDef* sd = src ? m_g->DefOf(src->id) : nullptr;
                if (src && sd && sd->isParameter)
                {
                    auto nm = src->props.find("name");
                    if (nm != src->props.end() && nm->second.is_string()) d.label = nm->second.get<std::string>();
                }
                else if (sd) d.label = sd->displayName;
            }
            else d.label = tex.empty() ? std::string("(テクスチャ未設定)") : Leaf(tex);
        }
    }
    e.valid = true;
}

const ng::NodeData* MatGraphModel::FindNode(ng::NodeId id) const
{
    auto sit = m_toStr.find(id);
    if (sit == m_toStr.end()) return nullptr;
    const std::string& sid = sit->second;
    auto it = m_entries.find(id);
    const bool dirty = m_dirty.erase(sid) != 0;
    if (it == m_entries.end() || dirty || !it->second.valid)
    {
        Entry& e = m_entries[id];
        FillEntry(sid, e);
        return e.valid ? &e.data : nullptr;
    }
    return &it->second.data;
}

// ---------------------------------------------------------------------------
// 生の操作
// ---------------------------------------------------------------------------
std::string MatGraphModel::UniqueParamName(const std::string& base, ng::NodeId except) const
{
    std::unordered_set<std::string> used;
    const std::string exceptS = StrOf(except);
    for (const auto& kv : m_g->Nodes())
    {
        if (kv.first == exceptS) continue;
        const mat::NodeDef* def = m_g->DefOf(kv.first);
        if (!def || !def->isParameter) continue;
        const mat::Json nm = m_g->GetNodeProp(kv.first, "name");
        if (nm.is_string()) used.insert(nm.get<std::string>());
    }
    if (!used.count(base)) return base;
    // "Scalar_3" → 語幹 "Scalar" に番号を付け直す
    std::string stem = base;
    const size_t us = stem.find_last_of('_');
    if (us != std::string::npos && us + 1 < stem.size() && stem.find_first_not_of("0123456789", us + 1) == std::string::npos)
        stem.resize(us);
    for (int k = 1; k < 100000; ++k)
    {
        const std::string c = stem + "_" + std::to_string(k);
        if (!used.count(c)) return c;
    }
    return base;
}

void MatGraphModel::ApplyExtra(const std::string& sid, const std::string& extra)
{
    const mat::Json j = mat::Json::parse(extra, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    if (auto p = j.find("p"); p != j.end() && p->is_object())
        for (auto it = p->begin(); it != p->end(); ++it) m_g->SetNodeProp(sid, it.key(), it.value());
    if (auto l = j.find("i"); l != j.end() && l->is_object())
        for (auto it = l->begin(); it != l->end(); ++it)
        {
            mat::Value v;
            if (LiteralFromJson(it.value(), v)) m_g->SetLiteral(sid, it.key(), v);
        }
    if (auto sh = j.find("s"); sh != j.end() && sh->is_object())
        for (auto it = sh->begin(); it != sh->end(); ++it)
        {
            mat::Value v;
            // 追加した時点ではワイヤは無い（コピペ・削除の Undo は、あとからワイヤを繋ぎ直す）ので、隠れていたリテラルは見える状態へ戻す。
            // ワイヤが繋ぎ直されるとき ConnectUnchecked が再び退避する。
            if (LiteralFromJson(it.value(), v)) m_g->SetLiteral(sid, it.key(), v);
        }
}

ng::NodeId MatGraphModel::AddNode(const ng::NodeData& d)
{
    const ng::NodeTypeDesc* desc = FindNodeType(d.type);
    if (!desc)
    {
        // 未知の型は、すでに合成記述がある（=読み込んだことがある）ときだけ許す（削除の Undo）
        if (m_unknownIndex.find(d.type) == m_unknownIndex.end()) return 0;
        desc = DescOfType(d.type);
    }
    std::string sid;
    if (d.id != 0)
    {
        auto it = m_toStr.find(d.id);
        if (it != m_toStr.end())
        {
            sid = it->second;
            if (m_g->FindNode(sid)) return 0;   // 使用中
        }
        else
        {
            sid = m_g->GenerateNodeId();
            Assign(sid, d.id);
        }
    }
    else
    {
        sid = m_g->GenerateNodeId();
        Assign(sid, 0);
    }
    if (m_g->AddNode(d.type, d.pos.x, d.pos.y, sid).empty()) return 0;
    const ng::NodeId id = FindInt(sid);
    if (!d.extra.empty()) ApplyExtra(sid, d.extra);
    else if (d.values.size() == desc->inputs.size())
    {
        for (size_t i = 0; i < d.values.size(); ++i)
            if (d.values[i] != [&] { ng::PinValue v = desc->inputs[i].defaultValue; v.kind = desc->inputs[i].valueKind; return v; }())
                SetPinValue(ng::PinRef{id, static_cast<uint16_t>(i), false}, d.values[i]);
    }
    // 新しく作るパラメータの名前は重複させない（コピペ・複製・パレットからの追加）。履歴の復元（d.id != 0）は名前を触らない
    if (d.id == 0)
        if (const mat::NodeDef* def = m_g->DefOf(sid); def && def->isParameter)
        {
            const mat::Json nm = m_g->GetNodeProp(sid, "name");
            if (nm.is_string())
            {
                const std::string u = UniqueParamName(nm.get<std::string>(), id);
                if (u != nm.get<std::string>()) m_g->SetNodeProp(sid, "name", u);
            }
        }
    return id;
}

bool MatGraphModel::RemoveNode(ng::NodeId id)
{
    const std::string sid = StrOf(id);
    if (sid.empty()) return false;
    mat::RemovedNode removed;
    if (!m_g->RemoveNode(sid, &removed)) return false;
    // 出力先の入力が接続で隠していたリテラルを戻す（ワイヤが消えて未接続に戻るので）
    for (const mat::Edge& e : removed.outgoing)
    {
        auto it = m_shadow.find(e.toNode + "\n" + e.toPin);
        if (it == m_shadow.end()) continue;
        const mat::InputBinding* b = m_g->Binding(e.toNode, e.toPin);
        if (!b || !b->link) m_g->SetLiteral(e.toNode, e.toPin, it->second);
        m_shadow.erase(it);
    }
    // このノード自身の退避は、履歴のスナップショット（extra）へ移ったのでここでは捨てる
    const std::string prefix = sid + "\n";
    for (auto it = m_shadow.lower_bound(prefix); it != m_shadow.end() && it->first.compare(0, prefix.size(), prefix) == 0;)
        it = m_shadow.erase(it);
    return true;
}

bool MatGraphModel::Connect(ng::PinRef output, ng::PinRef input)
{
    if (!CanConnect(output, input).ok) return false;
    return ConnectUnchecked(output, input);
}

bool MatGraphModel::ConnectUnchecked(ng::PinRef output, ng::PinRef input)
{
    if (!output.output || input.output || output.node == input.node) return false;
    const PinName a = NameOf(output), b = NameOf(input);
    if (!a.ok || !b.ok) return false;
    // 接続で消えるリテラルを退避（切断 / Undo で戻す）
    const mat::InputBinding* bind = m_g->Binding(b.node, b.pin);
    if (bind && bind->literal && !bind->link) m_shadow[b.node + "\n" + b.pin] = *bind->literal;
    return m_g->ConnectUnchecked(a.node, a.pin, b.node, b.pin);
}

bool MatGraphModel::Disconnect(ng::PinRef input)
{
    if (input.output) return false;
    const PinName b = NameOf(input);
    if (!b.ok) return false;
    const mat::InputBinding* bind = m_g->Binding(b.node, b.pin);
    if (!bind || !bind->link) return false;
    if (!m_g->Disconnect(b.node, b.pin)) return false;
    auto it = m_shadow.find(b.node + "\n" + b.pin);
    if (it != m_shadow.end())
    {
        m_g->SetLiteral(b.node, b.pin, it->second);
        m_shadow.erase(it);
    }
    return true;
}

void MatGraphModel::SetNodePos(ng::NodeId id, ng::Vec2 pos)
{
    const std::string sid = StrOf(id);
    if (sid.empty()) return;
    m_g->MoveNode(sid, pos.x, pos.y);
}

void MatGraphModel::SetPinValue(ng::PinRef input, const ng::PinValue& v)
{
    if (input.output) return;
    const std::string sid = StrOf(input.node);
    const mat::Node* gn = sid.empty() ? nullptr : m_g->FindNode(sid);
    if (!gn) return;
    const ng::NodeTypeDesc* td = DescOfType(gn->type);
    const TypeInfo* ti = InfoFor(td);
    if (!ti || !ti->def || input.index >= td->inputs.size()) return;
    const ng::PinDesc& pd = td->inputs[input.index];
    if (pd.valueKind == ng::ValueKind::None) return;
    ng::PinValue nv = v;
    nv.kind = pd.valueKind;

    if (ti->inDef[input.index] >= 0)
    {
        const mat::PinDecl& decl = ti->def->inputs[static_cast<size_t>(ti->inDef[input.index])];
        if (const mat::InputBinding* cb = m_g->Binding(sid, decl.name); cb && cb->link) return;   // 接続中のピンの値は編集しない（値欄は隠れている）
        const mat::Value lit =ToLiteral(nv, pd.valueKind, decl.mode == mat::PinMode::Fixed ? decl.type : mat::ValueType::F1);
        const bool isDefault = decl.hasDefault && decl.defaultValue == lit;   // 値欄が使わない成分（Float の f[1..3] など）に依らず、G1 のリテラルとして比べる
        if (isDefault)
        {
            const mat::InputBinding* b = m_g->Binding(sid, decl.name);
            if (b && b->literal && !b->link) m_g->ClearLiteral(sid, decl.name);
        }
        else
            m_g->SetLiteral(sid, decl.name, lit);
    }
    else
    {
        const mat::PropDecl* pdc = ti->def->FindProp(ti->inProp[input.index]);
        if (pdc) m_g->SetNodeProp(sid, pdc->name, PinToJson(nv, *pdc));
    }
}

void MatGraphModel::Clear()
{
    m_g->Clear();
}

// ---------------------------------------------------------------------------
// プロパティ
// ---------------------------------------------------------------------------
mat::Json MatGraphModel::GetNodeProp(ng::NodeId id, const std::string& key) const
{
    const std::string sid = StrOf(id);
    if (sid.empty()) return mat::Json();
    return m_g->GetNodeProp(sid, key);
}

bool MatGraphModel::SetNodeProp(ng::NodeId id, const std::string& key, const mat::Json& value)
{
    const std::string sid = StrOf(id);
    if (sid.empty()) return false;
    return m_g->SetNodeProp(sid, key, value);
}

std::vector<MatGraphModel::PropInfo> MatGraphModel::PropsOf(ng::NodeId id) const
{
    std::vector<PropInfo> out;
    const mat::NodeDef* def = DefOf(id);
    if (!def) return out;
    for (const mat::PropDecl& pd : def->props)
    {
        PropInfo pi;
        pi.decl = &pd;
        pi.inlineRow = IsInlineProp(pd) && KindForProp(*def, pd) != ng::ValueKind::None;
        out.push_back(pi);
    }
    return out;
}

bool MatGraphModel::CheckInvariants(std::string* why) const
{
    for (const auto& kv : m_shadow)
    {
        const size_t nl = kv.first.find('\n');
        const std::string sid = kv.first.substr(0, nl), pin = kv.first.substr(nl + 1);
        const mat::InputBinding* b = m_g->Binding(sid, pin);
        if (!b || !b->link || b->literal)
        {
            if (why) *why = "退避リテラルが不整合: " + sid + "." + pin + (b ? (b->link ? " (リテラルもある)" : " (リンクなし)") : " (束縛なし)");
            return false;
        }
    }
    // 既定値と同じ値の明示リテラルは持たない（UI の SetPinValue は既定値へ戻すと ClearLiteral する。Undo で厳密に戻せるよう、この状態を作らない）
    for (const auto& nk : m_g->Nodes())
    {
        const mat::NodeDef* def = m_g->DefOf(nk.first);
        if (!def) continue;
        for (const auto& ik : nk.second.inputs)
        {
            if (!ik.second.literal) continue;
            const mat::PinDecl* d = def->FindInput(ik.first);
            if (d && d->hasDefault && d->defaultValue == *ik.second.literal)
            {
                if (why) *why = "既定値と同じリテラル: " + nk.first + "." + ik.first;
                return false;
            }
        }
    }
    return true;
}

} // namespace dx12e::mg
