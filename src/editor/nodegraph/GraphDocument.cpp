#include "editor/nodegraph/GraphDocument.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dx12e::ng
{

// ===========================================================================
// GraphHistory
// ===========================================================================
void GraphHistory::Push(std::unique_ptr<IGraphCommand> cmd)
{
    if (!cmd) return;
    ++m_editSeq;
    if (!m_groups.empty()) { m_groups.back().cmds.push_back(std::move(cmd)); return; }
    m_redo.clear();
    m_undo.push_back(std::move(cmd));
    if (m_maxDepth > 0 && m_undo.size() > m_maxDepth) m_undo.erase(m_undo.begin());
}

void GraphHistory::Undo(GraphDocument& doc)
{
    if (m_undo.empty()) return;
    auto cmd = std::move(m_undo.back());
    m_undo.pop_back();
    cmd->Undo(doc);
    m_redo.push_back(std::move(cmd));
    ++m_editSeq;
}

void GraphHistory::Redo(GraphDocument& doc)
{
    if (m_redo.empty()) return;
    auto cmd = std::move(m_redo.back());
    m_redo.pop_back();
    cmd->Redo(doc);
    m_undo.push_back(std::move(cmd));
    ++m_editSeq;
}

namespace
{
class CmdComposite final : public IGraphCommand
{
public:
    explicit CmdComposite(const char* name) : m_name(name ? name : "") {}
    std::vector<std::unique_ptr<IGraphCommand>> cmds;
    void Undo(GraphDocument& d) override { for (auto it = cmds.rbegin(); it != cmds.rend(); ++it) (*it)->Undo(d); }
    void Redo(GraphDocument& d) override { for (auto& c : cmds) c->Redo(d); }
    const char* GetName() const override { return m_name.c_str(); }
private:
    std::string m_name;
};
} // namespace

void GraphHistory::BeginGroup(const char* name)
{
    m_groups.push_back(Group{name, {}});
}

void GraphHistory::EndGroup()
{
    if (m_groups.empty()) return;
    Group g = std::move(m_groups.back());
    m_groups.pop_back();
    if (g.cmds.empty()) return;
    std::unique_ptr<IGraphCommand> out;
    if (g.cmds.size() == 1) out = std::move(g.cmds[0]);
    else
    {
        auto comp = std::make_unique<CmdComposite>(g.name);
        comp->cmds = std::move(g.cmds);
        out = std::move(comp);
    }
    // Push は editSeq を進める（中身を積んだ時点で 1 度進んでいるので、ここでは戻す）
    const uint64_t seq = m_editSeq;
    Push(std::move(out));
    m_editSeq = seq;
}

// ===========================================================================
// 整列
// ===========================================================================
std::vector<std::pair<NodeId, Vec2>> ComputeAlign(AlignMode mode, const std::vector<AlignItem>& items)
{
    std::vector<std::pair<NodeId, Vec2>> out;
    out.reserve(items.size());
    for (const AlignItem& it : items) out.emplace_back(it.id, it.rect.min);
    if (items.size() < 2) return out;

    Rect b = EmptyRect();
    for (const AlignItem& it : items) b.Add(it.rect);

    switch (mode)
    {
    case AlignMode::Left:    for (size_t i = 0; i < items.size(); ++i) out[i].second.x = b.min.x; break;
    case AlignMode::Right:   for (size_t i = 0; i < items.size(); ++i) out[i].second.x = b.max.x - items[i].rect.Width(); break;
    case AlignMode::Top:     for (size_t i = 0; i < items.size(); ++i) out[i].second.y = b.min.y; break;
    case AlignMode::Bottom:  for (size_t i = 0; i < items.size(); ++i) out[i].second.y = b.max.y - items[i].rect.Height(); break;
    case AlignMode::CenterH: for (size_t i = 0; i < items.size(); ++i) out[i].second.x = b.Center().x - items[i].rect.Width() * 0.5f; break;
    case AlignMode::CenterV: for (size_t i = 0; i < items.size(); ++i) out[i].second.y = b.Center().y - items[i].rect.Height() * 0.5f; break;
    case AlignMode::DistributeH:
    case AlignMode::DistributeV:
    {
        if (items.size() < 3) break;
        const bool h = (mode == AlignMode::DistributeH);
        std::vector<size_t> order(items.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t c)
        {
            const float ka = h ? items[a].rect.min.x : items[a].rect.min.y;
            const float kc = h ? items[c].rect.min.x : items[c].rect.min.y;
            if (ka != kc) return ka < kc;
            return items[a].id < items[c].id;
        });
        float sum = 0.0f;
        for (const AlignItem& it : items) sum += h ? it.rect.Width() : it.rect.Height();
        const float span = h ? (b.max.x - b.min.x) : (b.max.y - b.min.y);
        const float gap = (span - sum) / static_cast<float>(items.size() - 1);
        float cur = h ? b.min.x : b.min.y;
        for (size_t k = 0; k < order.size(); ++k)
        {
            const size_t i = order[k];
            if (h) out[i].second.x = cur; else out[i].second.y = cur;
            cur += (h ? items[i].rect.Width() : items[i].rect.Height()) + gap;
        }
        break;
    }
    }
    return out;
}

