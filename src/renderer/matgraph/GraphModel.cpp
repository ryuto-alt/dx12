#include "renderer/matgraph/GraphModel.h"

#include "renderer/matgraph/GraphAnalysis.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace dx12e::matgraph
{

namespace
{
bool IsValidId(const std::string& id)
{
    if (id.empty()) return false;
    for (unsigned char c : id)
        if (!(std::isalnum(c) || c == '_')) return false;
    return true;
}
} // namespace

MaterialGraph::MaterialGraph(const NodeLibrary* lib) : m_lib(lib ? lib : &NodeLibrary::Builtin())
{
    std::random_device rd;
    m_rng.seed(rd());
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(m_rng()));
    m_guid = buf;
}

MaterialGraph::~MaterialGraph() = default;
MaterialGraph::MaterialGraph(MaterialGraph&&) noexcept = default;
MaterialGraph& MaterialGraph::operator=(MaterialGraph&&) noexcept = default;

MaterialGraph MaterialGraph::Clone() const
{
    MaterialGraph g(m_lib);
    g.m_nodes = m_nodes;
    g.m_comments = m_comments;
    g.m_settings = m_settings;
    g.m_view = m_view;
    g.m_guid = m_guid;
    g.m_name = m_name;
    g.m_version = m_version;
    g.m_structVersion = m_structVersion;
    g.m_rng = m_rng;
    return g;
}

void MaterialGraph::Notify(const GraphChange& c)
{
    // リスナーが登録 / 解除しても壊れないようにコピーして回す
    std::vector<Listener> fns;
    fns.reserve(m_listeners.size());
    for (const auto& kv : m_listeners) fns.push_back(kv.second);
    for (const Listener& fn : fns) fn(c);
}

void MaterialGraph::Bump(bool structure)
{
    ++m_version;
    if (structure) ++m_structVersion;
}

MaterialGraph::ListenerId MaterialGraph::AddListener(Listener fn)
{
    const ListenerId id = m_nextListener++;
    m_listeners[id] = std::move(fn);
    return id;
}

void MaterialGraph::RemoveListener(ListenerId id) { m_listeners.erase(id); }

void MaterialGraph::SetGuid(const std::string& g)
{
    if (g == m_guid) return;
    m_guid = g;
    Bump(false);
    Notify({ChangeKind::MetaChanged, {}, {}, false});
}

void MaterialGraph::SetName(const std::string& n)
{
    if (n == m_name) return;
    m_name = n;
    Bump(false);
    Notify({ChangeKind::MetaChanged, {}, {}, false});
}

void MaterialGraph::SetSettings(const GraphSettings& s)
{
    if (s == m_settings) return;
    m_settings = s;
    Bump(true);
    Notify({ChangeKind::SettingsChanged, {}, {}, true});
}

const Node* MaterialGraph::FindNode(const NodeId& id) const
{
    auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : &it->second;
}

const NodeDef* MaterialGraph::DefOf(const NodeId& id) const
{
    const Node* n = FindNode(id);
    return n ? m_lib->Find(n->type) : nullptr;
}

NodeId MaterialGraph::GenerateNodeId()
{
    for (;;)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "n_%06x", static_cast<unsigned>(m_rng() & 0xFFFFFFu));
        if (!m_nodes.count(buf)) return buf;
    }
}

void MaterialGraph::SetIdSeed(uint32_t seed) { m_rng.seed(seed); }

NodeId MaterialGraph::AddNode(const std::string& type, float x, float y, const NodeId& idIn)
{
    NodeId id = idIn;
    if (id.empty())
        id = GenerateNodeId();
    else if (!IsValidId(id) || m_nodes.count(id))
        return {};
    Node n;
    n.id = id; n.type = type; n.x = x; n.y = y;
    if (const NodeDef* def = m_lib->Find(type))
        for (const PropDecl& pd : def->props)
        {
            Json v;
            if (NormalizeProp(pd, pd.defaultValue, v)) n.props[pd.name] = std::move(v);
        }
    m_nodes[id] = std::move(n);
    Bump(true);
    Notify({ChangeKind::NodeAdded, id, {}, true});
    return id;
}

