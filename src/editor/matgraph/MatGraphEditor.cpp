#include "editor/matgraph/MatGraphEditor.h"

#include "renderer/matgraph/GraphIO.h"
#include "core/AtomicFile.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>

namespace dx12e::mg
{

namespace
{

// ---------------------------------------------------------------------------
// Undo コマンド（G0 の独立スタックへ積む）
// ---------------------------------------------------------------------------
class CmdSetProp final : public ng::IGraphCommand
{
public:
    CmdSetProp(ng::NodeId id, std::string key, mat::Json o, mat::Json n) : m_id(id), m_key(std::move(key)), m_old(std::move(o)), m_new(std::move(n)) {}
    void Undo(ng::GraphDocument& d) override { static_cast<MatGraphModel&>(d.Model()).SetNodeProp(m_id, m_key, m_old); }
    void Redo(ng::GraphDocument& d) override { static_cast<MatGraphModel&>(d.Model()).SetNodeProp(m_id, m_key, m_new); }
    const char* GetName() const override { return "プロパティを変更"; }
private:
    ng::NodeId m_id; std::string m_key; mat::Json m_old, m_new;
};

class CmdSetSettings final : public ng::IGraphCommand
{
public:
    CmdSetSettings(mat::GraphSettings o, mat::GraphSettings n) : m_old(std::move(o)), m_new(std::move(n)) {}
    void Undo(ng::GraphDocument& d) override { static_cast<MatGraphModel&>(d.Model()).Graph().SetSettings(m_old); }
    void Redo(ng::GraphDocument& d) override { static_cast<MatGraphModel&>(d.Model()).Graph().SetSettings(m_new); }
    const char* GetName() const override { return "グラフ設定を変更"; }
private:
    mat::GraphSettings m_old, m_new;
};

std::string NewGuid()
{
    std::random_device rd;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
    return buf;
}

std::string Leaf(const std::string& path)
{
    const size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

// 区切りを "/" にそろえる（アセットブラウザは "\\" 区切りで渡してくる）
std::string NormalizePath(const std::string& p)
{
    return std::filesystem::path(p).lexically_normal().generic_string();
}

std::string StemOf(const std::string& path)
{
    std::string l = Leaf(path);
    const size_t d = l.find_last_of('.');
    if (d != std::string::npos && d > 0) l.resize(d);
    return l;
}

bool ReadAll(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

struct Rgb { int r, g, b; };
bool ParseHex(const std::string& s, Rgb& out)
{
    if (s.size() != 7 || s[0] != '#') return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str() + 1, &end, 16);
    if (end != s.c_str() + s.size()) return false;
    out = Rgb{static_cast<int>((v >> 16) & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>(v & 0xFF)};
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// コメント色
// ---------------------------------------------------------------------------
const char* CommentHex(int i)
{
    static const char* const k[ng::kCommentColors] = {"#2f96ff", "#3dbfa8", "#5cc07a", "#e0a840", "#e08a4a", "#d060b0"};
    return k[(i >= 0 && i < ng::kCommentColors) ? i : 0];
}

int CommentColorFromHex(const std::string& hex)
{
    Rgb c;
    if (!ParseHex(hex, c)) return 0;
    int best = 0;
    long bestD = 1L << 30;
    for (int i = 0; i < ng::kCommentColors; ++i)
    {
        Rgb k;
        ParseHex(CommentHex(i), k);
        const long d = (long)(c.r - k.r) * (c.r - k.r) + (long)(c.g - k.g) * (c.g - k.g) + (long)(c.b - k.b) * (c.b - k.b);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// MatGraphEditor
// ---------------------------------------------------------------------------
MatGraphEditor::MatGraphEditor()
{
    m_graph = std::make_unique<mat::MaterialGraph>();
    m_model = std::make_unique<MatGraphModel>(m_graph.get());
    m_doc = std::make_unique<ng::GraphDocument>(m_model.get());
    std::random_device rd;
    m_graph->SetIdSeed(rd());
    NewGraph(NewTemplate::Empty);
}

MatGraphEditor::~MatGraphEditor()
{
    // 文書 → モデル → グラフの順（モデルはグラフのリスナーを外す）
    m_doc.reset();
    m_model.reset();
    m_graph.reset();
}

std::string MatGraphEditor::DisplayName() const
{
    if (m_path.empty()) return "名称未設定";
    return Leaf(m_path);
}

void MatGraphEditor::NewGraph(NewTemplate t)
{
    m_doc->Clear();   // モデルも履歴もコメントも空にする
    m_model->ResetMapping();
    m_graph->SetGuid(NewGuid());
    BuildTemplate(*m_graph, t);
    m_path.clear();
    m_hasLoadedView = false;
    SyncCommentsFromGraph();
    m_hasResult = false;
    MarkSaved();
    Update();
}

bool MatGraphEditor::Open(const std::string& path, std::string* error)
{
    std::string text;
    if (!ReadAll(path, text))
    {
        if (error) *error = "ファイルを開けません: " + path;
        return false;
    }
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    {
        // 先に別のグラフで検証する（壊れたファイルで今の編集内容を失わない）
        mat::MaterialGraph probe(&m_graph->Library());
        if (!mat::LoadDxmg(text, probe, error)) return false;
    }
    m_doc->Clear();
    m_model->ResetMapping();
    std::string err2;
    if (!mat::LoadDxmg(text, *m_graph, &err2))
    {
        if (error) *error = err2;
        return false;
    }
    m_path = NormalizePath(path);
    SyncCommentsFromGraph();
    const mat::ViewInfo& v = m_graph->View();
    m_hasLoadedView = v.valid;
    if (v.valid) { m_loadedView.pan = ng::Vec2(v.panX, v.panY); m_loadedView.zoom = v.zoom; }
    m_hasResult = false;
    MarkSaved();
    Update();
    return true;
}

bool MatGraphEditor::TakeLoadedView(ng::ViewState* out)
{
    if (!m_hasLoadedView) return false;
    m_hasLoadedView = false;
    if (out) *out = m_loadedView;
    return true;
}

void MatGraphEditor::SyncCommentsToGraph()
{
    std::vector<std::string> old;
    for (const auto& kv : m_graph->Comments()) old.push_back(kv.first);
    for (const std::string& id : old) m_graph->RemoveComment(id);
    for (const ng::Comment& c : m_doc->Comments())
    {
        mat::Comment gc;
        char buf[24];
        std::snprintf(buf, sizeof(buf), "c_%04x", static_cast<unsigned>(c.id));
        gc.id = buf;
        gc.text = c.title;
        gc.color = CommentHex(c.color);
        gc.x = c.rect.min.x; gc.y = c.rect.min.y; gc.w = c.rect.Width(); gc.h = c.rect.Height();
        m_graph->AddComment(gc);
    }
    m_syncedCommentRev = m_doc->CommentRevision();
}

void MatGraphEditor::SyncCommentsFromGraph()
{
    m_doc->RawClearComments();
    for (const auto& kv : m_graph->Comments())
    {
        const mat::Comment& gc = kv.second;
        ng::Comment c;
        unsigned long id = 0;
        if (gc.id.size() > 2 && gc.id[0] == 'c' && gc.id[1] == '_')
        {
            char* end = nullptr;
            id = std::strtoul(gc.id.c_str() + 2, &end, 16);
            if (end != gc.id.c_str() + gc.id.size()) id = 0;
        }
        if (id != 0)
            c.id = static_cast<ng::CommentId>(id);
        else
            c.id = m_doc->AllocCommentId();
        c.title = gc.text;
        c.color = CommentColorFromHex(gc.color);
        c.rect = ng::Rect(ng::Vec2(gc.x, gc.y), ng::Vec2(gc.x + gc.w, gc.y + gc.h));
        m_doc->RawAddComment(c);
    }
    m_syncedCommentRev = m_doc->CommentRevision();
}

std::string MatGraphEditor::CanonicalText(bool includeView) const
{
    if (m_doc->CommentRevision() != m_syncedCommentRev) const_cast<MatGraphEditor*>(this)->SyncCommentsToGraph();
    return mat::SaveDxmg(*m_graph, includeView);
}

void MatGraphEditor::MarkSaved()
{
    m_savedText = CanonicalText(false);
    m_dirtyVersion = ~0ull;
    m_dirtyCache = false;
}

bool MatGraphEditor::IsDirty() const
{
    if (m_doc->CommentRevision() != m_syncedCommentRev) const_cast<MatGraphEditor*>(this)->SyncCommentsToGraph();
    if (m_graph->Version() != m_dirtyVersion)
    {
        m_dirtyCache = mat::SaveDxmg(*m_graph, false) != m_savedText;
        m_dirtyVersion = m_graph->Version();
    }
    return m_dirtyCache;
}

bool MatGraphEditor::WriteTo(const std::string& path, std::string* error)
{
    SyncCommentsToGraph();
    const std::string text = mat::SaveDxmg(*m_graph, true);
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    // 一時ファイル→flush→検証→置き換え（途中で落ちても元の .dxmg は無傷）
    const auto wr = dx12e::atomicfile::WriteFile(p, text);
    if (!wr)
    {
        if (error) *error = "書き込みに失敗しました: " + path + " (" + wr.error + ")";
        return false;
    }
    return true;
}

bool MatGraphEditor::SaveAs(const std::string& path, std::string* error, const ng::ViewState* view)
{
    if (view)
    {
        mat::ViewInfo v;
        v.valid = true; v.panX = view->pan.x; v.panY = view->pan.y; v.zoom = view->zoom;
        m_graph->SetView(v);
    }
    if (m_graph->Name().empty() || m_graph->Name() == "Untitled") m_graph->SetName(StemOf(path));
    if (!WriteTo(path, error)) return false;
    m_path = NormalizePath(path);
    MarkSaved();
    return true;
}

bool MatGraphEditor::Save(std::string* error, const ng::ViewState* view)
{
    if (m_path.empty())
    {
        if (error) *error = "保存先がありません（名前を付けて保存してください）";
        return false;
    }
    return SaveAs(m_path, error, view);
}

// ---------------------------------------------------------------------------
// コンパイル・診断
// ---------------------------------------------------------------------------
void MatGraphEditor::Recompile()
{
    const auto t0 = std::chrono::steady_clock::now();
    mat::CompileOptions opt;
    opt.sourceName = m_path.empty() ? std::string("(untitled)") : Leaf(m_path);
    m_result = mat::CompileGraph(*m_graph, opt);
    m_lastCompileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++m_compileCount;
    m_hasResult = true;
    m_compiledStruct = m_graph->StructureVersion();
    m_diags = m_result.diagnostics;
    m_diagTick = m_model->AnalysisTick();
    m_valueOnlyEdits = 0;
    m_errors = m_warnings = 0;
    for (const mat::Diagnostic& d : m_diags)
    {
        if (!d.reachable) continue;
        if (d.severity == mat::Severity::Error) ++m_errors;
        else if (d.severity == mat::Severity::Warning) ++m_warnings;
    }
}

void MatGraphEditor::Update()
{
    if (m_doc->CommentRevision() != m_syncedCommentRev) SyncCommentsToGraph();
    if (!m_hasResult || m_graph->StructureVersion() != m_compiledStruct)
    {
        Recompile();
        return;
    }
    if (m_model->AnalysisTick() != m_diagTick)
    {
        // 値だけ / メタ情報だけの変更: HLSL は 1 バイトも変わらないので再生成しない。診断（重複名など）だけ最新にする
        m_diags = m_model->Analysis().diags;
        m_diagTick = m_model->AnalysisTick();
        ++m_valueOnlyEdits;
        ++m_valueOnlySeq;
        m_errors = m_warnings = 0;
        for (const mat::Diagnostic& d : m_diags)
        {
            if (!d.reachable) continue;
            if (d.severity == mat::Severity::Error) ++m_errors;
            else if (d.severity == mat::Severity::Warning) ++m_warnings;
        }
    }
}

void MatGraphEditor::SetExternalDiagnostics(std::vector<mat::Diagnostic> d)
{
    // 同じ内容なら何もしない（毎フレーム呼ばれても UI を作り直さない）
    bool same = d.size() == m_extDiags.size();
    for (size_t i = 0; same && i < d.size(); ++i)
        same = d[i].nodeId == m_extDiags[i].nodeId && d[i].message == m_extDiags[i].message && d[i].severity == m_extDiags[i].severity && d[i].line == m_extDiags[i].line;
    if (same) return;
    m_extDiags = std::move(d);
    ++m_extRev;
}

int MatGraphEditor::ExternalErrorCount() const
{
    int n = 0;
    for (const mat::Diagnostic& d : m_extDiags)
        if (d.severity == mat::Severity::Error) ++n;
    return n;
}

std::vector<DiagItem> MatGraphEditor::Diagnostics(bool includeInfo) const
{
    std::vector<DiagItem> out;
    std::vector<const mat::Diagnostic*> all;
    for (const mat::Diagnostic& d : m_diags) all.push_back(&d);
    for (const mat::Diagnostic& d : m_extDiags) all.push_back(&d);
    for (const mat::Diagnostic* pd : all)
    {
        const mat::Diagnostic& d = *pd;
        if (!includeInfo && d.severity == mat::Severity::Info) continue;
        DiagItem it;
        it.severity = d.severity;
        it.code = d.code;
        it.pin = d.pin;
        it.message = d.message;
        it.hint = d.hint;
        it.reachable = d.reachable;
        if (!d.nodeId.empty())
        {
            it.node = m_model->IntOf(d.nodeId);
            const mat::Node* n = m_graph->FindNode(d.nodeId);
            if (n)
            {
                const mat::NodeDef* def = m_graph->DefOf(d.nodeId);
                it.nodeTitle = def ? def->displayName : n->type;
                if (def && def->isParameter)
                {
                    const mat::Json nm = m_graph->GetNodeProp(d.nodeId, "name");
                    if (nm.is_string()) it.nodeTitle += " \"" + nm.get<std::string>() + "\"";
                }
            }
            else it.nodeTitle = d.nodeId;
        }
        out.push_back(std::move(it));
    }
    std::stable_sort(out.begin(), out.end(), [](const DiagItem& a, const DiagItem& b)
    {
        if (a.reachable != b.reachable) return a.reachable;
        if (a.severity != b.severity) return static_cast<int>(a.severity) > static_cast<int>(b.severity);
        return false;
    });
    return out;
}

ng::NodeId MatGraphEditor::OutputNode() const
{
    for (const auto& kv : m_graph->Nodes())
        if (kv.second.type == mat::kOutputNodeType) return m_model->IntOf(kv.first);
    return 0;
}

ng::NodeId MatGraphEditor::FocusNodeFor(const DiagItem& d) const
{
    if (d.node != 0 && m_model->FindNode(d.node)) return d.node;
    return OutputNode();
}

std::vector<mat::ParamDecl> MatGraphEditor::Parameters() const { return mat::CollectParameters(*m_graph); }

// ---------------------------------------------------------------------------
// 編集
// ---------------------------------------------------------------------------
bool MatGraphEditor::SetNodeProp(ng::NodeId id, const std::string& key, const mat::Json& value)
{
    const mat::Json old = m_model->GetNodeProp(id, key);
    if (!m_model->SetNodeProp(id, key, value)) return false;
    const mat::Json now = m_model->GetNodeProp(id, key);
    if (now == old) return true;
    m_doc->History().Push(std::make_unique<CmdSetProp>(id, key, old, now));
    return true;
}

bool MatGraphEditor::SetPropLive(ng::NodeId id, const std::string& key, const mat::Json& value)
{
    return m_model->SetNodeProp(id, key, value);
}

bool MatGraphEditor::CommitProp(ng::NodeId id, const std::string& key, const mat::Json& oldValue)
{
    const mat::Json now = m_model->GetNodeProp(id, key);
    if (now == oldValue || now.is_null()) return false;
    m_doc->History().Push(std::make_unique<CmdSetProp>(id, key, oldValue, now));
    return true;
}

bool MatGraphEditor::CommitSettings(const mat::GraphSettings& oldValue)
{
    const mat::GraphSettings now = m_graph->Settings();
    if (now == oldValue) return false;
    m_doc->History().Push(std::make_unique<CmdSetSettings>(oldValue, now));
    return true;
}

bool MatGraphEditor::SetSettings(const mat::GraphSettings& s)
{
    const mat::GraphSettings old = m_graph->Settings();
    if (old == s) return true;
    m_graph->SetSettings(s);
    m_doc->History().Push(std::make_unique<CmdSetSettings>(old, s));
    return true;
}

bool MatGraphEditor::CanPromote(ng::NodeId id, std::string* why) const
{
    const ng::NodeData* n = m_model->FindNode(id);
    if (!n) { if (why) *why = "ノードがありません"; return false; }
    if (n->type == "Float" || n->type == "Float3" || n->type == "Float4") return true;
    if (n->type == "TextureSample" || n->type == "NormalMap")
    {
        if (m_model->FindEdgeTo(ng::PinRef{id, 0, false}))
        {
            if (why) *why = "Tex にすでに接続があります";
            return false;
        }
        return true;
    }
    if (why)
    {
        if (n->type == "Float2") *why = "Float2 に対応するパラメータはありません（VectorParameter は 4 成分）";
        else *why = "定数（Float / Float3 / Float4）とテクスチャサンプルだけがパラメータにできます";
    }
    return false;
}

bool MatGraphEditor::PromoteToParameter(ng::NodeId id, ng::NodeId* newNode)
{
    if (!CanPromote(id)) return false;
    const ng::NodeData old = *m_model->FindNode(id);
    ng::GraphHistory& hist = m_doc->History();
    mat::Json extra = mat::Json::object();
    ng::NodeData nd;

    if (old.type == "TextureSample" || old.type == "NormalMap")
    {
        // 暗黙のテクスチャ → 新しい TextureParameter を左に置いて Tex へ繋ぐ
        const mat::Json tex = m_model->GetNodeProp(id, "texture");
        const mat::Json usage = m_model->GetNodeProp(id, "usage");
        std::string path = tex.is_string() ? tex.get<std::string>() : std::string();
        std::string base = path.empty() ? std::string("Texture") : StemOf(path);
        mat::Json p = mat::Json::object();
        p["name"] = base;
        p["default"] = path;
        if (usage.is_string()) p["sampler"] = usage;
        extra["p"] = p;
        nd.type = "TextureParameter";
        nd.pos = ng::Vec2(old.pos.x - 240.0f, old.pos.y);
        nd.extra = extra.dump();
        hist.BeginGroup("パラメータに昇格");
        const ng::NodeId nid = m_doc->AddNodeData(nd);
        bool ok = nid != 0;
        if (ok) ok = m_doc->Connect(m_model->PinOf(m_model->StrOf(nid), "Out", true), ng::PinRef{id, 0, false});
        hist.EndGroup();
        if (!ok)
        {
            if (hist.CanUndo()) hist.Undo(*m_doc);
            hist.DropRedo();
            return false;
        }
        if (newNode) *newNode = nid;
        return true;
    }

    // 定数 → パラメータ（値を引き継ぎ、出力の接続を付け替えて元のノードを消す）
    const mat::Json val = m_model->GetNodeProp(id, "value");
    mat::Json p = mat::Json::object();
    if (old.type == "Float")
    {
        nd.type = "ScalarParameter";
        p["name"] = "Scalar";
        p["default"] = val;
        const double v = val.is_number() ? val.get<double>() : 0.0;
        if (v < 0.0 || v > 1.0) p["range"] = mat::Json::array({v < 0.0 ? v : 0.0, v > 1.0 ? v : 1.0});
    }
    else
    {
        nd.type = "VectorParameter";
        p["name"] = old.type == "Float3" ? "Color" : "Vector";
        mat::Json col = mat::Json::array({0.0, 0.0, 0.0, 1.0});
        if (val.is_array())
            for (size_t k = 0; k < val.size() && k < 4; ++k) col[k] = val[k];
        p["default"] = col;
    }
    extra["p"] = p;
    nd.pos = old.pos;
    nd.extra = extra.dump();

    // 元の出力からの接続（出力ピン名 → 行き先）
    struct Link { std::string fromPin; ng::PinRef to; };
    std::vector<Link> links;
    const ng::NodeTypeDesc* od = old.desc;
    for (const ng::Edge& e : m_model->Edges())
    {
        if (e.from.node != id) continue;
        if (!od || e.from.index >= od->outputs.size()) continue;
        links.push_back(Link{od->outputs[e.from.index].name, e.to});
    }

    hist.BeginGroup("パラメータに昇格");
    const ng::NodeId nid = m_doc->AddNodeData(nd);
    bool ok = nid != 0;
    const std::string nsid = ok ? m_model->StrOf(nid) : std::string();
    for (const Link& l : links)
    {
        if (!ok) break;
        // Float3 の "Out"（RGB）は VectorParameter の "RGB"。それ以外は同名
        const std::string pin = (old.type == "Float3" && l.fromPin == "Out") ? "RGB" : l.fromPin;
        const ng::PinRef from = m_model->PinOf(nsid, pin, true);
        if (!from.Valid() || !m_doc->Connect(from, l.to)) ok = false;
    }
    if (ok)
    {
        ng::Selection s;
        s.nodes.insert(id);
        m_doc->RemoveItems(s);
    }
    hist.EndGroup();
    if (!ok)
    {
        if (hist.CanUndo()) hist.Undo(*m_doc);
        hist.DropRedo();
        return false;
    }
    if (newNode) *newNode = nid;
    return true;
}

std::string MatGraphEditor::Signature() const
{
    std::string s = m_doc->Signature();
    const mat::GraphSettings& st = m_graph->Settings();
    s += "S:" + st.blendMode + "/" + st.shadingModel + "/" + (st.twoSided ? "2" : "1") + "/" + std::to_string(st.maskClip) + "\n";
    return s;
}

// ---------------------------------------------------------------------------
// ひな形・見本
// ---------------------------------------------------------------------------
namespace
{

struct Builder
{
    mat::MaterialGraph& g;
    explicit Builder(mat::MaterialGraph& graph) : g(graph) {}
    void Add(const char* id, const char* type, float x, float y) { g.AddNode(type, x, y, id); }
    template <class T> void P(const char* id, const char* key, const T& v) { g.SetNodeProp(id, key, mat::Json(v)); }
    void PJ(const char* id, const char* key, const mat::Json& v) { g.SetNodeProp(id, key, v); }
    void Link(const char* from, const char* fromPin, const char* to, const char* toPin) { g.Connect(from, fromPin, to, toPin); }
    void Lit(const char* id, const char* pin, float v) { g.SetLiteral(id, pin, mat::Value::Float(v)); }
    void Comment(const char* id, const char* text, const char* color, float x, float y, float w, float h)
    {
        mat::Comment c;
        c.id = id; c.text = text; c.color = color; c.x = x; c.y = y; c.w = w; c.h = h;
        g.AddComment(c);
    }
};

} // namespace

void BuildTemplate(mat::MaterialGraph& g, NewTemplate t)
{
    Builder b(g);
    g.SetName("Untitled");
    if (t == NewTemplate::Empty)
    {
        b.Add("out", "MaterialOutput",  420.0f, 120.0f);
        return;
    }
    if (t == NewTemplate::Sample)
    {
        BuildSampleGraph(g);
        return;
    }
    if (t == NewTemplate::Big200)
    {
        BuildBigGraph(g, 200, 0.0f);
        SaltGraph(g, 0.0f);
        return;
    }
    // 標準 PBR: BaseColor = テクスチャ、Roughness / Metallic = パラメータ、Normal = 法線マップ
    b.Add("uv", "TexCoord", -420.0f, 40.0f);
    b.Add("tBase", "TextureParameter", -420.0f, 180.0f);
    b.PJ("tBase", "name", "BaseColor"); b.PJ("tBase", "sampler", "Color"); b.PJ("tBase", "group", "Textures");
    b.Add("sBase", "TextureSample", -170.0f, 40.0f);
    b.Add("tNormal", "TextureParameter", -420.0f, 340.0f);
    b.PJ("tNormal", "name", "Normal"); b.PJ("tNormal", "sampler", "Normal"); b.PJ("tNormal", "group", "Textures");
    b.Add("nrm", "NormalMap", -170.0f, 300.0f);
    b.Add("rough", "ScalarParameter", -170.0f, 480.0f);
    b.PJ("rough", "name", "Roughness"); b.PJ("rough", "default", 0.5);
    b.Add("metal", "ScalarParameter", -170.0f, 600.0f);
    b.PJ("metal", "name", "Metallic"); b.PJ("metal", "default", 0.0);
    b.Add("out", "MaterialOutput", 280.0f, 120.0f);
    b.Link("uv", "UV", "sBase", "UV");
    b.Link("tBase", "Out", "sBase", "Tex");
    b.Link("uv", "UV", "nrm", "UV");
    b.Link("tNormal", "Out", "nrm", "Tex");
    b.Link("sBase", "RGB", "out", "BaseColor");
    b.Link("nrm", "Out", "out", "Normal");
    b.Link("rough", "Out", "out", "Roughness");
    b.Link("metal", "Out", "out", "Metallic");
}

void BuildSampleGraph(mat::MaterialGraph& g)
{
    Builder b(g);
    g.SetName("SampleRockWet");

    // ---- ベースカラー = テクスチャ × 色（汚れとブレンド）----
    b.Add("uvA", "TexCoord", 0.0f, 30.0f);
    b.PJ("uvA", "tiling", mat::Json::array({3.0, 3.0}));
    b.Add("tBase", "TextureParameter", 0.0f, 150.0f);
    b.PJ("tBase", "name", "BaseColorTex"); b.PJ("tBase", "sampler", "Color"); b.PJ("tBase", "group", "Textures");
    b.PJ("tBase", "default", "textures/rock/rock_diff.jpg");
    b.Add("sBase", "TextureSample", 240.0f, 30.0f);
    b.Add("tint", "VectorParameter", 240.0f, 250.0f);
    b.PJ("tint", "name", "Tint"); b.PJ("tint", "group", "Color"); b.PJ("tint", "srgb", true);
    b.PJ("tint", "default", mat::Json::array({0.85, 0.78, 0.7, 1.0}));
    b.Add("mulBase", "Multiply", 480.0f, 60.0f);
    b.Add("dirt", "VectorParameter", 480.0f, 250.0f);
    b.PJ("dirt", "name", "DirtColor"); b.PJ("dirt", "group", "Color"); b.PJ("dirt", "srgb", true);
    b.PJ("dirt", "default", mat::Json::array({0.18, 0.14, 0.1, 1.0}));
    b.Add("lerpBase", "Lerp", 720.0f, 110.0f);

    // ---- ラフネス = ノイズ ----
    b.Add("uvB", "TexCoord", 1000.0f, 30.0f);
    b.Add("noise", "Custom", 1240.0f, 10.0f);
    b.PJ("noise", "outputType", "float");
    b.PJ("noise", "code",
         "float2 p = In0 * In1;\n"
         "float2 i = floor(p);\n"
         "float2 f = frac(p);\n"
         "float2 u = f * f * (3.0 - 2.0 * f);\n"
         "float a = frac(sin(dot(i, float2(12.9898, 78.233))) * 43758.5453);\n"
         "float b = frac(sin(dot(i + float2(1.0, 0.0), float2(12.9898, 78.233))) * 43758.5453);\n"
         "float c = frac(sin(dot(i + float2(0.0, 1.0), float2(12.9898, 78.233))) * 43758.5453);\n"
         "float d = frac(sin(dot(i + float2(1.0, 1.0), float2(12.9898, 78.233))) * 43758.5453);\n"
         "return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);\n");
    b.Add("noiseScale", "Float", 1000.0f, 150.0f);
    b.PJ("noiseScale", "value", 8.0);
    b.Add("smooth", "Smoothstep", 1480.0f, 30.0f);
    b.Lit("smooth", "Min", 0.35f); b.Lit("smooth", "Max", 0.75f);
    b.Add("rMin", "ScalarParameter", 1480.0f, 170.0f);
    b.PJ("rMin", "name", "RoughnessMin"); b.PJ("rMin", "default", 0.35); b.PJ("rMin", "group", "Surface");
    b.Add("rMax", "ScalarParameter", 1480.0f, 290.0f);
    b.PJ("rMax", "name", "RoughnessMax"); b.PJ("rMax", "default", 0.9); b.PJ("rMax", "group", "Surface");
    b.Add("lerpRough", "Lerp", 1720.0f, 90.0f);
    b.Add("dirtAmt", "ScalarParameter", 720.0f, 300.0f);
    b.PJ("dirtAmt", "name", "DirtAmount"); b.PJ("dirtAmt", "default", 0.6); b.PJ("dirtAmt", "group", "Surface");
    b.Add("mulMask", "Multiply", 720.0f, 440.0f);

    // ---- ノーマル ----
    b.Add("tNormal", "TextureParameter", 0.0f, 620.0f);
    b.PJ("tNormal", "name", "NormalTex"); b.PJ("tNormal", "sampler", "Normal"); b.PJ("tNormal", "group", "Textures");
    b.PJ("tNormal", "default", "textures/rock/rock_nor.jpg");
    b.Add("nStrength", "ScalarParameter", 240.0f, 760.0f);
    b.PJ("nStrength", "name", "NormalStrength"); b.PJ("nStrength", "default", 1.0); b.PJ("nStrength", "range", mat::Json::array({0.0, 3.0}));
    b.PJ("nStrength", "group", "Surface");
    b.Add("nrm", "NormalMap", 480.0f, 620.0f);

    // ---- フレネル → エミッシブ ----
    b.Add("fExp", "ScalarParameter", 800.0f, 620.0f);
    b.PJ("fExp", "name", "RimPower"); b.PJ("fExp", "default", 4.0); b.PJ("fExp", "range", mat::Json::array({0.5, 12.0}));
    b.PJ("fExp", "group", "Rim");
    b.Add("fres", "Fresnel", 1040.0f, 620.0f);
    b.Lit("fres", "BaseReflect", 0.0f);
    b.Add("rim", "VectorParameter", 1040.0f, 780.0f);
    b.PJ("rim", "name", "RimColor"); b.PJ("rim", "group", "Rim"); b.PJ("rim", "srgb", true);
    b.PJ("rim", "default", mat::Json::array({0.35, 0.65, 1.0, 1.0}));
    b.Add("mulRim", "Multiply", 1300.0f, 640.0f);
    b.Add("mulInt", "Multiply", 1540.0f, 640.0f);
    b.Lit("mulInt", "B", 1.5f);

    b.Add("metal", "ScalarParameter", 1720.0f, 300.0f);
    b.PJ("metal", "name", "Metallic"); b.PJ("metal", "default", 0.0); b.PJ("metal", "group", "Surface");

    b.Add("out", "MaterialOutput", 2000.0f, 260.0f);

    // ---- 接続 ----
    b.Link("uvA", "UV", "sBase", "UV");
    b.Link("tBase", "Out", "sBase", "Tex");
    b.Link("sBase", "RGB", "mulBase", "A");
    b.Link("tint", "RGB", "mulBase", "B");
    b.Link("mulBase", "Out", "lerpBase", "A");
    b.Link("dirt", "RGB", "lerpBase", "B");
    b.Link("mulMask", "Out", "lerpBase", "Alpha");
    b.Link("lerpBase", "Out", "out", "BaseColor");

    b.Link("uvB", "UV", "noise", "In0");
    b.Link("noiseScale", "Out", "noise", "In1");
    b.Link("noise", "Out", "smooth", "X");
    b.Link("smooth", "Out", "lerpRough", "Alpha");
    b.Link("rMin", "Out", "lerpRough", "A");
    b.Link("rMax", "Out", "lerpRough", "B");
    b.Link("lerpRough", "Out", "out", "Roughness");
    b.Link("smooth", "Out", "mulMask", "A");
    b.Link("dirtAmt", "Out", "mulMask", "B");
    b.Link("metal", "Out", "out", "Metallic");

    b.Link("uvA", "UV", "nrm", "UV");
    b.Link("tNormal", "Out", "nrm", "Tex");
    b.Link("nStrength", "Out", "nrm", "Strength");
    b.Link("nrm", "Out", "out", "Normal");

    b.Link("fExp", "Out", "fres", "Exponent");
    b.Link("fres", "Out", "mulRim", "A");
    b.Link("rim", "RGB", "mulRim", "B");
    b.Link("mulRim", "Out", "mulInt", "A");
    b.Link("mulInt", "Out", "out", "Emissive");

    b.Comment("c_0001", "ベースカラー = テクスチャ x 色", "#2f96ff", -30.0f, -30.0f, 960.0f, 590.0f);
    b.Comment("c_0002", "ラフネス = ノイズ", "#3dbfa8", 970.0f, -30.0f, 930.0f, 460.0f);
    b.Comment("c_0003", "ノーマル", "#e0a840", -30.0f, 580.0f, 700.0f, 290.0f);
    b.Comment("c_0004", "フレネル -> エミッシブ", "#d060b0", 770.0f, 580.0f, 1130.0f, 400.0f);
}

void SaltGraph(mat::MaterialGraph& g, float salt)
{
    // Metallic に「リテラル同士の Multiply」を挟む: リテラルは HLSL のテキストに入る（値スロットにならない）ので、salt ごとに HLSL が変わる
    if (!g.FindNode("saltm")) g.AddNode("Multiply", 2100.0f, 600.0f, "saltm");
    g.SetLiteral("saltm", "A", mat::Value::Float(0.25f));
    g.SetLiteral("saltm", "B", mat::Value::Float(0.5f + salt));
    if (g.FindNode("out")) g.Connect("saltm", "Out", "out", "Metallic");
}

void BuildBigGraph(mat::MaterialGraph& g, int targetNodes, float salt)
{
    BuildSampleGraph(g);
    // 見本の BaseColor の後ろへ「テクスチャ x 定数 → Lerp」の枝を 5 ノードずつ足して targetNodes 個にする（実測ゲートの大きめグラフ）
    mat::NodeId prev = "lerpBase";
    int i = 0;
    while (static_cast<int>(g.Nodes().size()) + 5 <= targetNodes)
    {
        const std::string n = std::to_string(i++);
        const float y = 1000.0f + static_cast<float>(i) * 20.0f;
        g.AddNode("TexCoord", -300.0f, y, "bu" + n);
        g.SetNodeProp("bu" + n, "tiling", mat::Json::array({1.0 + 0.25 * (i % 7), 1.0 + 0.5 * (i % 5)}));
        g.AddNode("TextureParameter", -300.0f, y, "bt" + n);
        g.SetNodeProp("bt" + n, "name", "BigTex" + n);
        g.SetNodeProp("bt" + n, "sampler", "Color");
        g.AddNode("TextureSample", -100.0f, y, "bs" + n);
        g.Connect("bu" + n, "UV", "bs" + n, "UV");
        g.Connect("bt" + n, "Out", "bs" + n, "Tex");
        g.AddNode("Multiply", 100.0f, y, "bm" + n);
        g.Connect("bs" + n, "RGB", "bm" + n, "A");
        g.SetLiteral("bm" + n, "B", mat::Value::Float(0.5f + 0.01f * static_cast<float>(i) + salt));
        g.AddNode("Lerp", 300.0f, y, "bl" + n);
        g.Connect(prev, "Out", "bl" + n, "A");
        g.Connect("bm" + n, "Out", "bl" + n, "B");
        g.SetLiteral("bl" + n, "Alpha", mat::Value::Float(0.15f));
        prev = "bl" + n;
    }
    g.Connect(prev, "Out", "out", "BaseColor");
}

} // namespace dx12e::mg
