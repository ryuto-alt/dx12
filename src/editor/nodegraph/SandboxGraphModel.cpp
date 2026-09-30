#include "editor/nodegraph/SandboxGraphModel.h"

#include <algorithm>

namespace dx12e::ng
{

namespace
{
using T = SandboxGraphModel;

PinValue VF(float a) { PinValue v; v.kind = ValueKind::Float; v.f[0] = a; return v; }
PinValue V2(float a, float b) { PinValue v; v.kind = ValueKind::Vec2; v.f[0] = a; v.f[1] = b; return v; }
PinValue V3(float a, float b, float c) { PinValue v; v.kind = ValueKind::Vec3; v.f[0] = a; v.f[1] = b; v.f[2] = c; return v; }
PinValue VC(float r, float g, float b, float a = 1.0f) { PinValue v; v.kind = ValueKind::Color; v.f[0] = r; v.f[1] = g; v.f[2] = b; v.f[3] = a; return v; }
PinValue VE(int i) { PinValue v; v.kind = ValueKind::Enum; v.f[0] = static_cast<float>(i); return v; }

PinDesc Out(const char* name, int type)
{
    PinDesc p; p.name = name; p.type = type; return p;
}
PinDesc In(const char* name, int type)
{
    PinDesc p; p.name = name; p.type = type; return p;
}
// 値欄つき入力（未接続のとき定数として使える）
PinDesc InV(const char* name, int type, const PinValue& def, float mn = -1e9f, float mx = 1e9f, float speed = 0.01f)
{
    PinDesc p; p.name = name; p.type = type; p.valueKind = def.kind; p.defaultValue = def;
    p.rangeMin = mn; p.rangeMax = mx; p.dragSpeed = speed;
    return p;
}
// プロパティ行（ピンなし）
PinDesc Prop(const char* name, int type, const PinValue& def, float mn = -1e9f, float mx = 1e9f, float speed = 0.01f)
{
    PinDesc p = InV(name, type, def, mn, mx, speed);
    p.propertyOnly = true;
    return p;
}

NodeTypeDesc Node(const char* id, const char* title, const char* category, int catSlot, const char* keywords,
                  std::vector<PinDesc> ins, std::vector<PinDesc> outs, char hotkey = 0)
{
    NodeTypeDesc t;
    t.id = id; t.title = title; t.category = category; t.categorySlot = catSlot; t.keywords = keywords;
    t.inputs = std::move(ins); t.outputs = std::move(outs); t.hotkey = hotkey;
    return t;
}

bool IsNumeric(PinType t) { return t == T::kFloat || t == T::kVec2 || t == T::kVec3 || t == T::kVec4 || t == T::kAny; }
int  Dim(PinType t)
{
    switch (t) { case T::kFloat: return 1; case T::kVec2: return 2; case T::kVec3: return 3; case T::kVec4: return 4; default: return 1; }
}
PinType FromDim(int d)
{
    switch (d) { case 2: return T::kVec2; case 3: return T::kVec3; case 4: return T::kVec4; default: return T::kFloat; }
}
} // namespace

SandboxGraphModel::SandboxGraphModel()
{
    m_pinTypes[kFloat]    = {"Float",    0, PinShape::Circle};
    m_pinTypes[kVec2]     = {"Vector2",  1, PinShape::CircleLine1};
    m_pinTypes[kVec3]     = {"Vector3",  2, PinShape::CircleLine2};
    m_pinTypes[kVec4]     = {"Vector4",  3, PinShape::Diamond};
    m_pinTypes[kTexture]  = {"Texture",  4, PinShape::RoundedSquare};
    m_pinTypes[kBool]     = {"Bool",     5, PinShape::Square};
    m_pinTypes[kAny]      = {"Any (数値)", 6, PinShape::Circle};
    m_pinTypes[kWildcard] = {"Wildcard", 7, PinShape::Pentagon};
    BuildTypes();
}

void SandboxGraphModel::BuildTypes()
{
    auto add = [&](NodeTypeDesc t) { m_typeIndex[t.id] = static_cast<int>(m_types.size()); m_types.push_back(std::move(t)); };
    const char* kConst = "定数", *kParam = "パラメータ", *kTex = "テクスチャ", *kCoord = "座標", *kMath = "数学";
    const char* kVec = "ベクトル", *kCol = "色", *kUtil = "ユーティリティ", *kMisc = "その他", *kOut = "出力";
    m_types.reserve(48);

    // ---- 定数 (slot 0) ----
    add(Node("float", "Float", kConst, 0, "定数 数値 スカラー constant scalar 1",
             {Prop("Value", kFloat, VF(0.0f), -1e6f, 1e6f, 0.01f)}, {Out("Out", kFloat)}, '1'));
    add(Node("vec2", "Vector2", kConst, 0, "定数 ベクトル2 constant vector2 2",
             {Prop("Value", kVec2, V2(0.0f, 0.0f))}, {Out("Out", kVec2)}, '2'));
    add(Node("vec3", "Vector3", kConst, 0, "定数 ベクトル3 constant vector3 3",
             {Prop("Value", kVec3, V3(0.0f, 0.0f, 0.0f))}, {Out("Out", kVec3)}, '3'));
    add(Node("color", "Color", kConst, 0, "定数 色 カラー constant color 4",
             {Prop("Value", kVec4, VC(1.0f, 1.0f, 1.0f, 1.0f))}, {Out("RGB", kVec3), Out("A", kFloat)}, '4'));

    // ---- パラメータ (slot 1) ----
    add(Node("scalarParam", "ScalarParameter", kParam, 1, "パラメータ スカラー parameter scalar float",
             {Prop("Default", kFloat, VF(0.5f), 0.0f, 1.0f, 0.005f)}, {Out("Value", kFloat)}, 'S'));
    add(Node("colorParam", "VectorParameter", kParam, 1, "パラメータ 色 parameter vector color",
             {Prop("Default", kVec4, VC(0.8f, 0.4f, 0.2f, 1.0f))}, {Out("RGB", kVec3), Out("A", kFloat)}, 'V'));

    // ---- テクスチャ (slot 2) ----
    {
        PinDesc p = Prop("Asset", kTexture, VE(0));
        p.enumOptions = {"Checker", "Rock_Diffuse", "Rock_Normal", "Noise", "Gradient"};
        add(Node("texture", "TextureObject", kTex, 2, "テクスチャ 画像 texture object asset", {p}, {Out("Tex", kTexture)}));
    }
    {
        NodeTypeDesc t = Node("sample", "TextureSample", kTex, 2, "テクスチャ サンプル 画像 texture sample",
                              {In("Tex", kTexture), In("UV", kVec2)},
                              {Out("RGB", kVec3), Out("R", kFloat), Out("G", kFloat), Out("B", kFloat), Out("A", kFloat)}, 'T');
        t.hasPreview = true;
        add(std::move(t));
    }

    // ---- 座標 (slot 3) ----
    add(Node("texcoord", "TexCoord", kCoord, 3, "座標 UV texcoord uv",
             {Prop("Tiling", kVec2, V2(1.0f, 1.0f)), Prop("Offset", kVec2, V2(0.0f, 0.0f))}, {Out("UV", kVec2)}, 'U'));
    add(Node("panner", "Panner", kCoord, 3, "座標 スクロール panner scroll uv",
             {In("UV", kVec2), InV("Speed", kVec2, V2(0.1f, 0.0f))}, {Out("UV", kVec2)}, 'P'));
    add(Node("worldpos", "WorldPosition", kCoord, 3, "座標 ワールド位置 world position", {}, {Out("XYZ", kVec3)}));

    // ---- 数学 (slot 4)。Any = 多相（上流から型を推論）----
    add(Node("add", "Add", kMath, 4, "数学 加算 足し算 add plus +",
             {InV("A", kAny, VF(0.0f)), InV("B", kAny, VF(0.0f))}, {Out("Out", kAny)}, 'A'));
    add(Node("subtract", "Subtract", kMath, 4, "数学 減算 引き算 subtract minus -",
             {InV("A", kAny, VF(0.0f)), InV("B", kAny, VF(0.0f))}, {Out("Out", kAny)}));
    add(Node("multiply", "Multiply", kMath, 4, "数学 乗算 掛け算 multiply times *",
             {InV("A", kAny, VF(1.0f)), InV("B", kAny, VF(1.0f))}, {Out("Out", kAny)}, 'M'));
    add(Node("divide", "Divide", kMath, 4, "数学 除算 割り算 divide /",
             {InV("A", kAny, VF(1.0f)), InV("B", kAny, VF(1.0f))}, {Out("Out", kAny)}, 'D'));
    add(Node("lerp", "Lerp", kMath, 4, "数学 混合 線形補間 ブレンド lerp mix blend",
             {InV("A", kAny, VF(0.0f)), InV("B", kAny, VF(1.0f)), InV("Alpha", kFloat, VF(0.5f), 0.0f, 1.0f, 0.005f)}, {Out("Out", kAny)}, 'L'));
    add(Node("saturate", "Saturate", kMath, 4, "数学 0-1 クランプ saturate clamp01", {In("X", kAny)}, {Out("Out", kAny)}));
    add(Node("oneminus", "OneMinus", kMath, 4, "数学 反転 1-x one minus invert", {In("X", kAny)}, {Out("Out", kAny)}, 'O'));
    add(Node("power", "Power", kMath, 4, "数学 べき乗 power pow",
             {In("Base", kAny), InV("Exp", kFloat, VF(2.0f), 0.0f, 32.0f, 0.02f)}, {Out("Out", kAny)}, 'E'));
    add(Node("clamp", "Clamp", kMath, 4, "数学 範囲 クランプ clamp",
             {In("X", kAny), InV("Min", kFloat, VF(0.0f)), InV("Max", kFloat, VF(1.0f))}, {Out("Out", kAny)}));
    add(Node("abs", "Abs", kMath, 4, "数学 絶対値 abs", {In("X", kAny)}, {Out("Out", kAny)}));
    add(Node("sine", "Sine", kMath, 4, "数学 サイン sin sine wave",
             {In("X", kAny), InV("Period", kFloat, VF(1.0f), 0.001f, 1000.0f, 0.02f)}, {Out("Out", kAny)}));

    // ---- ベクトル (slot 5) ----
    add(Node("append2", "Append (2)", kVec, 5, "ベクトル 結合 append merge",
             {InV("A", kFloat, VF(0.0f)), InV("B", kFloat, VF(0.0f))}, {Out("XY", kVec2)}));
    add(Node("append3", "Append (3)", kVec, 5, "ベクトル 結合 append merge",
             {In("XY", kVec2), InV("Z", kFloat, VF(0.0f))}, {Out("XYZ", kVec3)}));
    add(Node("split", "Split", kVec, 5, "ベクトル 分解 split mask component", {In("XYZ", kVec3)},
             {Out("X", kFloat), Out("Y", kFloat), Out("Z", kFloat)}));
    add(Node("normalize", "Normalize", kVec, 5, "ベクトル 正規化 normalize", {In("X", kAny)}, {Out("Out", kAny)}, 'N'));
    add(Node("dot", "Dot", kVec, 5, "ベクトル 内積 dot", {In("A", kAny), In("B", kAny)}, {Out("Out", kFloat)}));
    add(Node("cross", "Cross", kVec, 5, "ベクトル 外積 cross", {In("A", kVec3), In("B", kVec3)}, {Out("Out", kVec3)}));

    // ---- 色 (slot 6) ----
    add(Node("desaturate", "Desaturation", kCol, 6, "色 彩度 グレースケール desaturate gray",
             {In("Color", kVec3), InV("Fraction", kFloat, VF(1.0f), 0.0f, 1.0f, 0.005f)}, {Out("Out", kVec3)}));
    add(Node("tint", "Tint", kCol, 6, "色 色付け tint multiply color",
             {In("Color", kVec3), InV("Tint", kVec4, VC(1.0f, 0.8f, 0.6f, 1.0f))}, {Out("Out", kVec3)}));

    // ---- ユーティリティ (slot 7) ----
    add(Node("time", "Time", kUtil, 7, "時間 タイム time seconds", {}, {Out("Seconds", kFloat)}));
    add(Node("fresnel", "Fresnel", kUtil, 7, "フレネル 縁 fresnel rim",
             {InV("Exponent", kFloat, VF(5.0f), 0.0f, 32.0f, 0.05f), InV("BaseReflect", kFloat, VF(0.04f), 0.0f, 1.0f, 0.002f)}, {Out("Out", kFloat)}));
    {
        NodeTypeDesc t = Node("noise", "Noise", kUtil, 7, "ノイズ noise procedural",
                              {In("UV", kVec2), InV("Scale", kFloat, VF(8.0f), 0.0f, 128.0f, 0.1f)}, {Out("Out", kFloat)});
        t.hasPreview = true;
        add(std::move(t));
    }
    add(Node("if", "If", kUtil, 7, "条件 分岐 if compare",
             {In("A", kFloat), In("B", kFloat), In("A>B", kAny), In("A=B", kAny), In("A<B", kAny)}, {Out("Out", kAny)}, 'I'));
    {
        PinDesc p = InV("Mode", kFloat, VE(0));
        p.enumOptions = {"Lit", "Unlit", "Masked"};
        add(Node("switch", "Switch", kUtil, 7, "スイッチ 切り替え switch static",
                 {p, In("On", kAny), In("Off", kAny)}, {Out("Out", kAny)}));
    }
    {
        PinDesc p = InV("Enabled", kBool, PinValue{ValueKind::Bool, {1.0f, 0, 0, 0}});
        add(Node("gate", "Gate", kUtil, 7, "ゲート 有効 bool switch", {p, In("In", kAny)}, {Out("Out", kAny)}));
    }

    // ---- 出力 (slot 9) ----
    add(Node("output", "MaterialOutput", kOut, 9, "出力 マテリアル output result",
             {InV("BaseColor", kVec3, VC(0.5f, 0.5f, 0.5f, 1.0f)),
              InV("Metallic", kFloat, VF(0.0f), 0.0f, 1.0f, 0.005f),
              InV("Roughness", kFloat, VF(0.5f), 0.0f, 1.0f, 0.005f),
              In("Normal", kVec3),
              InV("Emissive", kVec3, VC(0.0f, 0.0f, 0.0f, 1.0f)),
              InV("Opacity", kFloat, VF(1.0f), 0.0f, 1.0f, 0.005f)},
             {}));

    // ---- その他 (slot 8) ----
    {
        NodeTypeDesc t = Node("reroute", "Reroute", kMisc, 8, "リルート 中継 reroute pass", {In("", kWildcard)}, {Out("", kWildcard)});
        t.isReroute = true;
        add(std::move(t));
    }
}

const NodeTypeDesc* SandboxGraphModel::FindNodeType(const std::string& id) const
{
    auto it = m_typeIndex.find(id);
    return it == m_typeIndex.end() ? nullptr : &m_types[static_cast<size_t>(it->second)];
}

const PinTypeDesc& SandboxGraphModel::PinTypeInfo(PinType t) const
{
    return m_pinTypes[(t >= 0 && t < kTypeCount) ? t : 0];
}

int SandboxGraphModel::InputIndex(const std::string& typeId, const std::string& pinName) const
{
    const NodeTypeDesc* t = FindNodeType(typeId);
    if (!t) return -1;
    for (size_t i = 0; i < t->inputs.size(); ++i) if (t->inputs[i].name == pinName) return static_cast<int>(i);
    return -1;
}
int SandboxGraphModel::OutputIndex(const std::string& typeId, const std::string& pinName) const
{
    const NodeTypeDesc* t = FindNodeType(typeId);
    if (!t) return -1;
    for (size_t i = 0; i < t->outputs.size(); ++i) if (t->outputs[i].name == pinName) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------------------
// 型の規則
// ---------------------------------------------------------------------------
bool SandboxGraphModel::CanConnectTypes(PinType from, PinType to) const
{
    if (from == kWildcard || to == kWildcard) return true;
    if (IsNumeric(from) && IsNumeric(to)) return true;
    return from == to;
}

bool SandboxGraphModel::TypeOk(PinType from, PinType to) const
{
    if (from == kWildcard || to == kWildcard) return true;
    if (IsNumeric(from) && IsNumeric(to))
    {
        if (to == kAny) return true;
        const int df = Dim(from), dt = Dim(to);
        return df == 1 || df >= dt;    // F1 → Fn（拡張）/ Fn → Fm（m ≤ n。切り詰め）
    }
    return from == to;
}

bool SandboxGraphModel::ConsumersAccept(NodeId reroute, PinType t, int depth) const
{
    if (depth > 64) return true;
    for (const Edge& e : m_edges)
    {
        if (e.from.node != reroute) continue;
        auto it = m_nodes.find(e.to.node);
        if (it == m_nodes.end() || !it->second.desc) continue;
        const NodeTypeDesc& d = *it->second.desc;
        if (d.isReroute) { if (!ConsumersAccept(e.to.node, t, depth + 1)) return false; }
        else if (!TypeOk(t, d.inputs[e.to.index].type)) return false;
    }
    return true;
}

ConnectCheck SandboxGraphModel::CanConnect(PinRef out, PinRef in) const
{
    ConnectCheck r;
    if (!out.output || in.output) { r.reason = "出力ピンから入力ピンへ繋いでください"; return r; }
    auto no = m_nodes.find(out.node), ni = m_nodes.find(in.node);
    if (no == m_nodes.end() || ni == m_nodes.end() || !no->second.desc || !ni->second.desc) { r.reason = "ノードが存在しません"; return r; }
    const NodeTypeDesc& dout = *no->second.desc;
    const NodeTypeDesc& din  = *ni->second.desc;
    if (out.index >= dout.outputs.size() || in.index >= din.inputs.size()) { r.reason = "ピンが存在しません"; return r; }
    if (din.inputs[in.index].propertyOnly) { r.reason = "この項目はワイヤを繋げません（値だけの欄）"; return r; }
    if (out.node == in.node) { r.reason = "同じノードには繋げません"; return r; }

    const PinType from = ResolvedPinType(out);
    const PinType to   = din.inputs[in.index].type;
    if (!TypeOk(from, to))
    {
        r.reason = std::string("型が合いません: ") + PinTypeInfo(from).name + " → " + PinTypeInfo(to).name;
        if (IsNumeric(from) && IsNumeric(to) && Dim(from) > 1 && Dim(from) < Dim(to)) r.reason += "（Append で拡張してください）";
        return r;
    }
    if (din.isReroute && !ConsumersAccept(in.node, from, 0))
    {
        r.reason = "下流のピンがその型を受け付けません";
        return r;
    }
    if (WouldCreateCycle(*this, out, in)) { r.reason = "循環になるため接続できません"; return r; }
    r.ok = true;
    const Edge* ex = FindEdgeTo(in);
    r.replaces = ex && !(ex->from == out);
    return r;
}

PinType SandboxGraphModel::ResolveImpl(PinRef pin, int depth) const
{
    auto it = m_nodes.find(pin.node);
    if (it == m_nodes.end() || !it->second.desc || depth > 256) return kFloat;
    const NodeTypeDesc& d = *it->second.desc;
    const std::vector<PinDesc>& pins = pin.output ? d.outputs : d.inputs;
    if (pin.index >= pins.size()) return kFloat;
    const PinType decl = pins[pin.index].type;
    if (decl != kAny && decl != kWildcard) return decl;

    const uint64_t key = pin.Key();
    auto c = m_resolveCache.find(key);
    if (c != m_resolveCache.end()) return c->second;

    PinType res = kFloat;
    if (decl == kWildcard)
    {
        res = kWildcard;
        if (const Edge* e = FindEdgeTo(PinRef{pin.node, 0, false}))
            res = ResolveImpl(e->from, depth + 1);
    }
    else
    {
        // 多相: 同じノードの Any 入力に繋がる上流の最大次元
        int dim = 1;
        for (size_t i = 0; i < d.inputs.size(); ++i)
        {
            if (d.inputs[i].type != kAny) continue;
            if (const Edge* e = FindEdgeTo(PinRef{pin.node, static_cast<uint16_t>(i), false}))
            {
                const PinType u = ResolveImpl(e->from, depth + 1);
                if (IsNumeric(u)) dim = std::max(dim, Dim(u));
            }
        }
        res = FromDim(dim);
    }
    m_resolveCache[key] = res;
    return res;
}

PinType SandboxGraphModel::ResolvedPinType(PinRef pin) const
{
    if (m_resolveRev != m_rev) { m_resolveCache.clear(); m_resolveRev = m_rev; }
    return ResolveImpl(pin, 0);
}

// ---------------------------------------------------------------------------
// 隣接
// ---------------------------------------------------------------------------
void SandboxGraphModel::RebuildAdjacency() const
{
    if (m_adjRev == m_rev) return;
    m_down.clear();
    m_up.clear();
    for (const Edge& e : m_edges)
    {
        auto& d = m_down[e.from.node];
        if (std::find(d.begin(), d.end(), e.to.node) == d.end()) d.push_back(e.to.node);
        auto& u = m_up[e.to.node];
        if (std::find(u.begin(), u.end(), e.from.node) == u.end()) u.push_back(e.from.node);
    }
    m_adjRev = m_rev;
}

void SandboxGraphModel::Downstream(NodeId n, std::vector<NodeId>& out) const
{
    RebuildAdjacency();
    auto it = m_down.find(n);
    if (it != m_down.end()) out.insert(out.end(), it->second.begin(), it->second.end());
}

void SandboxGraphModel::Upstream(NodeId n, std::vector<NodeId>& out) const
{
    RebuildAdjacency();
    auto it = m_up.find(n);
    if (it != m_up.end()) out.insert(out.end(), it->second.begin(), it->second.end());
}

// ---------------------------------------------------------------------------
// 読み取り
// ---------------------------------------------------------------------------
const NodeData* SandboxGraphModel::FindNode(NodeId id) const
{
    auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : &it->second;
}

const Edge* SandboxGraphModel::FindEdgeTo(PinRef input) const
{
    Edge key;
    key.to = input;
    auto it = std::lower_bound(m_edges.begin(), m_edges.end(), key, ByTo{});
    if (it != m_edges.end() && it->to == input) return &*it;
    return nullptr;
}

// ---------------------------------------------------------------------------
// 生の操作
// ---------------------------------------------------------------------------
NodeId SandboxGraphModel::AddNode(const NodeData& d)
{
    const NodeTypeDesc* t = FindNodeType(d.type);
    if (!t) return 0;
    const NodeId id = d.id ? d.id : m_nextId;
    if (m_nodes.count(id)) return 0;
    NodeData n;
    n.id = id;
    n.type = d.type;
    n.pos = d.pos;
    n.desc = t;
    if (d.values.size() == t->inputs.size()) n.values = d.values;
    else
    {
        n.values.clear();
        for (const PinDesc& p : t->inputs) { PinValue v = p.defaultValue; v.kind = p.valueKind; n.values.push_back(v); }
    }
    for (size_t i = 0; i < n.values.size(); ++i) n.values[i].kind = t->inputs[i].valueKind;
    m_nodes.emplace(id, std::move(n));
    m_ids.insert(std::lower_bound(m_ids.begin(), m_ids.end(), id), id);
    if (id >= m_nextId) m_nextId = id + 1;
    ++m_rev;
    return id;
}

bool SandboxGraphModel::RemoveNode(NodeId id)
{
    auto it = m_nodes.find(id);
    if (it == m_nodes.end()) return false;
    m_edges.erase(std::remove_if(m_edges.begin(), m_edges.end(),
                                 [&](const Edge& e) { return e.from.node == id || e.to.node == id; }), m_edges.end());
    m_nodes.erase(it);
    m_ids.erase(std::lower_bound(m_ids.begin(), m_ids.end(), id));
    ++m_rev;
    return true;
}

bool SandboxGraphModel::Connect(PinRef output, PinRef input)
{
    if (!CanConnect(output, input).ok) return false;
    return ConnectUnchecked(output, input);
}

bool SandboxGraphModel::ConnectUnchecked(PinRef output, PinRef input)
{
    // 構造だけ確かめる（向き・存在・ピン番号・ワイヤを受けられる入力か・自己接続でない）
    if (!output.output || input.output || output.node == input.node) return false;
    auto no = m_nodes.find(output.node), ni = m_nodes.find(input.node);
    if (no == m_nodes.end() || ni == m_nodes.end() || !no->second.desc || !ni->second.desc) return false;
    if (output.index >= no->second.desc->outputs.size() || input.index >= ni->second.desc->inputs.size()) return false;
    if (ni->second.desc->inputs[input.index].propertyOnly) return false;
    Edge e{output, input};
    auto it = std::lower_bound(m_edges.begin(), m_edges.end(), e, ByTo{});
    if (it != m_edges.end() && it->to == input) *it = e;
    else m_edges.insert(it, e);
    ++m_rev;
    return true;
}

bool SandboxGraphModel::Disconnect(PinRef input)
{
    Edge key;
    key.to = input;
    auto it = std::lower_bound(m_edges.begin(), m_edges.end(), key, ByTo{});
    if (it == m_edges.end() || !(it->to == input)) return false;
    m_edges.erase(it);
    ++m_rev;
    return true;
}

void SandboxGraphModel::SetNodePos(NodeId id, Vec2 pos)
{
    auto it = m_nodes.find(id);
    if (it == m_nodes.end() || it->second.pos == pos) return;
    it->second.pos = pos;
    ++m_rev;
}

void SandboxGraphModel::SetPinValue(PinRef input, const PinValue& v)
{
    auto it = m_nodes.find(input.node);
    if (it == m_nodes.end() || input.output || input.index >= it->second.values.size()) return;
    PinValue nv = v;
    nv.kind = it->second.values[input.index].kind;
    if (it->second.values[input.index] == nv) return;
    it->second.values[input.index] = nv;
    ++m_rev;
}

void SandboxGraphModel::Clear()
{
    m_nodes.clear();
    m_ids.clear();
    m_edges.clear();
    m_nextId = 1;
    ++m_rev;
}

} // namespace dx12e::ng