bool MaterialGraph::RemoveNode(const NodeId& id, RemovedNode* undoInfo)
{
    auto it = m_nodes.find(id);
    if (it == m_nodes.end()) return false;
    RemovedNode r;
    r.node = it->second;
    r.outgoing = EdgesFrom(id);
    m_nodes.erase(it);
    // 他ノードの入力からこのノードへの接続を外す
    for (const Edge& e : r.outgoing)
    {
        auto tn = m_nodes.find(e.toNode);
        if (tn == m_nodes.end()) continue;
        auto b = tn->second.inputs.find(e.toPin);
        if (b != tn->second.inputs.end() && b->second.link && b->second.link->node == id)
        {
            tn->second.inputs.erase(b);
            Bump(true);
            Notify({ChangeKind::LinkChanged, e.toNode, e.toPin, true});
        }
    }
    Bump(true);
    Notify({ChangeKind::NodeRemoved, id, {}, true});
    if (undoInfo) *undoInfo = std::move(r);
    return true;
}

bool MaterialGraph::RestoreNode(const RemovedNode& r)
{
    if (m_nodes.count(r.node.id)) return false;
    m_nodes[r.node.id] = r.node;
    Bump(true);
    Notify({ChangeKind::NodeAdded, r.node.id, {}, true});
    for (const Edge& e : r.outgoing)
    {
        auto tn = m_nodes.find(e.toNode);
        if (tn == m_nodes.end()) continue;
        InputBinding& b = tn->second.inputs[e.toPin];
        b.link = PinRef{e.fromNode, e.fromPin};
        b.literal.reset();
        Bump(true);
        Notify({ChangeKind::LinkChanged, e.toNode, e.toPin, true});
    }
    return true;
}

bool MaterialGraph::MoveNode(const NodeId& id, float x, float y)
{
    auto it = m_nodes.find(id);
    if (it == m_nodes.end()) return false;
    if (it->second.x == x && it->second.y == y) return true;
    it->second.x = x; it->second.y = y;
    Bump(false);
    Notify({ChangeKind::NodeMoved, id, {}, false});
    return true;
}

bool MaterialGraph::SetNodeProp(const NodeId& id, const std::string& key, const Json& value)
{
    auto it = m_nodes.find(id);
    if (it == m_nodes.end()) return false;
    Node& n = it->second;
    const NodeDef* def = m_lib->Find(n.type);
    const PropDecl* pd = def ? def->FindProp(key) : nullptr;
    Json norm;
    if (pd)
    {
        if (!NormalizeProp(*pd, value, norm)) return false;
    }
    else
        norm = value;
    auto old = n.props.find(key);
    if (old != n.props.end() && old->second == norm) return true;
    n.props[key] = norm;

    bool structure = true;
    if (pd)
    {
        if (pd->role == PropRole::Meta) structure = false;
        else if (pd->role == PropRole::SlotValue)
        {
            const PropView pv(*def, n.props);
            structure = def->FindProp("inline") != nullptr && pv.Bool("inline");
        }
    }
    Bump(structure);
    Notify({ChangeKind::PropChanged, id, key, structure});
    return true;
}

Json MaterialGraph::GetNodeProp(const NodeId& id, const std::string& key) const
{
    const Node* n = FindNode(id);
    if (!n) return Json();
    auto it = n->props.find(key);
    if (it != n->props.end()) return it->second;
    if (const NodeDef* def = m_lib->Find(n->type))
        if (const PropDecl* pd = def->FindProp(key)) return pd->defaultValue;
    return Json();
}

// from の上流に to がいる（= from → to を繋ぐと循環になる）か
bool MaterialGraph::WouldCycle(const NodeId& fromNode, const NodeId& toNode) const
{
    if (fromNode == toNode) return true;
    std::set<NodeId> seen;
    std::vector<NodeId> stack{fromNode};
    while (!stack.empty())
    {
        const NodeId cur = stack.back();
        stack.pop_back();
        if (!seen.insert(cur).second) continue;
        const Node* n = FindNode(cur);
        if (!n) continue;
        for (const auto& kv : n->inputs)
        {
            if (!kv.second.link) continue;
            if (kv.second.link->node == toNode) return true;
            stack.push_back(kv.second.link->node);
        }
    }
    return false;
}

