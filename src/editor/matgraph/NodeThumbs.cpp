#include "editor/matgraph/NodeThumbs.h"

#include <algorithm>
#include <cstring>

namespace dx12e::mg
{
namespace
{

uint64_t Fnv(uint64_t h, const void* p, size_t n)
{
    const unsigned char* c = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ull; }
    return h;
}

// 部分グラフの「見た目が変わるか」の署名: HLSL のハッシュ + 全スロットの値 / テクスチャのパス
uint64_t SignatureOf(const mat::CompileResult& r)
{
    uint64_t h = 1469598103934665603ull;
    h = Fnv(h, &r.hash, sizeof(r.hash));
    for (const mat::SlotInfo& s : r.slots)
    {
        h = Fnv(h, s.value, sizeof(s.value));
        h = Fnv(h, s.texturePath.data(), s.texturePath.size());
        const unsigned char sr = s.srgb ? 1 : 0;
        h = Fnv(h, &sr, 1);
    }
    return h;
}

const char* const kThumbTypes[] = {
    "TextureSample", "TextureParameter", "NormalMap",
    "Float", "Float2", "Float3", "Float4", "ScalarParameter", "VectorParameter",
    "Fresnel", "Custom",
};

} // namespace

NodeThumbs::NodeThumbs() = default;

NodeThumbs::~NodeThumbs()
{
    if (m_graph && m_listener) m_graph->RemoveListener(m_listener);
}

bool NodeThumbs::WantsThumb(const mat::NodeDef* def)
{
    if (!def || def->isOutput) return false;
    for (const char* t : kThumbTypes)
        if (def->type == t) return true;
    return false;
}

void NodeThumbs::ReserveInModel(MatGraphModel& model)
{
    for (const char* t : kThumbTypes) model.SetNodePreview(t, true);
}

void NodeThumbs::Attach(mat::MaterialGraph* g)
{
    if (g == m_graph) return;
    if (m_graph && m_listener) m_graph->RemoveListener(m_listener);
    m_graph = g;
    m_listener = 0;
    if (g) m_listener = g->AddListener([this](const mat::GraphChange& c) { OnChange(c); });
    m_dirtyAll = true;
}

void NodeThumbs::OnChange(const mat::GraphChange& c)
{
    switch (c.kind)
    {
    case mat::ChangeKind::NodeMoved:
    case mat::ChangeKind::CommentChanged:
    case mat::ChangeKind::SettingsChanged:
    case mat::ChangeKind::MetaChanged:
        return;   // 部分グラフの内容に影響しない
    case mat::ChangeKind::Reset:
        m_dirtyAll = true;
        return;
    default:
        if (c.node.empty()) m_dirtyAll = true;
        else m_dirty.insert(c.node);
        return;
    }
}

void NodeThumbs::CollectAncestors(const mat::MaterialGraph& g, const mat::NodeId& node, std::set<mat::NodeId>& out)
{
    std::vector<mat::NodeId> stack = {node};
    while (!stack.empty())
    {
        const mat::NodeId id = stack.back();
        stack.pop_back();
        if (!out.insert(id).second) continue;
        const mat::Node* n = g.FindNode(id);
        if (!n) continue;
        for (const auto& kv : n->inputs)
            if (kv.second.link && !out.count(kv.second.link->node)) stack.push_back(kv.second.link->node);
    }
}

int NodeThumbs::AllocTile()
{
    if (!m_freeTiles.empty())
    {
        auto it = std::min_element(m_freeTiles.begin(), m_freeTiles.end());
        const int t = *it;
        m_freeTiles.erase(it);
        return t;
    }
    return -1;
}

void NodeThumbs::FreeEntry(std::map<ng::NodeId, Entry>::iterator it, NodeThumbBackend* be)
{
    Entry& e = it->second;
    if (e.sent && be) be->Remove(e.key);
    if (e.tile >= 0)
    {
        m_freeTiles.push_back(e.tile);
        m_needDraw.erase(e.tile);
    }
    m_entries.erase(it);
}

void NodeThumbs::Reset(NodeThumbBackend* be)
{
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        if (it->second.sent && be) be->Remove(it->second.key);
        it = m_entries.erase(it);
    }
    m_freeTiles.clear();
    for (int i = 0; i < m_cfg.maxTiles; ++i) m_freeTiles.push_back(i);
    m_needDraw.clear();
    m_dirty.clear();
    m_dirtyAll = true;
}

void NodeThumbs::InvalidateAll() { m_dirtyAll = true; }

NodeThumbs::State NodeThumbs::StateOf(ng::NodeId id) const
{
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return State::None;
    return it->second.state;
}

int NodeThumbs::TileOf(ng::NodeId id) const
{
    auto it = m_entries.find(id);
    return it == m_entries.end() || it->second.state == State::Error ? -1 : it->second.tile;
}

std::string NodeThumbs::ErrorOf(ng::NodeId id) const
{
    auto it = m_entries.find(id);
    return it == m_entries.end() ? std::string() : it->second.error;
}

void NodeThumbs::MarkDrawn(int tile)
{
    m_needDraw.erase(tile);
    for (auto& kv : m_entries)
        if (kv.second.tile == tile && kv.second.state != State::Error)
        {
            kv.second.needDraw = false;
            kv.second.state = State::Ready;
        }
}