// ===========================================================================
// コマンド
// ===========================================================================
namespace
{
using CmdPtr = std::unique_ptr<IGraphCommand>;

NodeData SnapNode(const IGraphModel& m, NodeId id)
{
    const NodeData* n = m.FindNode(id);
    NodeData d;
    if (n) d = *n;
    return d;
}

class CmdAddNode final : public IGraphCommand
{
public:
    explicit CmdAddNode(NodeData d) : m_data(std::move(d)) {}
    void Undo(GraphDocument& doc) override { doc.Model().RemoveNode(m_data.id); }
    void Redo(GraphDocument& doc) override { doc.Model().AddNode(m_data); }
    const char* GetName() const override { return "ノードを追加"; }
private:
    NodeData m_data;
};

class CmdRemoveItems final : public IGraphCommand
{
public:
    std::vector<NodeData> nodes;    // ID 昇順
    std::vector<Edge>     edges;
    std::vector<Comment>  comments;
    void Undo(GraphDocument& doc) override
    {
        for (const NodeData& n : nodes) doc.Model().AddNode(n);
        for (const Edge& e : edges) doc.Model().ConnectUnchecked(e.from, e.to);
        for (const Comment& c : comments) doc.RawAddComment(c);
    }
    void Redo(GraphDocument& doc) override
    {
        for (const NodeData& n : nodes) doc.Model().RemoveNode(n.id);
        for (const Comment& c : comments) doc.RawRemoveComment(c.id);
    }
    const char* GetName() const override { return "削除"; }
};

class CmdConnect final : public IGraphCommand
{
public:
    CmdConnect(Edge e, bool had, Edge replaced) : m_e(e), m_had(had), m_replaced(replaced) {}
    void Undo(GraphDocument& doc) override
    {
        doc.Model().Disconnect(m_e.to);
        if (m_had) doc.Model().ConnectUnchecked(m_replaced.from, m_replaced.to);
    }
    void Redo(GraphDocument& doc) override { doc.Model().ConnectUnchecked(m_e.from, m_e.to); }
    const char* GetName() const override { return "ワイヤを接続"; }
private:
    Edge m_e; bool m_had; Edge m_replaced;
};

class CmdDisconnect final : public IGraphCommand
{
public:
    explicit CmdDisconnect(Edge e) : m_e(e) {}
    void Undo(GraphDocument& doc) override { doc.Model().ConnectUnchecked(m_e.from, m_e.to); }
    void Redo(GraphDocument& doc) override { doc.Model().Disconnect(m_e.to); }
    const char* GetName() const override { return "ワイヤを切断"; }
private:
    Edge m_e;
};

class CmdSetValue final : public IGraphCommand
{
public:
    CmdSetValue(PinRef p, PinValue o, PinValue n) : m_pin(p), m_old(o), m_new(n) {}
    void Undo(GraphDocument& doc) override { doc.Model().SetPinValue(m_pin, m_old); }
    void Redo(GraphDocument& doc) override { doc.Model().SetPinValue(m_pin, m_new); }
    const char* GetName() const override { return "値を変更"; }
private:
    PinRef m_pin; PinValue m_old, m_new;
};

class CmdMove final : public IGraphCommand
{
public:
    struct N { NodeId id; Vec2 from, to; };
    struct C { CommentId id; Rect from, to; };
    std::vector<N> nodes;
    std::vector<C> comments;
    explicit CmdMove(const char* name) : m_name(name) {}
    void Undo(GraphDocument& doc) override { Apply(doc, false); }
    void Redo(GraphDocument& doc) override { Apply(doc, true); }
    const char* GetName() const override { return m_name; }
private:
    void Apply(GraphDocument& doc, bool forward)
    {
        for (const N& n : nodes) doc.Model().SetNodePos(n.id, forward ? n.to : n.from);
        for (const C& c : comments)
        {
            const Comment* cur = doc.FindComment(c.id);
            if (!cur) continue;
            Comment next = *cur;
            next.rect = forward ? c.to : c.from;
            doc.RawSetComment(next);
        }
    }
    const char* m_name;
};

class CmdAddComment final : public IGraphCommand
{
public:
    explicit CmdAddComment(Comment c) : m_c(std::move(c)) {}
    void Undo(GraphDocument& doc) override { doc.RawRemoveComment(m_c.id); }
    void Redo(GraphDocument& doc) override { doc.RawAddComment(m_c); }
    const char* GetName() const override { return "コメントを追加"; }
private:
    Comment m_c;
};

class CmdSetComment final : public IGraphCommand
{
public:
    CmdSetComment(Comment o, Comment n) : m_old(std::move(o)), m_new(std::move(n)) {}
    void Undo(GraphDocument& doc) override { doc.RawSetComment(m_old); }
    void Redo(GraphDocument& doc) override { doc.RawSetComment(m_new); }
    const char* GetName() const override { return "コメントを編集"; }
private:
    Comment m_old, m_new;
};

// 浮動小数のビット表現（0 は "0" 1 文字。それ以外は 8 桁の 16 進）。
void FloatBits(std::string& s, float v)
{
    uint32_t b;
    std::memcpy(&b, &v, sizeof(b));
    if (b == 0) { s += '0'; return; }
    static const char* kHex = "0123456789abcdef";
    char buf[8];
    for (int i = 0; i < 8; ++i) buf[7 - i] = kHex[(b >> (4 * i)) & 0xF];
    s.append(buf, 8);
}

void AppendU(std::string& s, uint32_t v)
{
    char buf[12];
    int n = 0;
    do { buf[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    while (n) s += buf[--n];
}
} // namespace

// ===========================================================================
// GraphDocument
// ===========================================================================
GraphDocument::GraphDocument(IGraphModel* model)
    : m_model(model), m_layout(nullptr), m_nodeGrid(256.0f), m_edgeGrid(256.0f), m_commentGrid(256.0f)
{
}

Rect GraphDocument::NodeRect(NodeId id) const
{
    const NodeData* n = m_model->FindNode(id);
    if (!n || !n->desc) return Rect({0, 0}, {-1, -1});
    return m_layout.NodeRect(*n);
}

Vec2 GraphDocument::PinPos(PinRef pin) const
{
    const NodeData* n = m_model->FindNode(pin.node);
    if (!n || !n->desc) return {};
    return m_layout.PinPos(*n, pin);
}

Rect GraphDocument::SlotRect(PinRef pin) const
{
    const NodeData* n = m_model->FindNode(pin.node);
    if (!n || !n->desc || pin.output) return Rect();
    return m_layout.SlotRect(*n, pin.index);
}

Rect GraphDocument::ContentBounds() const
{
    Rect b = EmptyRect();
    for (NodeId id : m_model->Nodes())
    {
        const Rect r = NodeRect(id);
        if (r.Width() >= 0.0f) b.Add(r);
    }
    for (const Comment& c : m_comments) b.Add(c.rect);
    return b;
}

Rect GraphDocument::BoundsOf(const Selection& s) const
{
    Rect b = EmptyRect();
    for (NodeId id : s.nodes)
    {
        const Rect r = NodeRect(id);
        if (r.Width() >= 0.0f) b.Add(r);
    }
    for (CommentId id : s.comments)
        if (const Comment* c = FindComment(id)) b.Add(c->rect);
    return b;
}

const Comment* GraphDocument::FindComment(CommentId id) const
{
    auto it = std::lower_bound(m_comments.begin(), m_comments.end(), id, [](const Comment& c, CommentId v) { return c.id < v; });
    return (it != m_comments.end() && it->id == id) ? &*it : nullptr;
}

std::vector<NodeId> GraphDocument::NodesInsideComment(CommentId id) const
{
    std::vector<NodeId> out;
    const Comment* c = FindComment(id);
    if (!c) return out;
    for (NodeId n : m_model->Nodes())
    {
        const Rect r = NodeRect(n);
        if (r.Width() >= 0.0f && c->rect.Contains(r)) out.push_back(n);
    }
    return out;
}

// ---- コメントの生の操作（ID 昇順を保つ）----
void GraphDocument::RawAddComment(const Comment& c)
{
    auto it = std::lower_bound(m_comments.begin(), m_comments.end(), c.id, [](const Comment& a, CommentId v) { return a.id < v; });
    if (it != m_comments.end() && it->id == c.id) *it = c;
    else m_comments.insert(it, c);
    SetNextCommentIdAtLeast(c.id);
    ++m_commentRev;
}

void GraphDocument::RawRemoveComment(CommentId id)
{
    auto it = std::lower_bound(m_comments.begin(), m_comments.end(), id, [](const Comment& a, CommentId v) { return a.id < v; });
    if (it != m_comments.end() && it->id == id) { m_comments.erase(it); ++m_commentRev; }
}

void GraphDocument::RawSetComment(const Comment& c)
{
    auto it = std::lower_bound(m_comments.begin(), m_comments.end(), c.id, [](const Comment& a, CommentId v) { return a.id < v; });
    if (it != m_comments.end() && it->id == c.id) { *it = c; ++m_commentRev; }
}

// ---- 編集 ----
NodeId GraphDocument::AddNode(const std::string& type, Vec2 pos)
{
    NodeData d;
    d.type = type;
    d.pos = pos;
    return AddNodeData(std::move(d));
}

NodeId GraphDocument::AddNodeData(NodeData d)
{
    d.id = 0;
    const NodeId id = m_model->AddNode(d);
    if (!id) return 0;
    m_history.Push(std::make_unique<CmdAddNode>(SnapNode(*m_model, id)));
    return id;
}

bool GraphDocument::Connect(PinRef out, PinRef in)
{
    const ConnectCheck chk = m_model->CanConnect(out, in);
    if (!chk.ok) return false;
    const Edge* ex = m_model->FindEdgeTo(in);
    bool had = false;
    Edge replaced;
    if (ex)
    {
        if (ex->from == out) return true;   // すでに同じ接続（何も変わらない）
        had = true;
        replaced = *ex;
    }
    if (!m_model->Connect(out, in)) return false;
    m_history.Push(std::make_unique<CmdConnect>(Edge{out, in}, had, replaced));
    return true;
}

bool GraphDocument::Disconnect(PinRef in)
{
    const Edge* ex = m_model->FindEdgeTo(in);
    if (!ex) return false;
    const Edge e = *ex;
    if (!m_model->Disconnect(in)) return false;
    m_history.Push(std::make_unique<CmdDisconnect>(e));
    return true;
}

int GraphDocument::DisconnectPin(PinRef pin)
{
    if (!pin.output) return Disconnect(pin) ? 1 : 0;
    std::vector<PinRef> ins;
    for (const Edge& e : m_model->Edges())
        if (e.from == pin) ins.push_back(e.to);
    if (ins.empty()) return 0;
    m_history.BeginGroup("ワイヤを切断");
    for (const PinRef& in : ins) Disconnect(in);
    m_history.EndGroup();
    return static_cast<int>(ins.size());
}

void GraphDocument::RemoveItems(const Selection& s)
{
    auto cmd = std::make_unique<CmdRemoveItems>();
    std::set<NodeId> ids;
    for (NodeId id : s.nodes)
        if (m_model->FindNode(id)) ids.insert(id);
    for (NodeId id : ids) cmd->nodes.push_back(SnapNode(*m_model, id));
    for (const Edge& e : m_model->Edges())
        if (ids.count(e.from.node) || ids.count(e.to.node)) cmd->edges.push_back(e);
    for (CommentId cid : s.comments)
        if (const Comment* c = FindComment(cid)) cmd->comments.push_back(*c);
    std::sort(cmd->comments.begin(), cmd->comments.end(), [](const Comment& a, const Comment& b) { return a.id < b.id; });
    if (cmd->nodes.empty() && cmd->comments.empty()) return;
    for (const NodeData& n : cmd->nodes) m_model->RemoveNode(n.id);
    for (const Comment& c : cmd->comments) RawRemoveComment(c.id);
    m_history.Push(std::move(cmd));
}

void GraphDocument::SetPinValue(PinRef in, const PinValue& v)
{
    const NodeData* n = m_model->FindNode(in.node);
    if (!n || in.output || in.index >= n->values.size()) return;
    const PinValue old = n->values[in.index];
    if (old == v) return;
    m_model->SetPinValue(in, v);
    m_history.Push(std::make_unique<CmdSetValue>(in, old, v));
}

bool GraphDocument::CommitPinValue(PinRef in, const PinValue& oldValue)
{
    const NodeData* n = m_model->FindNode(in.node);
    if (!n || in.output || in.index >= n->values.size()) return false;
    const PinValue cur = n->values[in.index];
    if (cur == oldValue) return false;
    m_history.Push(std::make_unique<CmdSetValue>(in, oldValue, cur));
    return true;
}

GraphDocument::MoveSession GraphDocument::BeginMove(const Selection& s) const
{
    MoveSession ms;
    ms.active = true;
    for (NodeId id : s.nodes)   // Selection は set なので重複しない
        if (const NodeData* n = m_model->FindNode(id)) ms.nodes.emplace_back(id, n->pos);
    for (CommentId id : s.comments)
        if (const Comment* c = FindComment(id)) ms.comments.emplace_back(id, c->rect);
    return ms;
}

void GraphDocument::UpdateMove(const MoveSession& ms, Vec2 delta)
{
    for (const auto& n : ms.nodes)
    {
        const NodeData* cur = m_model->FindNode(n.first);
        const Vec2 want = n.second + delta;
        if (cur && cur->pos != want) m_model->SetNodePos(n.first, want);
    }
    for (const auto& c : ms.comments)
    {
        const Comment* cur = FindComment(c.first);
        if (!cur) continue;
        const Rect want = c.second.Translated(delta);
        if (cur->rect != want) { Comment next = *cur; next.rect = want; RawSetComment(next); }
    }
}

bool GraphDocument::CommitMove(MoveSession& ms, const char* name)
{
    auto cmd = std::make_unique<CmdMove>(name);
    for (const auto& n : ms.nodes)
    {
        const NodeData* cur = m_model->FindNode(n.first);
        if (cur && cur->pos != n.second) cmd->nodes.push_back({n.first, n.second, cur->pos});
    }
    for (const auto& c : ms.comments)
    {
        const Comment* cur = FindComment(c.first);
        if (cur && cur->rect != c.second) cmd->comments.push_back({c.first, c.second, cur->rect});
    }
    ms.active = false;
    if (cmd->nodes.empty() && cmd->comments.empty()) return false;
    m_history.Push(std::move(cmd));
    return true;
}

void GraphDocument::CancelMove(MoveSession& ms)
{
    UpdateMove(ms, Vec2(0, 0));
    ms.active = false;
}

void GraphDocument::MoveItems(const Selection& s, Vec2 delta)
{
    MoveSession ms = BeginMove(s);
    UpdateMove(ms, delta);
    CommitMove(ms);
}

void GraphDocument::SetNodePositions(const std::vector<std::pair<NodeId, Vec2>>& pos, const char* name)
{
    auto cmd = std::make_unique<CmdMove>(name);
    std::set<NodeId> seen;   // 同じ ID が複数あっても、最初の位置を「元」にする（Undo で必ず元へ戻る）
    for (const auto& p : pos)
    {
        const NodeData* cur = m_model->FindNode(p.first);
        if (!cur || !seen.insert(p.first).second) continue;
        cmd->nodes.push_back({p.first, cur->pos, p.second});
    }
    for (auto it = cmd->nodes.begin(); it != cmd->nodes.end();)
    {
        if (it->from == it->to) it = cmd->nodes.erase(it); else ++it;
    }
    if (cmd->nodes.empty()) return;
    for (const auto& n : cmd->nodes) m_model->SetNodePos(n.id, n.to);
    m_history.Push(std::move(cmd));
}

void GraphDocument::Align(const std::vector<NodeId>& nodes, AlignMode mode)
{
    std::vector<AlignItem> items;
    std::set<NodeId> seen;
    for (NodeId id : nodes)
    {
        if (!seen.insert(id).second) continue;   // 重複 ID は 1 回だけ
        const Rect r = NodeRect(id);
        if (r.Width() >= 0.0f) items.push_back({id, r});
    }
    if (items.size() < 2) return;
    static const char* kNames[] = {"左揃え", "右揃え", "上揃え", "下揃え", "水平中央揃え", "垂直中央揃え", "水平に等間隔", "垂直に等間隔"};
    SetNodePositions(ComputeAlign(mode, items), kNames[static_cast<int>(mode)]);
}

CommentId GraphDocument::AddComment(const Rect& rect, const std::string& title, int color)
{
    Comment c;
    c.id = AllocCommentId();
    c.rect = rect;
    c.title = title;
    c.color = std::min(std::max(color, 0), kCommentColors - 1);
    RawAddComment(c);
    m_history.Push(std::make_unique<CmdAddComment>(c));
    return c.id;
}

CommentId GraphDocument::AddCommentAround(const std::vector<NodeId>& nodes, const std::string& title, int color)
{
    Rect b = EmptyRect();
    for (NodeId id : nodes)
    {
        const Rect r = NodeRect(id);
        if (r.Width() >= 0.0f) b.Add(r);
    }
    if (IsEmptyRect(b)) return 0;
    const float pad = 24.0f, titleH = 30.0f;
    return AddComment(Rect({b.min.x - pad, b.min.y - pad - titleH}, {b.max.x + pad, b.max.y + pad}), title, color);
}

void GraphDocument::EditComment(CommentId id, const Comment& next)
{
    const Comment* cur = FindComment(id);
    if (!cur) return;
    Comment n = next;
    n.id = id;
    if (*cur == n) return;
    const Comment old = *cur;
    RawSetComment(n);
    m_history.Push(std::make_unique<CmdSetComment>(old, n));
}

void GraphDocument::SetCommentLive(const Comment& c) { RawSetComment(c); }

bool GraphDocument::CommitComment(const Comment& oldValue)
{
    const Comment* cur = FindComment(oldValue.id);
    if (!cur || *cur == oldValue) return false;
    m_history.Push(std::make_unique<CmdSetComment>(oldValue, *cur));
    return true;
}

NodeId GraphDocument::InsertReroute(const Edge& e, Vec2 pos)
{
    const char* rt = m_model->RerouteTypeId();
    if (!rt) return 0;
    const Edge* ex = m_model->FindEdgeTo(e.to);
    if (!ex || !(*ex == e)) return 0;
    const NodeTypeDesc* desc = m_model->FindNodeType(rt);
    if (!desc) return 0;
    const Vec2 half = m_layout.For(*desc).size * 0.5f;
    m_history.BeginGroup("リルート点を挿入");
    const NodeId rid = AddNode(rt, pos - half);
    bool ok = rid != 0;
    if (ok) ok = Connect(e.from, PinRef{rid, 0, false});
    if (ok) ok = Connect(PinRef{rid, 0, true}, e.to);
    m_history.EndGroup();
    if (!ok)
    {
        // 途中で失敗したら丸ごと戻す（モデルが型を拒否した等）
        if (m_history.CanUndo()) m_history.Undo(*this);
        m_history.DropRedo();
        return 0;
    }
    return rid;
}

void GraphDocument::Clear()
{
    m_model->Clear();
    m_comments.clear();
    m_nextCommentId = 1;
    ++m_commentRev;
    m_history.Clear();
}

// ---- 空間分割 ----
Bezier GraphDocument::EdgeCurve(const Edge& e) const
{
    return MakeWire(PinPos(e.from), PinPos(e.to));
}

void GraphDocument::RebuildIndex()
{
    m_nodeGrid.Clear();
    m_edgeGrid.Clear();
    m_commentGrid.Clear();
    for (NodeId id : m_model->Nodes())
    {
        const Rect r = NodeRect(id);
        if (r.Width() >= 0.0f) m_nodeGrid.Insert(id, r);
    }
    const std::vector<Edge>& edges = m_model->Edges();
    for (size_t i = 0; i < edges.size(); ++i)
        m_edgeGrid.Insert(static_cast<uint32_t>(i), EdgeCurve(edges[i]).Bounds().Expanded(4.0f));
    for (const Comment& c : m_comments) m_commentGrid.Insert(c.id, c.rect);
    m_indexModelRev = m_model->Revision();
    m_indexCommentRev = m_commentRev;
}

void GraphDocument::EnsureIndex()
{
    if (m_indexModelRev != m_model->Revision() || m_indexCommentRev != m_commentRev) RebuildIndex();
}

void GraphDocument::NodesInRect(const Rect& r, bool fullyInside, std::vector<NodeId>& out)
{
    EnsureIndex();
    out.clear();
    m_tmpIds.clear();
    m_nodeGrid.Query(r, m_tmpIds);
    for (uint32_t id : m_tmpIds)
    {
        const Rect nr = NodeRect(id);
        if (nr.Width() < 0.0f) continue;
        if (fullyInside ? r.Contains(nr) : r.Overlaps(nr)) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
}

void GraphDocument::NodesInRectBrute(const Rect& r, bool fullyInside, std::vector<NodeId>& out) const
{
    out.clear();
    for (NodeId id : m_model->Nodes())
    {
        const Rect nr = NodeRect(id);
        if (nr.Width() < 0.0f) continue;
        if (fullyInside ? r.Contains(nr) : r.Overlaps(nr)) out.push_back(id);
    }
}

void GraphDocument::CommentsInRect(const Rect& r, bool fullyInside, std::vector<CommentId>& out) const
{
    out.clear();
    for (const Comment& c : m_comments)
        if (fullyInside ? r.Contains(c.rect) : r.Overlaps(c.rect)) out.push_back(c.id);
}

void GraphDocument::EdgesInRect(const Rect& r, std::vector<int>& out)
{
    EnsureIndex();
    out.clear();
    m_tmpIds.clear();
    m_edgeGrid.Query(r, m_tmpIds);
    const std::vector<Edge>& edges = m_model->Edges();
    for (uint32_t i : m_tmpIds)
        if (i < edges.size() && EdgeCurve(edges[i]).Bounds().Expanded(4.0f).Overlaps(r)) out.push_back(static_cast<int>(i));
    std::sort(out.begin(), out.end());
}

void GraphDocument::EdgesInRectBrute(const Rect& r, std::vector<int>& out) const
{
    out.clear();
    const std::vector<Edge>& edges = m_model->Edges();
    for (size_t i = 0; i < edges.size(); ++i)
        if (EdgeCurve(edges[i]).Bounds().Expanded(4.0f).Overlaps(r)) out.push_back(static_cast<int>(i));
}

Hit GraphDocument::HitTest(Vec2 p, const HitParams& hp)
{
    EnsureIndex();
    Hit hit;

    // ---- ノード（ピン > 値欄 > 本体）。手前（z が大きい）から見て、最初に当たったものを返す ----
    std::vector<NodeId> cands;
    {
        const float m = hp.pinRadius + 2.0f;
        m_tmpIds.clear();
        m_nodeGrid.Query(Rect({p.x - m, p.y - m}, {p.x + m, p.y + m}), m_tmpIds);
        cands.assign(m_tmpIds.begin(), m_tmpIds.end());
    }
    auto zOf = [&](NodeId id) -> uint32_t
    {
        if (hp.z) { auto it = hp.z->find(id); if (it != hp.z->end()) return it->second; }
        return 0;
    };
    std::sort(cands.begin(), cands.end(), [&](NodeId a, NodeId b)
    {
        const uint32_t za = zOf(a), zb = zOf(b);
        if (za != zb) return za > zb;
        return a > b;
    });
    for (NodeId id : cands)
    {
        const NodeData* n = m_model->FindNode(id);
        if (!n || !n->desc) continue;
        const NodeTypeDesc& t = *n->desc;
        const Rect r = m_layout.NodeRect(*n);
        const float pr = hp.pinRadius * (t.isReroute ? 0.6f : 1.0f);
        const float pr2 = pr * pr;
        auto pinHit = [&](bool output, size_t count) -> bool
        {
            for (size_t i = 0; i < count; ++i)
            {
                if (!output && t.inputs[i].propertyOnly) continue;
                const PinRef pin{id, static_cast<uint16_t>(i), output};
                const Vec2 pp = m_layout.PinPos(*n, pin);
                const float dx = pp.x - p.x, dy = pp.y - p.y;
                if (dx * dx + dy * dy <= pr2) { hit.kind = Hit::Pin; hit.node = id; hit.pin = pin; return true; }
            }
            return false;
        };
        if (pinHit(false, t.inputs.size())) return hit;
        if (pinHit(true, t.outputs.size())) return hit;
        if (!r.Contains(p)) continue;
        if (hp.slots && !t.isReroute)
        {
            for (size_t i = 0; i < t.inputs.size(); ++i)
            {
                const PinDesc& pd = t.inputs[i];
                if (pd.valueKind == ValueKind::None) continue;
                if (!pd.propertyOnly && m_model->FindEdgeTo(PinRef{id, static_cast<uint16_t>(i), false})) continue;   // 接続済みは値欄が無効
                const Rect sr = m_layout.SlotRect(*n, static_cast<int>(i));
                if (sr.Width() <= 0.0f || !sr.Contains(p)) continue;
                hit.kind = Hit::Slot; hit.node = id; hit.pin = PinRef{id, static_cast<uint16_t>(i), false};
                hit.slotComp = 0;
                const int nc = (pd.valueKind == ValueKind::Vec2 || pd.valueKind == ValueKind::Vec3 || pd.valueKind == ValueKind::Vec4)
                                   ? ValueComponentCount(pd.valueKind) : 1;
                for (int c = 0; c < nc; ++c)
                    if (SlotComponentRect(sr, pd.valueKind, c).Contains(p)) { hit.slotComp = c; break; }
                return hit;
            }
        }
        hit.kind = Hit::Node;
        hit.node = id;
        return hit;
    }

    // ---- コメント（リサイズ > 見出し）。手前 = ID が大きい ----
    for (auto it = m_comments.rbegin(); it != m_comments.rend(); ++it)
    {
        const Comment& c = *it;
        if (!c.rect.Contains(p)) continue;
        const Rect rs(Vec2(c.rect.max.x - hp.resizeSize, c.rect.max.y - hp.resizeSize), c.rect.max);
        if (rs.Contains(p)) { hit.kind = Hit::CommentResize; hit.comment = c.id; return hit; }
        if (p.y <= c.rect.min.y + hp.commentTitleH) { hit.kind = Hit::CommentTitle; hit.comment = c.id; return hit; }
    }

    // ---- ワイヤ ----
    {
        const float m = hp.edgeTol + 4.0f;
        std::vector<int> es;
        EdgesInRect(Rect({p.x - m, p.y - m}, {p.x + m, p.y + m}), es);
        const std::vector<Edge>& edges = m_model->Edges();
        float best = hp.edgeTol;
        int bestIdx = -1;
        for (int i : es)
        {
            const float d = DistanceToBezier(p, EdgeCurve(edges[static_cast<size_t>(i)]));
            if (d <= best) { best = d; bestIdx = i; }
        }
        if (bestIdx >= 0)
        {
            hit.kind = Hit::Edge;
            hit.edge = edges[static_cast<size_t>(bestIdx)];
            return hit;
        }
    }

    for (auto it = m_comments.rbegin(); it != m_comments.rend(); ++it)
        if (it->rect.Contains(p)) { hit.kind = Hit::CommentBody; hit.comment = it->id; return hit; }
    return hit;
}

// ---- 状態の同一性 ----
std::string GraphDocument::Signature() const
{
    std::string s;
    s.reserve(m_model->Nodes().size() * 48 + m_model->Edges().size() * 12 + 256);
    for (NodeId id : m_model->Nodes())
    {
        const NodeData* n = m_model->FindNode(id);
        if (!n) continue;
        s += 'N'; AppendU(s, id); s += ':'; s += n->type; s += '@';
        FloatBits(s, n->pos.x); s += ','; FloatBits(s, n->pos.y);
        for (const PinValue& v : n->values)
        {
            s += '|';
            AppendU(s, static_cast<uint32_t>(v.kind)); s += ':';
            for (int c = 0; c < 4; ++c) { FloatBits(s, v.f[c]); s += ' '; }
        }
        s += '\n';
    }
    for (const Edge& e : m_model->Edges())
    {
        s += 'E'; AppendU(s, e.from.node); s += '.'; AppendU(s, e.from.index); s += '>';
        AppendU(s, e.to.node); s += '.'; AppendU(s, e.to.index); s += '\n';
    }
    for (const Comment& c : m_comments)
    {
        s += 'C'; AppendU(s, c.id); s += ':'; AppendU(s, static_cast<uint32_t>(c.color)); s += ':';
        FloatBits(s, c.rect.min.x); s += ','; FloatBits(s, c.rect.min.y); s += ',';
        FloatBits(s, c.rect.max.x); s += ','; FloatBits(s, c.rect.max.y);
        s += ':'; s += c.title; s += '\n';
    }
    return s;
}

} // namespace dx12e::ng