ConnectCheck MaterialGraph::CanConnect(const NodeId& fromNode, const std::string& fromPin,
                                       const NodeId& toNode, const std::string& toPin) const
{
    auto reject = [](const char* c, std::string msg) {
        ConnectCheck r; r.verdict = ConnectCheck::Verdict::Reject; r.code = c; r.message = std::move(msg); return r;
    };
    const Node* fn = FindNode(fromNode);
    const Node* tn = FindNode(toNode);
    if (!fn || !tn) return reject(code::kDanglingLink, "接続元または接続先のノードが存在しません");
    const NodeDef* fd = m_lib->Find(fn->type);
    const NodeDef* td = m_lib->Find(tn->type);
    if (!fd || !td) return reject(code::kUnknownNode, "未知のノード型には接続できません");
    const int oi = fd->OutputIndex(fromPin);
    if (oi < 0) return reject(code::kUnknownPin, fd->type + " に出力ピン \"" + fromPin + "\" がありません");
    const PinDecl* pin = td->FindInput(toPin);
    if (!pin) return reject(code::kUnknownPin, td->type + " に入力ピン \"" + toPin + "\" がありません");
    if (pin->reserved) return reject(code::kReservedPin, td->type + "." + toPin + " は予約ピンです（現在は接続できません）");
    if (WouldCycle(fromNode, toNode)) return reject(code::kCycle, "循環になるため接続できません");

    const GraphAnalysis& an = Analysis();
    const NodeTypeInfo* fi = an.Find(fromNode);
    ValueType src = (fi && static_cast<size_t>(oi) < fi->outTypes.size()) ? fi->outTypes[static_cast<size_t>(oi)] : ValueType::Invalid;
    ConnectCheck ok;
    if (src == ValueType::Invalid) return ok;   // 上流にエラーがあり型を判定できない → 許可（エラーは上流に出ている）

    const std::string where = td->type + "." + toPin;
    if (pin->mode == PinMode::Fixed)
    {
        const CastResult cr = CheckCast(src, pin->type);
        if (!cr.ok)
            return reject(IsFloatVec(src) && IsFloatVec(pin->type) ? code::kExpandForbidden : code::kTypeMismatch, where + " — " + cr.reason);
        if (cr.warn)
        {
            ok.verdict = ConnectCheck::Verdict::Warn; ok.code = code::kTruncate; ok.message = where + " — " + cr.reason;
        }
        return ok;
    }
    if (pin->mode == PinMode::Free)
    {
        if (IsNumeric(src) || (src == ValueType::Tex2D && pin->allowTexture)) return ok;
        const CastResult cr = CheckCast(src, ValueType::F1);
        return reject(code::kTypeMismatch, where + " — " + (cr.reason.empty() ? "この型は受け取れません" : cr.reason));
    }
    // Poly: 数値であること + 同じグループの次元と揃うこと（試しに繋いで診断を見る）
    if (!IsNumeric(src))
    {
        const CastResult cr = CheckCast(src, ValueType::F1);
        return reject(code::kTypeMismatch, where + " — " + (cr.reason.empty() ? "数値が必要です" : cr.reason));
    }
    MaterialGraph trial = Clone();
    InputBinding& b = trial.m_nodes[toNode].inputs[toPin];
    b.link = PinRef{fromNode, fromPin};
    b.literal.reset();
    trial.m_version++;
    const GraphAnalysis after = AnalyzeGraph(trial);
    std::set<std::string> before;
    for (const Diagnostic& d : an.diags)
        if (d.nodeId == toNode && d.code == code::kDimMismatch) before.insert(d.message);
    for (const Diagnostic& d : after.diags)
        if (d.nodeId == toNode && d.severity == Severity::Error && d.code == code::kDimMismatch && !before.count(d.message))
            return reject(d.code.c_str(), d.message);
    return ok;
}

bool MaterialGraph::Connect(const NodeId& fromNode, const std::string& fromPin,
                            const NodeId& toNode, const std::string& toPin, ConnectCheck* why)
{
    ConnectCheck c = CanConnect(fromNode, fromPin, toNode, toPin);
    if (why) *why = c;
    if (!c.ok()) return false;
    Node& tn = m_nodes[toNode];
    InputBinding& b = tn.inputs[toPin];
    const PinRef nr{fromNode, fromPin};
    if (b.link && *b.link == nr && !b.literal) return true;
    b.link = nr;
    b.literal.reset();
    Bump(true);
    Notify({ChangeKind::LinkChanged, toNode, toPin, true});
    return true;
}