std::vector<NodeThumbs::TileWork> NodeThumbs::Update(MatGraphEditor& ed, const std::vector<ng::NodeId>& visible, NodeThumbBackend& be, uint64_t frame)
{
    std::vector<TileWork> work;
    m_stats.compiledThisFrame = 0;
    m_stats.wanted = 0;
    if (!m_enabled) { m_stats.resident = static_cast<int>(m_entries.size()); return work; }
    mat::MaterialGraph& g = ed.Graph();
    Attach(&g);
    if (m_freeTiles.empty() && m_entries.empty())
        for (int i = 0; i < m_cfg.maxTiles; ++i) m_freeTiles.push_back(i);

    // ---- 1. 可視の対象ノード ----
    struct Want { ng::NodeId id; std::string str; };
    std::vector<Want> wanted;
    for (ng::NodeId id : visible)
    {
        const mat::NodeDef* def = ed.Model().DefOf(id);
        if (!WantsThumb(def)) continue;
        const std::string s = ed.Model().StrOf(id);
        if (s.empty()) continue;
        wanted.push_back({id, s});
    }
    m_stats.wanted = static_cast<int>(wanted.size());

    // ---- 2. 変更の反映（上流に変更ノードを含むものだけ作り直し）----
    if (m_dirtyAll)
    {
        for (auto& kv : m_entries) kv.second.needCompile = true;
        m_dirtyAll = false;
        m_dirty.clear();
    }
    else if (!m_dirty.empty())
    {
        for (auto& kv : m_entries)
        {
            Entry& e = kv.second;
            if (e.needCompile) continue;
            bool hit = false;
            for (const mat::NodeId& d : m_dirty) if (e.anc.count(d)) { hit = true; break; }
            if (!hit)
            {
                // 今の上流（接続が変わって増えた分）にも変更ノードがあるか
                std::set<mat::NodeId> cur;
                CollectAncestors(g, e.nodeStr, cur);
                for (const mat::NodeId& d : m_dirty) if (cur.count(d)) { hit = true; break; }
            }
            if (hit) e.needCompile = true;
        }
        m_dirty.clear();
    }

    // ---- 3. 対象ノードへタイルを割り当てる（新規）。上限を超えたら LRU の画面外を解放、それでも無ければ「出さない」----
    for (const Want& w : wanted)
    {
        auto it = m_entries.find(w.id);
        if (it != m_entries.end())
        {
            it->second.lastWanted = frame;
            if (it->second.nodeStr != w.str) { it->second.nodeStr = w.str; it->second.key = KeyFor(w.str); it->second.needCompile = true; }
            continue;
        }
        int tile = AllocTile();
        if (tile < 0)
        {
            // 画面外のうち一番古いものを解放
            auto victim = m_entries.end();
            for (auto e = m_entries.begin(); e != m_entries.end(); ++e)
            {
                if (e->second.lastWanted == frame) continue;
                if (victim == m_entries.end() || e->second.lastWanted < victim->second.lastWanted) victim = e;
            }
            if (victim != m_entries.end())
            {
                FreeEntry(victim, &be);
                ++m_stats.evicted;
                tile = AllocTile();
            }
        }
        if (tile < 0) { ++m_stats.overBudget; continue; }
        Entry e;
        e.tile = tile;
        e.nodeStr = w.str;
        e.key = KeyFor(w.str);
        e.lastWanted = frame;
        e.needCompile = true;
        m_entries[w.id] = std::move(e);
    }

    // ---- 4. 部分グラフを作り直す（1 フレームの上限つき）。可視のノードを先に ----
    int budget = m_cfg.maxCompilesPerFrame;
    auto compileOne = [&](ng::NodeId id, Entry& e) {
        mat::CompileOptions opt;
        opt.previewNode = e.nodeStr;
        mat::CompileResult r = mat::CompileGraph(g, opt);
        ++m_stats.compilesTotal;
        ++m_stats.compiledThisFrame;
        e.anc.clear();
        CollectAncestors(g, e.nodeStr, e.anc);
        e.needCompile = false;
        if (!r.ok)
        {
            e.state = State::Error;
            e.error.clear();
            for (const mat::Diagnostic& d : r.diagnostics)
                if (d.severity == mat::Severity::Error) { e.error = d.message; break; }
            if (e.error.empty()) e.error = "プレビューできません";
            if (e.sent) { be.Remove(e.key); e.sent = false; }
            e.needDraw = false;
            m_needDraw.erase(e.tile);
            return;
        }
        const uint64_t sig = SignatureOf(r);
        if (e.sent && sig == e.sig && e.state != State::Error)
        {
            ++m_stats.skippedSame;
            return;
        }
        e.sig = sig;
        e.error.clear();
        if (e.state == State::Error || e.state == State::None) e.state = State::Pending;
        be.SetCompiled(e.key, std::make_shared<const mat::CompileResult>(std::move(r)));
        e.sent = true;
        e.needDraw = true;
        m_needDraw.insert(e.tile);
        ++m_stats.sentTotal;
        (void)id;
    };
    for (const Want& w : wanted)
    {
        if (budget <= 0) break;
        auto it = m_entries.find(w.id);
        if (it == m_entries.end() || !it->second.needCompile) continue;
        compileOne(w.id, it->second);
        --budget;
    }

    // ---- 5. 画面外のタイルの解放 ----
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        if (frame > it->second.lastWanted + static_cast<uint64_t>(m_cfg.evictAfterFrames))
        {
            auto cur = it++;
            FreeEntry(cur, &be);
            ++m_stats.evicted;
        }
        else
            ++it;
    }

    // ---- 6. 描く物（GPU が使えるまで毎フレーム含める）----
    for (auto& kv : m_entries)
        if (kv.second.needDraw && kv.second.state != State::Error) work.push_back({kv.second.tile, kv.second.key});
    m_stats.resident = static_cast<int>(m_entries.size());
    m_stats.drawPending = static_cast<int>(work.size());
    return work;
}

} // namespace dx12e::mg
