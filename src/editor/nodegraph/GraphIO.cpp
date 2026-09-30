#include "editor/nodegraph/GraphIO.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <map>

namespace dx12e::ng
{

namespace
{
using json = nlohmann::ordered_json;

double R(float v, double scale) { return std::round(static_cast<double>(v) * scale) / scale; }
double Rpos(float v) { return R(v, 100.0); }
double Rval(float v) { return R(v, 10000.0); }

json ValueJson(const PinValue& v)
{
    json a = json::array();
    const int n = ValueComponentCount(v.kind);
    for (int i = 0; i < n; ++i) a.push_back(Rval(v.f[i]));
    return a;
}

json NodeJson(const NodeData& n)
{
    json j = json::object();
    j["id"] = n.id;
    j["type"] = n.type;
    j["pos"] = json::array({Rpos(n.pos.x), Rpos(n.pos.y)});
    json vals = json::object();
    if (n.desc)
    {
        for (size_t i = 0; i < n.desc->inputs.size() && i < n.values.size(); ++i)
        {
            const PinDesc& pd = n.desc->inputs[i];
            if (pd.valueKind == ValueKind::None) continue;
            PinValue def = pd.defaultValue;
            def.kind = pd.valueKind;
            if (n.values[i] == def) continue;
            vals[std::to_string(i)] = ValueJson(n.values[i]);
        }
    }
    if (!vals.empty()) j["values"] = std::move(vals);
    if (!n.extra.empty()) j["extra"] = n.extra;   // モデル固有の状態（不透明な文字列）
    return j;
}

json CommentJson(const Comment& c)
{
    json j = json::object();
    j["id"] = c.id;
    j["rect"] = json::array({Rpos(c.rect.min.x), Rpos(c.rect.min.y), Rpos(c.rect.max.x), Rpos(c.rect.max.y)});
    j["title"] = c.title;
    j["color"] = c.color;
    return j;
}

std::string Compose(const std::string& kind, const json* view, const std::vector<json>& nodes,
                    const std::vector<json>& edges, const std::vector<json>& comments)
{
    std::string s = "{\n";
    s += " \"kind\": \"" + kind + "\",\n";
    s += " \"version\": 1,\n";
    if (view) s += " \"view\": " + view->dump() + ",\n";
    auto block = [&](const char* name, const std::vector<json>& items, bool last)
    {
        s += std::string(" \"") + name + "\": [";
        if (!items.empty()) s += "\n";
        for (size_t i = 0; i < items.size(); ++i)
        {
            s += "  " + items[i].dump();
            s += (i + 1 < items.size()) ? ",\n" : "\n ";
        }
        s += last ? "]\n" : "],\n";
    };
    block("nodes", nodes, false);
    block("edges", edges, false);
    block("comments", comments, true);
    s += "}\n";
    return s;
}

// values オブジェクトから NodeData.values を組む（既定値は desc から）。
void ApplyValues(NodeData& d, const NodeTypeDesc& t, const json& jn)
{
    d.values.clear();
    for (const PinDesc& p : t.inputs)
    {
        PinValue v = p.defaultValue;
        v.kind = p.valueKind;
        d.values.push_back(v);
    }
    auto it = jn.find("values");
    if (it == jn.end() || !it->is_object()) return;
    for (auto& kv : it->items())
    {
        int idx = -1;
        try { idx = std::stoi(kv.key()); } catch (...) { continue; }
        if (idx < 0 || static_cast<size_t>(idx) >= d.values.size() || !kv.value().is_array()) continue;
        PinValue& v = d.values[static_cast<size_t>(idx)];
        if (v.kind == ValueKind::None) continue;
        const int n = ValueComponentCount(v.kind);
        for (int c = 0; c < n && static_cast<size_t>(c) < kv.value().size(); ++c)
            if (kv.value()[static_cast<size_t>(c)].is_number()) v.f[c] = kv.value()[static_cast<size_t>(c)].get<float>();
    }
}

std::string ExtraOf(const json& jn)
{
    auto it = jn.find("extra");
    return (it != jn.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

bool ReadVec2(const json& j, const char* key, Vec2& out)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) return false;
    out = Vec2((*it)[0].get<float>(), (*it)[1].get<float>());
    return true;
}

bool ReadRect(const json& j, Rect& out)
{
    auto it = j.find("rect");
    if (it == j.end() || !it->is_array() || it->size() < 4) return false;
    for (size_t i = 0; i < 4; ++i) if (!(*it)[i].is_number()) return false;
    out = Rect(Vec2((*it)[0].get<float>(), (*it)[1].get<float>()), Vec2((*it)[2].get<float>(), (*it)[3].get<float>()));
    return true;
}

json ParseOrNull(const std::string& text, const char* expectKind, std::string* err)
{
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
    {
        if (err) *err = "JSON として読めません";
        return json();
    }
    auto k = j.find("kind");
    if (k == j.end() || !k->is_string() || k->get<std::string>() != expectKind)
    {
        if (err) *err = std::string("種別が違います（") + expectKind + " ではありません）";
        return json();
    }
    return j;
}
} // namespace

std::string SaveGraphJson(const GraphDocument& doc, const ViewState* view)
{
    const IGraphModel& m = doc.Model();
    std::vector<json> nodes, edges, comments;
    for (NodeId id : m.Nodes())
        if (const NodeData* n = m.FindNode(id)) nodes.push_back(NodeJson(*n));
    for (const Edge& e : m.Edges())
        edges.push_back(json::array({e.from.node, static_cast<int>(e.from.index), e.to.node, static_cast<int>(e.to.index)}));
    for (const Comment& c : doc.Comments()) comments.push_back(CommentJson(c));
    json v;
    if (view)
    {
        v = json::object();
        v["pan"] = json::array({Rpos(view->pan.x), Rpos(view->pan.y)});
        v["zoom"] = R(view->zoom, 10000.0);
    }
    return Compose("nodegraph", view ? &v : nullptr, nodes, edges, comments);
}

bool LoadGraphJson(GraphDocument& doc, const std::string& text, ViewState* outView, std::string* warnings)
{
    std::string err;
    json j = ParseOrNull(text, "nodegraph", &err);
    if (j.is_null())
    {
        if (warnings) *warnings = err;
        return false;
    }
    IGraphModel& m = doc.Model();
    doc.Clear();
    int skipNodes = 0, skipEdges = 0;

    if (outView)
    {
        auto vi = j.find("view");
        if (vi != j.end() && vi->is_object())
        {
            ReadVec2(*vi, "pan", outView->pan);
            auto z = vi->find("zoom");
            if (z != vi->end() && z->is_number()) outView->zoom = ClampZoom(z->get<float>());
        }
    }
    auto ni = j.find("nodes");
    if (ni != j.end() && ni->is_array())
    {
        for (const json& jn : *ni)
        {
            if (!jn.is_object() || !jn.contains("id") || !jn["id"].is_number_unsigned() || !jn.contains("type") || !jn["type"].is_string())
            { ++skipNodes; continue; }
            NodeData d;
            d.id = jn["id"].get<NodeId>();
            d.type = jn["type"].get<std::string>();
            const NodeTypeDesc* t = m.FindNodeType(d.type);
            if (!t || d.id == 0) { ++skipNodes; continue; }
            ReadVec2(jn, "pos", d.pos);
            ApplyValues(d, *t, jn);
            d.extra = ExtraOf(jn);
            if (!m.AddNode(d)) ++skipNodes;
        }
    }
    auto ei = j.find("edges");
    if (ei != j.end() && ei->is_array())
    {
        for (const json& je : *ei)
        {
            if (!je.is_array() || je.size() < 4) { ++skipEdges; continue; }
            const PinRef out{je[0].get<NodeId>(), static_cast<uint16_t>(je[1].get<int>()), true};
            const PinRef in{je[2].get<NodeId>(), static_cast<uint16_t>(je[3].get<int>()), false};
            // 型の不一致は許す（上流の型が変わって下流が不一致になった状態も保存されうる）。構造と循環だけ弾く。
            if (WouldCreateCycle(m, out, in) || !m.ConnectUnchecked(out, in)) ++skipEdges;
        }
    }
    auto ci = j.find("comments");
    if (ci != j.end() && ci->is_array())
    {
        for (const json& jc : *ci)
        {
            if (!jc.is_object()) continue;
            Comment c;
            c.id = jc.value("id", 0u);
            if (c.id == 0) c.id = doc.AllocCommentId();
            if (!ReadRect(jc, c.rect)) continue;
            c.title = jc.value("title", std::string());
            c.color = std::min(std::max(jc.value("color", 0), 0), kCommentColors - 1);
            doc.RawAddComment(c);
        }
    }
    if (warnings)
    {
        warnings->clear();
        if (skipNodes) *warnings += "読み飛ばしたノード: " + std::to_string(skipNodes) + " 件（未知の型または不正）。";
        if (skipEdges) *warnings += "読み飛ばした接続: " + std::to_string(skipEdges) + " 件。";
    }
    return true;
}

std::string CopySelectionJson(const GraphDocument& doc, const Selection& sel)
{
    const IGraphModel& m = doc.Model();
    std::vector<json> nodes, edges, comments;
    for (NodeId id : sel.nodes)
        if (const NodeData* n = m.FindNode(id)) nodes.push_back(NodeJson(*n));
    for (const Edge& e : m.Edges())
        if (sel.nodes.count(e.from.node) && sel.nodes.count(e.to.node))
            edges.push_back(json::array({e.from.node, static_cast<int>(e.from.index), e.to.node, static_cast<int>(e.to.index)}));
    for (CommentId id : sel.comments)
        if (const Comment* c = doc.FindComment(id)) comments.push_back(CommentJson(*c));
    return Compose("nodegraph-clip", nullptr, nodes, edges, comments);
}

bool ClipboardBounds(const GraphDocument& doc, const std::string& text, Rect& out)
{
    json j = ParseOrNull(text, "nodegraph-clip", nullptr);
    if (j.is_null()) return false;
    Rect b = EmptyRect();
    if (j.contains("nodes") && j["nodes"].is_array())
    {
        GraphLayout& lay = const_cast<GraphDocument&>(doc).Layout();
        for (const json& jn : j["nodes"])
        {
            Vec2 p;
            if (!jn.is_object() || !ReadVec2(jn, "pos", p)) continue;
            const NodeTypeDesc* t = doc.Model().FindNodeType(jn.value("type", std::string()));
            if (!t) continue;
            b.Add(Rect::FromPosSize(p, lay.For(*t).size));
        }
    }
    if (j.contains("comments") && j["comments"].is_array())
        for (const json& jc : j["comments"])
        {
            Rect r;
            if (jc.is_object() && ReadRect(jc, r)) b.Add(r);
        }
    if (IsEmptyRect(b)) return false;
    out = b;
    return true;
}

bool PasteJson(GraphDocument& doc, const std::string& text, Vec2 offset, PasteResult* out, std::string* error)
{
    std::string err;
    json j = ParseOrNull(text, "nodegraph-clip", &err);
    if (j.is_null())
    {
        if (error) *error = err;
        return false;
    }
    IGraphModel& m = doc.Model();
    PasteResult res;
    std::map<NodeId, NodeId> remap;
    doc.History().BeginGroup("貼り付け");
    if (j.contains("nodes") && j["nodes"].is_array())
    {
        for (const json& jn : j["nodes"])
        {
            if (!jn.is_object() || !jn.contains("id") || !jn["id"].is_number_unsigned() || !jn.contains("type") || !jn["type"].is_string()) continue;
            NodeData d;
            const NodeId oldId = jn["id"].get<NodeId>();
            d.type = jn["type"].get<std::string>();
            const NodeTypeDesc* t = m.FindNodeType(d.type);
            if (!t) continue;
            ReadVec2(jn, "pos", d.pos);
            d.pos = d.pos + offset;
            ApplyValues(d, *t, jn);
            d.extra = ExtraOf(jn);
            const NodeId nid = doc.AddNodeData(std::move(d));
            if (!nid) continue;
            remap[oldId] = nid;
            res.selection.nodes.insert(nid);
            ++res.nodes;
        }
    }
    if (j.contains("edges") && j["edges"].is_array())
    {
        for (const json& je : j["edges"])
        {
            if (!je.is_array() || je.size() < 4) continue;
            auto a = remap.find(je[0].get<NodeId>());
            auto b = remap.find(je[2].get<NodeId>());
            if (a == remap.end() || b == remap.end()) continue;
            const PinRef o{a->second, static_cast<uint16_t>(je[1].get<int>()), true};
            const PinRef i{b->second, static_cast<uint16_t>(je[3].get<int>()), false};
            if (doc.Connect(o, i)) ++res.edges;
        }
    }
    if (j.contains("comments") && j["comments"].is_array())
    {
        for (const json& jc : j["comments"])
        {
            Rect r;
            if (!jc.is_object() || !ReadRect(jc, r)) continue;
            const CommentId cid = doc.AddComment(r.Translated(offset), jc.value("title", std::string()), jc.value("color", 0));
            res.selection.comments.insert(cid);
            ++res.comments;
        }
    }
    doc.History().EndGroup();
    if (out) *out = std::move(res);
    return true;
}

} // namespace dx12e::ng