bool MaterialGraph::ConnectUnchecked(const NodeId& fromNode, const std::string& fromPin,
                                     const NodeId& toNode, const std::string& toPin)
{
    const Node* fn = FindNode(fromNode);
    auto tit = m_nodes.find(toNode);
    if (!fn || tit == m_nodes.end()) return false;
    // 型が登録表にあるときだけピンの存在と予約を確かめる（未知の型はファイル往復のため素通し）
    if (const NodeDef* fd = m_lib->Find(fn->type))
        if (fd->OutputIndex(fromPin) < 0) return false;
    if (const NodeDef* td = m_lib->Find(tit->second.type))
    {
        const PinDecl* pin = td->FindInput(toPin);
        if (!pin || pin->reserved) return false;
    }
    InputBinding& b = tit->second.inputs[toPin];
    const PinRef nr{fromNode, fromPin};
    if (b.link && *b.link == nr && !b.literal) return true;
    b.link = nr;
    b.literal.reset();
    Bump(true);
    Notify({ChangeKind::LinkChanged, toNode, toPin, true});
    return true;
}

bool MaterialGraph::Disconnect(const NodeId& toNode, const std::string& toPin)
{
    auto it = m_nodes.find(toNode);
    if (it == m_nodes.end()) return false;
    auto b = it->second.inputs.find(toPin);
    if (b == it->second.inputs.end()) return false;
    const bool hadLink = b->second.link.has_value();
    it->second.inputs.erase(b);
    Bump(true);
    Notify({hadLink ? ChangeKind::LinkChanged : ChangeKind::LiteralChanged, toNode, toPin, true});
    return true;
}

bool MaterialGraph::SetLiteral(const NodeId& toNode, const std::string& toPin, const Value& vIn)
{
    auto it = m_nodes.find(toNode);
    if (it == m_nodes.end()) return false;
    const NodeDef* def = m_lib->Find(it->second.type);
    const PinDecl* pin = def ? def->FindInput(toPin) : nullptr;
    if (!pin || pin->reserved) return false;
    Value v = vIn;
    if (pin->mode == PinMode::Fixed)
    {
        if (pin->type == ValueType::Int)
        {
            if (v.type == ValueType::F1 && v.f[0] == std::floor(v.f[0])) v = Value::Int32(static_cast<int32_t>(v.f[0]));
            if (v.type != ValueType::Int) return false;
        }
        else if (pin->type == ValueType::Bool)
        {
            if (v.type != ValueType::Bool) return false;
        }
        else if (pin->type == ValueType::Tex2D || !IsFloatVec(pin->type))
            return false;
        else if (!CheckCast(v.type, pin->type).ok)
            return false;
    }
    else if (!IsNumeric(v.type))
        return false;
    InputBinding& b = it->second.inputs[toPin];
    if (b.literal && !b.link && *b.literal == v) return true;
    b.literal = v;
    b.link.reset();
    Bump(true);
    Notify({ChangeKind::LiteralChanged, toNode, toPin, true});
    return true;
}

bool MaterialGraph::ClearLiteral(const NodeId& toNode, const std::string& toPin)
{
    auto it = m_nodes.find(toNode);
    if (it == m_nodes.end()) return false;
    auto b = it->second.inputs.find(toPin);
    if (b == it->second.inputs.end() || !b->second.literal) return false;
    if (b->second.link) { b->second.literal.reset(); }
    else it->second.inputs.erase(b);
    Bump(true);
    Notify({ChangeKind::LiteralChanged, toNode, toPin, true});
    return true;
}

const InputBinding* MaterialGraph::Binding(const NodeId& toNode, const std::string& toPin) const
{
    const Node* n = FindNode(toNode);
    if (!n) return nullptr;
    auto it = n->inputs.find(toPin);
    return it == n->inputs.end() ? nullptr : &it->second;
}

std::vector<Edge> MaterialGraph::Edges() const
{
    std::vector<Edge> v;
    for (const auto& nk : m_nodes)
        for (const auto& ik : nk.second.inputs)
            if (ik.second.link) v.push_back({ik.second.link->node, ik.second.link->pin, nk.first, ik.first});
    return v;
}

std::vector<Edge> MaterialGraph::EdgesFrom(const NodeId& fromNode) const
{
    std::vector<Edge> v;
    for (const Edge& e : Edges())
        if (e.fromNode == fromNode) v.push_back(e);
    return v;
}

std::vector<Edge> MaterialGraph::EdgesTo(const NodeId& toNode) const
{
    std::vector<Edge> v;
    const Node* n = FindNode(toNode);
    if (!n) return v;
    for (const auto& ik : n->inputs)
        if (ik.second.link) v.push_back({ik.second.link->node, ik.second.link->pin, toNode, ik.first});
    return v;
}

const GraphAnalysis& MaterialGraph::Analysis() const
{
    if (!m_analysis || m_analysisVersion != m_version)
    {
        m_analysis = std::make_shared<const GraphAnalysis>(AnalyzeGraph(*this));
        m_analysisVersion = m_version;
    }
    return *m_analysis;
}

PinInfo MaterialGraph::QueryPin(const NodeId& node, const std::string& pin, bool isOutput) const
{
    PinInfo r;
    r.name = pin; r.isOutput = isOutput;
    const Node* n = FindNode(node);
    const NodeDef* def = n ? m_lib->Find(n->type) : nullptr;
    if (!def) return r;
    const NodeTypeInfo* ti = Analysis().Find(node);
    if (isOutput)
    {
        const int oi = def->OutputIndex(pin);
        if (oi < 0) return r;
        r.exists = true;
        r.outDecl = &def->outputs[static_cast<size_t>(oi)];
        if (ti && static_cast<size_t>(oi) < ti->outTypes.size()) r.type = ti->outTypes[static_cast<size_t>(oi)];
        for (const Edge& e : Edges())
            if (e.fromNode == node && e.fromPin == pin) { r.connected = true; break; }
        return r;
    }
    const int ii = def->InputIndex(pin);
    if (ii < 0) return r;
    r.exists = true;
    r.inDecl = &def->inputs[static_cast<size_t>(ii)];
    r.reserved = r.inDecl->reserved;
    if (ti && static_cast<size_t>(ii) < ti->inTypes.size()) r.type = ti->inTypes[static_cast<size_t>(ii)];
    if (const InputBinding* b = Binding(node, pin))
    {
        r.connected = b->link.has_value();
        r.hasLiteral = b->literal.has_value();
    }
    return r;
}

std::string MaterialGraph::AddComment(const Comment& cIn)
{
    Comment c = cIn;
    if (c.id.empty())
        for (;;)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "c_%04x", static_cast<unsigned>(m_rng() & 0xFFFFu));
            if (!m_comments.count(buf)) { c.id = buf; break; }
        }
    else if (!IsValidId(c.id) || m_comments.count(c.id))
        return {};
    const std::string id = c.id;
    m_comments[id] = std::move(c);
    Bump(false);
    Notify({ChangeKind::CommentChanged, {}, id, false});
    return id;
}

bool MaterialGraph::SetComment(const Comment& c)
{
    auto it = m_comments.find(c.id);
    if (it == m_comments.end()) return false;
    it->second = c;
    Bump(false);
    Notify({ChangeKind::CommentChanged, {}, c.id, false});
    return true;
}

bool MaterialGraph::RemoveComment(const std::string& id)
{
    if (!m_comments.erase(id)) return false;
    Bump(false);
    Notify({ChangeKind::CommentChanged, {}, id, false});
    return true;
}

void MaterialGraph::LoadNode(Node n)
{
    const NodeId id = n.id;
    m_nodes[id] = std::move(n);
}

void MaterialGraph::LoadComment(Comment c)
{
    const std::string id = c.id;
    m_comments[id] = std::move(c);
}

void MaterialGraph::LoadFinish()
{
    Bump(true);
    Notify({ChangeKind::Reset, {}, {}, true});
}

void MaterialGraph::Clear()
{
    m_nodes.clear();
    m_comments.clear();
    m_settings = GraphSettings{};
    m_view = ViewInfo{};
    Bump(true);
    Notify({ChangeKind::Reset, {}, {}, true});
}

} // namespace dx12e::matgraph
