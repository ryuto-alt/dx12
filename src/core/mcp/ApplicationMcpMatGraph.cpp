// ===========================================================================
// MCP: マテリアルグラフ（G2b）  material_graph_get / edit / validate / compile / status / graphize / set_param / apply / nodes
// ---------------------------------------------------------------------------
// 設計: docs/MATERIAL_GRAPH_DESIGN.md §4.5・§7 G2b、仕様: docs/MATGRAPH_G2B.md「MCP」、面: docs/MCP.md「マテリアルグラフ」。
//   ・meta を直接渡す（McpDefine(name, McpMeta, fn)）。引数は中央検査され、describe_mcp_manifest に載り、再起動なしで dx12_call から使える。
//     旧ツール名の別名は dx12_material_graph_*（core 面 40 本には入れない = 長尾。dx12_tool_search / dx12_call で使う）。
//   ・グラフの実体は G1 の MaterialGraph（src/renderer/matgraph）。編集は「複製に ops を全部当てる → 検証 → 書く」で、
//     途中で失敗したらファイルに 1 バイトも書かない。書き出しは正準形（git 差分が 1 ノード 1 行）。
//   ・書いた .dxmg はエンジンが（NotifyGraphFileChanged で）次のフレームに作り直す。HLSL が変わらない編集（定数 / パラメータの値・
//     ノードの移動）は再コンパイルしない。HLSL が変わる編集は DXC + PSO をワーカーで作り、完了まで旧版で描く。
//   ・dryRun:true は edit / graphize / set_param で「何が起こるか」だけを返す（ファイルもエンジンの状態も変えない）。
//   ・エディタ UI（ノード編集）は別担当。ここは AI / スクリプト向けの入口だけ。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/mcp/McpManifestBuild.h"
#include "core/mcp/McpSafety.h"
#include "core/AtomicFile.h"
#include "renderer/matgraph/Compiler.h"
#include "renderer/matgraph/GraphIO.h"
#include "resource/MaterialGraphize.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dx12e
{
// material_graph_panel（G2c）: 実体は editor/panels/MaterialGraphPanelPreview.cpp（ヘッダを include すると mg の名前空間が衝突するので前方宣言）
namespace MaterialGraphPanel { std::string DebugCall(EditorContext& ctx, const std::string& jsonIn); }
using namespace appdetail;
using mcpdata::P;

namespace
{
using json = nlohmann::json;
namespace mg = dx12e::matgraph;
namespace fs = std::filesystem;

[[noreturn]] void ThrowMg(int code, const char* name, const std::string& msg, const std::string& hint, const std::string& cause = {},
                          std::vector<std::string> didYouMean = {})
{
    McpError err(code, msg, hint);
    err.name = name;
    err.cause = cause;
    err.didYouMean = std::move(didYouMean);
    throw err;
}

// ---- パス --------------------------------------------------------------------------------
fs::path AssetAbs(const std::string& rel)
{
    const std::string s = PathResolver::AssetsDir() + rel;   // UTF-8
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string CheckRel(const json& p, const char* key, const char* method)
{
    if (!p.contains(key) || !p[key].is_string() || p[key].get<std::string>().empty())
        ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", std::string(method) + ": '" + key + "' が必要です", "assets 相対パスを渡す（例 materials/rock.dxmg）");
    std::string rel = p[key].get<std::string>();
    for (char& c : rel) if (c == '\\') c = '/';
    if (rel.rfind("assets/", 0) == 0) rel.erase(0, 7);
    if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos || (rel.size() > 1 && rel[1] == ':'))
        ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", std::string(method) + ": パスは assets 相対で指定してください: " + rel,
                "例: materials/rock.dxmg（絶対パスと .. は使えません）");
    return rel;
}

bool ReadText(const fs::path& p, std::string& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool EndsWith(const std::string& s, const char* suf)
{
    const std::string t = suf;
    if (s.size() < t.size()) return false;
    std::string a = s.substr(s.size() - t.size());
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a == t;
}

// path（.dxmg または .dxmat）→ グラフのパス。.dxmat のときは中身（材質）も返す。
struct GraphRef
{
    std::string graphRel;
    std::string dxmatRel;          // path が .dxmat のとき
    MaterialAssetData dxmat;
    bool hasDxmat = false;
};

GraphRef ResolveRef(const json& params, const char* method)
{
    GraphRef r;
    const std::string rel = CheckRel(params, "path", method);
    if (EndsWith(rel, ".dxmat"))
    {
        std::string text;
        if (!ReadText(AssetAbs(rel), text))
            ThrowMg(McpErr::NotFound, "E_NOT_FOUND", std::string(method) + ": 材質ファイルが無い: " + rel, "dx12_list_assets で確かめる", "assets 内にこの .dxmat が無い");
        if (!ParseMaterialAsset(std::vector<uint8_t>(text.begin(), text.end()), r.dxmat))
            ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", std::string(method) + ": 材質を解釈できません: " + rel, ".dxmat の JSON を確かめる");
        if (!r.dxmat.IsGraph())
            ThrowMg(McpErr::InvalidParam, "E_NOT_GRAPH", std::string(method) + ": " + rel + " は従来の材質（graph キー無し）です",
                    "material_graph_graphize {path} でグラフ材質へ変換できます（手動のみ。元ファイルは .bak に退避）");
        r.dxmatRel = rel;
        r.graphRel = r.dxmat.graphPath;
        r.hasDxmat = true;
        return r;
    }
    if (!EndsWith(rel, ".dxmg"))
        ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", std::string(method) + ": path は .dxmg か .dxmat です: " + rel, "例: materials/_graphs/pbr_std_n1m1e0.dxmg");
    r.graphRel = rel;
    return r;
}

bool LoadGraph(const std::string& graphRel, mg::MaterialGraph& g, std::string& err)
{
    std::string text;
    if (!ReadText(AssetAbs(graphRel), text)) { err = "ファイルを開けません: " + graphRel; return false; }
    return mg::LoadDxmg(text, g, &err);
}

// ---- JSON 変換 -----------------------------------------------------------------------------
json DiagJson(const mg::Diagnostic& d)
{
    json j = {{"severity", d.severity == mg::Severity::Error ? "error" : d.severity == mg::Severity::Warning ? "warning" : "info"},
              {"code", d.code}, {"message", d.message}};
    if (!d.nodeId.empty()) j["nodeId"] = d.nodeId;
    if (!d.pin.empty()) j["pin"] = d.pin;
    if (!d.hint.empty()) j["hint"] = d.hint;
    if (!d.reachable) j["reachable"] = false;
    if (d.line > 0) j["line"] = d.line;
    return j;
}

json DiagsJson(const std::vector<mg::Diagnostic>& v, bool includeDead = true)
{
    json a = json::array();
    for (const auto& d : v)
    {
        if (!includeDead && !d.reachable) continue;
        a.push_back(DiagJson(d));
    }
    return a;
}

json ValueJson(const mg::Value& v)
{
    switch (v.type)
    {
    case mg::ValueType::Bool: return v.b;
    case mg::ValueType::Int:  return v.i;
    case mg::ValueType::F1:   return v.f[0];
    default:
    {
        json a = json::array();
        for (int k = 0; k < mg::Dim(v.type); ++k) a.push_back(v.f[k]);
        return a;
    }
    }
}

// .dxmg の literal 記法（数値 / 配列 / bool）→ Value（GraphIO と同じ規則）
bool JsonToValue(const mg::PinDecl* pd, const json& v, mg::Value& out)
{
    if (v.is_boolean()) { out = mg::Value::Bool1(v.get<bool>()); return true; }
    if (v.is_number())
    {
        if (pd && pd->mode == mg::PinMode::Fixed && pd->type == mg::ValueType::Int) out = mg::Value::Int32(static_cast<int32_t>(std::llround(v.get<double>())));
        else out = mg::Value::Float(v.get<float>());
        return true;
    }
    if (v.is_array() && v.size() >= 2 && v.size() <= 4)
    {
        float f[4] = {0, 0, 0, 0};
        for (size_t k = 0; k < v.size(); ++k)
        {
            if (!v[k].is_number()) return false;
            f[k] = v[k].get<float>();
        }
        out = mg::Value::Vec(static_cast<int>(v.size()), f);
        return true;
    }
    return false;
}

json GraphJson(const mg::MaterialGraph& g)
{
    json nodes = json::object();
    for (const auto& [id, n] : g.Nodes())
    {
        json o = {{"type", n.type}, {"pos", {n.x, n.y}}};
        json in = json::object();
        for (const auto& [pin, b] : n.inputs)
        {
            if (b.link) in[pin] = b.link->node + "." + b.link->pin;
            else if (b.literal) in[pin] = ValueJson(*b.literal);
        }
        if (!in.empty()) o["in"] = std::move(in);
        json props = json::object();
        for (const auto& [k, v] : n.props) props[k] = v;
        if (!props.empty()) o["props"] = std::move(props);
        nodes[id] = std::move(o);
    }
    json comments = json::object();
    for (const auto& [id, c] : g.Comments())
        comments[id] = {{"text", c.text}, {"color", c.color}, {"rect", {c.x, c.y, c.w, c.h}}};
    const mg::GraphSettings& s = g.Settings();
    return {{"name", g.Name()}, {"guid", g.Guid()},
            {"settings", {{"blendMode", s.blendMode}, {"shadingModel", s.shadingModel}, {"twoSided", s.twoSided}, {"maskClip", s.maskClip}}},
            {"nodes", std::move(nodes)}, {"comments", std::move(comments)}};
}

json SlotsJson(const mg::CompileResult& cr)
{
    json a = json::array();
    for (const mg::SlotInfo& s : cr.slots)
    {
        const char* kind = s.kind == mg::SlotKind::Scalar ? "scalar" : s.kind == mg::SlotKind::Vector ? "vector" : s.kind == mg::SlotKind::Texture ? "texture"
                         : s.kind == mg::SlotKind::Constant ? "constant" : s.kind == mg::SlotKind::NodeProp ? "nodeProp" : "implicitTexture";
        json o = {{"slot", s.slot}, {"kind", kind}, {"type", mg::TypeName(s.type)}, {"nodeId", s.nodeId}};
        if (!s.name.empty()) o["name"] = s.name;
        if (!s.group.empty()) o["group"] = s.group;
        if (s.type == mg::ValueType::Tex2D) { o["texture"] = s.texturePath; o["usage"] = s.textureUsage; }
        else
        {
            json v = json::array();
            for (int k = 0; k < std::max(1, mg::Dim(s.type)); ++k) v.push_back(s.value[k]);
            o["value"] = v;
        }
        a.push_back(std::move(o));
    }
    return a;
}

json StatsJson(const mg::CompileResult& cr)
{
    return {{"nodes", cr.stats.nodesTotal}, {"reachable", cr.stats.nodesReachable}, {"ops", cr.stats.ops}, {"cseHits", cr.stats.cseHits},
            {"slots", cr.stats.slots}, {"textures", cr.stats.textures}, {"hlslLines", cr.stats.hlslLines}};
}

json StatusJson(const GraphMaterialSystem::InstanceStatus& s)
{
    return {{"dxmat", s.dxmat}, {"graph", s.graph}, {"hasActive", s.hasActive}, {"compiling", s.compiling}, {"failed", s.failed},
            {"activeHash", mg::Hex16(s.activeHash)}, {"pendingHash", mg::Hex16(s.pendingHash)},
            {"slotCount", s.slotCount}, {"textureCount", s.textureCount}, {"recordBase", s.recordBase},
            {"generation", s.generation}, {"valueUpdates", s.valueUpdates},
            {"errorLog", s.errorLog}, {"diagnostics", DiagsJson(s.diagnostics)},
            {"codegenMs", s.codegenMs}, {"dxcMs", s.dxcMs}, {"psoMs", s.psoMs}, {"cacheHit", s.cacheHit}};
}

json CountersJson(const GraphMaterialSystem::Counters& c)
{
    return {{"instances", c.instances}, {"variants", c.variants}, {"poolUsed", c.poolUsed}, {"poolCapacity", c.poolCapacity},
            {"codegen", c.codegen}, {"dxcCompiles", c.dxcCompiles}, {"cacheHits", c.cacheHits}, {"cacheMisses", c.cacheMisses},
            {"cacheStores", c.cacheStores}, {"psoCreates", c.psoCreates}, {"valueUpdates", c.valueUpdates},
            {"jobsQueued", c.jobsQueued}, {"jobsDone", c.jobsDone}, {"jobsFailed", c.jobsFailed}, {"fallbackDraws", c.fallbackDraws},
            {"cacheDir", c.cacheDir}};
}

// ---- 編集（ops）----------------------------------------------------------------------------
// 1 個の op を graph へ当てる。失敗したら false（err に日本語の理由）。out に結果（採番した id など）。
bool ParsePinRef(const std::string& s, std::string& node, std::string& pin)
{
    const size_t dot = s.find('.');
    node = dot == std::string::npos ? s : s.substr(0, dot);
    pin = dot == std::string::npos ? std::string() : s.substr(dot + 1);
    return !node.empty();
}

bool ApplyOp(mg::MaterialGraph& g, const json& op, std::string& err, json& out)
{
    if (!op.is_object() || !op.contains("op") || !op["op"].is_string()) { err = "op は {\"op\":\"add|remove|connect|disconnect|set|move|settings\", ...} の形で書く"; return false; }
    const std::string kind = op["op"].get<std::string>();
    auto str = [&](const char* k) { return op.contains(k) && op[k].is_string() ? op[k].get<std::string>() : std::string(); };

    if (kind == "add")
    {
        const std::string type = str("type");
        if (type.empty()) { err = "add: type が必要（material_graph_nodes で一覧）"; return false; }
        if (!g.Library().Find(type))
        {
            std::vector<std::string> names;
            for (const mg::NodeDef* d : g.Library().List()) names.push_back(d->type);
            std::string sug;
            for (const std::string& s : McpSuggest(type, names, 3)) sug += (sug.empty() ? "" : ", ") + s;
            err = "add: 知らないノード型: " + type + (sug.empty() ? "" : "（近い名前: " + sug + "）");
            return false;
        }
        float x = 0, y = 0;
        if (op.contains("pos") && op["pos"].is_array() && op["pos"].size() >= 2) { x = op["pos"][0].get<float>(); y = op["pos"][1].get<float>(); }
        const std::string wantId = str("id");
        if (!wantId.empty())
        {
            for (char c : wantId)
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) { err = "add: id は [A-Za-z0-9_] だけ: " + wantId; return false; }
            if (g.FindNode(wantId)) { err = "add: id が既にある: " + wantId; return false; }
        }
        const std::string id = g.AddNode(type, x, y, wantId);
        if (id.empty()) { err = "add: ノードを追加できない（id の衝突）"; return false; }
        if (op.contains("props") && op["props"].is_object())
            for (auto it = op["props"].begin(); it != op["props"].end(); ++it)
                if (!g.SetNodeProp(id, it.key(), it.value())) { err = "add: プロパティ " + it.key() + " の値が不正: " + it.value().dump(); return false; }
        if (op.contains("in") && op["in"].is_object())
            for (auto it = op["in"].begin(); it != op["in"].end(); ++it)
            {
                if (it.value().is_string())
                {
                    std::string fn, fp;
                    ParsePinRef(it.value().get<std::string>(), fn, fp);
                    const mg::Node* src = g.FindNode(fn);
                    if (!src) { err = "add: in." + it.key() + ": 接続元のノードが無い: " + fn + "（先に add してから connect する）"; return false; }
                    if (fp.empty())
                    {
                        const mg::NodeDef* sd = g.DefOf(fn);
                        fp = (sd && !sd->outputs.empty()) ? sd->outputs[0].name : "Out";
                    }
                    mg::ConnectCheck why;
                    if (!g.Connect(fn, fp, id, it.key(), &why)) { err = "add: in." + it.key() + ": 接続できません: " + why.message; return false; }
                }
                else
                {
                    const mg::NodeDef* d = g.DefOf(id);
                    mg::Value v;
                    if (!JsonToValue(d ? d->FindInput(it.key()) : nullptr, it.value(), v) || !g.SetLiteral(id, it.key(), v))
                    { err = "add: in." + it.key() + ": 値が不正: " + it.value().dump(); return false; }
                }
            }
        out["id"] = id;
        return true;
    }
    if (kind == "remove")
    {
        const std::string id = str("id");
        if (!g.FindNode(id)) { err = "remove: ノードが無い: " + id; return false; }
        if (!g.RemoveNode(id)) { err = "remove: 消せない: " + id; return false; }
        return true;
    }
    if (kind == "connect")
    {
        std::string fn, fp, tn, tp;
        if (!ParsePinRef(str("from"), fn, fp)) { err = "connect: from が必要（\"ノードID.出力ピン\"）"; return false; }
        if (!ParsePinRef(str("to"), tn, tp) || tp.empty()) { err = "connect: to が必要（\"ノードID.入力ピン\"）"; return false; }
        if (!g.FindNode(fn)) { err = "connect: from のノードが無い: " + fn; return false; }
        if (!g.FindNode(tn)) { err = "connect: to のノードが無い: " + tn; return false; }
        if (fp.empty())
        {
            const mg::NodeDef* sd = g.DefOf(fn);
            fp = (sd && !sd->outputs.empty()) ? sd->outputs[0].name : "Out";
        }
        mg::ConnectCheck why;
        if (!g.Connect(fn, fp, tn, tp, &why)) { err = "connect: " + fn + "." + fp + " → " + tn + "." + tp + " は接続できません: " + why.message; return false; }
        if (why.verdict == mg::ConnectCheck::Verdict::Warn) out["warning"] = why.message;
        return true;
    }
    if (kind == "disconnect")
    {
        std::string tn, tp;
        if (!ParsePinRef(str("to"), tn, tp) || tp.empty()) { err = "disconnect: to が必要（\"ノードID.入力ピン\"）"; return false; }
        if (!g.FindNode(tn)) { err = "disconnect: ノードが無い: " + tn; return false; }
        g.Disconnect(tn, tp);
        return true;
    }
    if (kind == "set")
    {
        const std::string id = str("id");
        if (!g.FindNode(id)) { err = "set: ノードが無い: " + id; return false; }
        if (!op.contains("value")) { err = "set: value が必要（消すときは null）"; return false; }
        if (op.contains("prop"))
        {
            const std::string prop = str("prop");
            if (!g.SetNodeProp(id, prop, op["value"])) { err = "set: プロパティ " + prop + " の値が不正: " + op["value"].dump(); return false; }
            return true;
        }
        if (op.contains("pin"))
        {
            const std::string pin = str("pin");
            if (op["value"].is_null()) { g.ClearLiteral(id, pin); return true; }
            const mg::NodeDef* d = g.DefOf(id);
            mg::Value v;
            if (!JsonToValue(d ? d->FindInput(pin) : nullptr, op["value"], v) || !g.SetLiteral(id, pin, v))
            { err = "set: 入力 " + pin + " に " + op["value"].dump() + " は入れられない"; return false; }
            return true;
        }
        err = "set: prop（プロパティ）か pin（入力ピンのリテラル）を指定する";
        return false;
    }
    if (kind == "move")
    {
        const std::string id = str("id");
        if (!g.FindNode(id)) { err = "move: ノードが無い: " + id; return false; }
        if (!op.contains("pos") || !op["pos"].is_array() || op["pos"].size() < 2) { err = "move: pos:[x,y] が必要"; return false; }
        g.MoveNode(id, op["pos"][0].get<float>(), op["pos"][1].get<float>());
        return true;
    }
    if (kind == "settings")
    {
        if (!op.contains("values") || !op["values"].is_object()) { err = "settings: values:{blendMode,shadingModel,twoSided,maskClip} が必要"; return false; }
        mg::GraphSettings s = g.Settings();
        const json& v = op["values"];
        if (v.contains("blendMode"))
        {
            const std::string b = v["blendMode"].is_string() ? v["blendMode"].get<std::string>() : "";
            if (b != "Opaque" && b != "Masked" && b != "Translucent" && b != "Additive") { err = "settings: blendMode は Opaque / Masked / Translucent / Additive"; return false; }
            s.blendMode = b;
        }
        if (v.contains("shadingModel") && v["shadingModel"].is_string()) s.shadingModel = v["shadingModel"].get<std::string>();
        if (v.contains("twoSided") && v["twoSided"].is_boolean()) s.twoSided = v["twoSided"].get<bool>();
        if (v.contains("maskClip") && v["maskClip"].is_number()) s.maskClip = v["maskClip"].get<float>();
        g.SetSettings(s);
        return true;
    }
    err = "知らない op: " + kind + "（有効: add, remove, connect, disconnect, set, move, settings）";
    return false;
}

// 複製に ops を全部当てる。失敗したら McpError（どの op か分かる）
json ApplyOps(mg::MaterialGraph& g, const json& ops, const char* method)
{
    json results = json::array();
    for (size_t i = 0; i < ops.size(); ++i)
    {
        std::string err;
        json out = json::object();
        if (!ApplyOp(g, ops[i], err, out))
        {
            const std::string opName = ops[i].is_object() && ops[i].contains("op") && ops[i]["op"].is_string() ? ops[i]["op"].get<std::string>() : "?";
            McpError e(McpErr::InvalidParam, std::string(method) + ": ops[" + std::to_string(i) + "] " + opName + ": " + err,
                       "エラー文の位置（ops[i]）を直して撃ち直す。1 個でも失敗するとファイルは 1 バイトも変わらない");
            e.name = "E_INVALID_OP";
            e.cause = err;
            e.details = {{"index", i}, {"op", ops[i]}};
            throw e;
        }
        out["index"] = i;
        results.push_back(std::move(out));
    }
    return results;
}

// 検証（解析 + コンパイル + エンジン側の警告）の結果を JSON にまとめる
json ValidateJson(const mg::MaterialGraph& g, bool includeHlsl, const MaterialAssetData* data)
{
    const mg::CompileResult cr = mg::CompileGraph(g);
    std::vector<mg::Diagnostic> diags = cr.diagnostics;
    for (auto& d : GraphMaterialSystem::CollectEngineDiagnostics(cr, data)) diags.push_back(std::move(d));
    int errors = 0, warnings = 0;
    for (const auto& d : diags)
    {
        if (!d.reachable) continue;
        if (d.severity == mg::Severity::Error) ++errors;
        else if (d.severity == mg::Severity::Warning) ++warnings;
    }
    json j = {{"ok", cr.ok}, {"errors", errors}, {"warnings", warnings}, {"hash", mg::Hex16(cr.hash)}, {"slotCount", cr.slotCount},
              {"stats", StatsJson(cr)}, {"diagnostics", DiagsJson(diags)}, {"slots", SlotsJson(cr)}};
    if (includeHlsl && cr.ok) j["hlsl"] = cr.hlsl;
    return j;
}

bool WriteBytesAtomic(const fs::path& p, const std::string& bytes, std::string& err)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    // 一時ファイル→flush→読み戻し検証→置き換え（core/AtomicFile.h）。失敗しても元のファイルは無傷
    const auto wr = dx12e::atomicfile::WriteFile(p, bytes);
    if (!wr) { err = "書き込みに失敗: " + p.string() + " (" + wr.error + ")"; return false; }
    return true;
}

} // namespace

void Application::RegisterMcpMatGraphMethods()
{
    // ------------------------------------------------------------------ material_graph_nodes
    {
        McpMeta m;
        m.summary   = "マテリアルグラフに置けるノード型の一覧（型 ID・カテゴリ・入力 / 出力ピン・プロパティ）。query で絞る。material_graph_edit の add で使う型名を引く";
        m.keywords  = "material graph node catalog マテリアル グラフ ノード 一覧 型 パレット search";
        m.category  = "material";
        m.effect    = McpEffect::Read;
        m.timeoutMs = 8000;
        m.idempotent = true;
        m.aliases   = {"dx12_material_graph_nodes"};
        m.params    = {P("query", "string", false, nullptr, nullptr, nullptr, nullptr, "表示名 / 型 / 検索語 / 説明の部分一致（例 texture, 乗算, fresnel）"),
                       P("detail", "enum", false, "summary|full", nullptr, nullptr, "\"summary\"", "summary = 型とカテゴリと説明 / full = ピンとプロパティも")};
        m.next      = {{"material_graph_edit", "add でノードを置く"}};
        McpDefine("material_graph_nodes", McpMeta(m), DX12E_MCP_HANDLER
            {
                const std::string q = params.value("query", std::string());
                const bool full = params.value("detail", std::string("summary")) == "full";
                const mg::NodeLibrary& lib = mg::NodeLibrary::Builtin();
                const auto list = q.empty() ? lib.List() : lib.Search(q);
                json arr = json::array();
                for (const mg::NodeDef* d : list)
                {
                    json o = {{"type", d->type}, {"category", d->category}, {"description", d->description}};
                    if (full)
                    {
                        json in = json::array(), out = json::array(), props = json::array();
                        for (const auto& p : d->inputs)
                        {
                            json pj = {{"name", p.name}, {"type", p.mode == mg::PinMode::Fixed ? mg::TypeName(p.type) : (p.mode == mg::PinMode::Poly ? "float(1-4)" : "any")}};
                            if (p.required) pj["required"] = true;
                            if (p.reserved) pj["reserved"] = true;
                            if (p.hasDefault) pj["default"] = ValueJson(p.defaultValue);
                            in.push_back(std::move(pj));
                        }
                        for (const auto& p : d->outputs)
                            out.push_back({{"name", p.name}, {"type", p.mode == mg::PinMode::Fixed ? mg::TypeName(p.type) : "float(1-4)"}});
                        for (const auto& p : d->props)
                        {
                            json pj = {{"name", p.name}, {"role", p.role == mg::PropRole::Code ? "code" : p.role == mg::PropRole::SlotValue ? "slot" : "meta"}, {"default", p.defaultValue}};
                            if (!p.enumValues.empty()) pj["values"] = p.enumValues;
                            props.push_back(std::move(pj));
                        }
                        o["inputs"] = std::move(in);
                        o["outputs"] = std::move(out);
                        o["props"] = std::move(props);
                    }
                    arr.push_back(std::move(o));
                }
                resp["ok"] = true;
                resp["result"] = {{"count", arr.size()}, {"nodes", std::move(arr)}};
            });
    }

    // ------------------------------------------------------------------ material_graph_get
    {
        McpMeta m;
        m.summary   = "グラフ（.dxmg）の取得: ノード・接続・設定・パラメータ一覧・診断を JSON で返す（material_graph_edit の入力と同じ語彙）。.dxmat を渡すとインスタンス（params）と親グラフを返す";
        m.keywords  = "material graph get describe dxmg dxmat マテリアル グラフ 取得 ノード 接続 パラメータ";
        m.category  = "material";
        m.effect    = McpEffect::Read;
        m.timeoutMs = 10000;
        m.idempotent = true;
        m.aliases   = {"dx12_material_graph_get"};
        m.params    = {P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "assets 相対。.dxmg（グラフ）または .dxmat（グラフのインスタンス）"),
                       P("detail", "enum", false, "summary|full", nullptr, nullptr, "\"full\"", "summary = 要約（ノード数・パラメータ・診断）/ full = ノードと接続の全部"),
                       P("hlsl", "bool", false, nullptr, nullptr, nullptr, "false", "true: 生成された HLSL の全文も返す"),
                       P("text", "bool", false, nullptr, nullptr, nullptr, "false", "true: 正準形の .dxmg テキストも返す")};
        m.next      = {{"material_graph_edit", "ops でグラフを編集する"}, {"material_graph_compile", "エンジンでのビルド状況"}};
        McpDefine("material_graph_get", McpMeta(m), DX12E_MCP_HANDLER
            {
                const GraphRef ref = ResolveRef(params, "material_graph_get");
                mg::MaterialGraph g;
                std::string err;
                if (!LoadGraph(ref.graphRel, g, err))
                    ThrowMg(McpErr::NotFound, "E_NOT_FOUND", "material_graph_get: グラフを読めません: " + ref.graphRel + "（" + err + "）",
                            "パスを確かめる（.dxmat の graph キーが指す先）", err);
                const bool full = params.value("detail", std::string("full")) == "full";
                json r = {{"path", ref.graphRel}};
                json gj = GraphJson(g);
                r["name"] = gj["name"];
                r["guid"] = gj["guid"];
                r["settings"] = gj["settings"];
                r["nodeCount"] = g.Nodes().size();
                if (full) { r["nodes"] = gj["nodes"]; r["comments"] = gj["comments"]; }
                json ps = json::array();
                for (const mg::ParamDecl& p : mg::CollectParameters(g))
                    ps.push_back({{"name", p.name}, {"nodeId", p.nodeId}, {"node", p.nodeType}, {"type", mg::TypeName(p.type)}, {"default", p.defaultValue},
                                  {"group", p.group}, {"reachable", p.reachable}, {"usage", p.usage}});
                r["parameters"] = std::move(ps);
                r["validation"] = ValidateJson(g, params.value("hlsl", false), ref.hasDxmat ? &ref.dxmat : nullptr);
                if (params.value("text", false)) r["text"] = mg::SaveDxmg(g, false);
                if (ref.hasDxmat)
                {
                    json pj = json::object();
                    for (const auto& [name, p] : ref.dxmat.graphParams)
                    {
                        if (p.kind == MaterialAssetData::GraphParam::Kind::Texture) pj[name] = p.texture;
                        else if (p.kind == MaterialAssetData::GraphParam::Kind::Scalar) pj[name] = p.v[0];
                        else { json a = json::array(); for (int i = 0; i < p.n; ++i) a.push_back(p.v[i]); pj[name] = a; }
                    }
                    r["instance"] = {{"path", ref.dxmatRel}, {"name", ref.dxmat.name}, {"params", std::move(pj)},
                                     {"uvTiling", {ref.dxmat.uvTilingU, ref.dxmat.uvTilingV}}};
                    GraphMaterialSystem::InstanceStatus st;
                    if (m_graphMaterials && m_graphMaterials->GetInstanceStatus(ref.dxmatRel, st)) r["engine"] = StatusJson(st);
                }
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
    }

    // ------------------------------------------------------------------ material_graph_validate
    {
        McpMeta m;
        m.summary   = "グラフの検証（型・接続・循環・未接続の必須入力・エンジン側の未対応設定）とコンパイル: 診断（nodeId + pin つき）・スロット表・統計を返す。何も書かず、エンジンの状態も変えない";
        m.keywords  = "material graph validate diagnostics compile check マテリアル グラフ 検証 診断 エラー";
        m.category  = "material";
        m.effect    = McpEffect::Read;
        m.timeoutMs = 10000;
        m.idempotent = true;
        m.aliases   = {"dx12_material_graph_validate"};
        m.params    = {P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, ".dxmg または .dxmat"),
                       P("hlsl", "bool", false, nullptr, nullptr, nullptr, "false", "true: 生成された HLSL の全文も返す")};
        m.next      = {{"material_graph_compile", "エンジンで DXC + PSO を作らせる"}};
        McpDefine("material_graph_validate", McpMeta(m), DX12E_MCP_HANDLER
            {
                const GraphRef ref = ResolveRef(params, "material_graph_validate");
                mg::MaterialGraph g;
                std::string err;
                if (!LoadGraph(ref.graphRel, g, err))
                {
                    resp["ok"] = true;
                    resp["result"] = {{"ok", false}, {"path", ref.graphRel}, {"errors", 1}, {"warnings", 0},
                                      {"diagnostics", json::array({{{"severity", "error"}, {"code", "E_GRAPH_LOAD"}, {"message", "グラフを読めません: " + err}}})}};
                    return;
                }
                json r = ValidateJson(g, params.value("hlsl", false), ref.hasDxmat ? &ref.dxmat : nullptr);
                r["path"] = ref.graphRel;
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
    }

    // ------------------------------------------------------------------ material_graph_edit
    {
        McpMeta m;
        m.summary   = "グラフを ops で差分編集して .dxmg に保存する（add / remove / connect / disconnect / set / move / settings）。全 op を複製へ当てて検証し、1 個でも失敗したら何も書かない。"
                      "値だけの編集（定数・パラメータの既定値・移動）は再コンパイルしない。dryRun で診断と HLSL の変化だけ返す";
        m.keywords  = "material graph edit ops add connect set node マテリアル グラフ 編集 ノード追加 接続 パラメータ設定";
        m.category  = "material";
        m.effect    = McpEffect::WriteFile;
        m.timeoutMs = 15000;
        m.dryRun    = "preview";
        m.journal   = true;
        m.aliases   = {"dx12_material_graph_edit"};
        m.params    = {P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "編集する .dxmg（.dxmat を渡すとその親グラフ）"),
                       P("ops", "array", true, nullptr, nullptr, nullptr, nullptr,
                         "操作の配列。{op:'add',type,id?,pos?,props?,in?} / {op:'remove',id} / {op:'connect',from:'id.pin',to:'id.pin'} / {op:'disconnect',to:'id.pin'} / "
                         "{op:'set',id,prop|pin,value} / {op:'move',id,pos} / {op:'settings',values:{blendMode,...}}"),
                       P("create", "bool", false, nullptr, nullptr, nullptr, "false", "true: .dxmg が無ければ空のグラフ（出力ノードだけ）から作る"),
                       P("save", "bool", false, nullptr, nullptr, nullptr, "true", "false: 検証だけで書かない（dryRun と同じ）")};
        m.next      = {{"material_graph_compile", "エンジンでのビルド状況を見る"}, {"material_graph_validate", "診断を読む"}};
        m.examples  = {{"{\"path\":\"materials/_graphs/pbr_std_n1m1e0.dxmg\",\"ops\":[{\"op\":\"set\",\"id\":\"pRough\",\"prop\":\"default\",\"value\":0.4}]}", "ラフネスの既定値を 0.4 に（HLSL は変わらない = 再コンパイル無し）"}};
        // 実体は preview と共有する（apply=false なら何も書かない）
        auto runEdit = [this](const json& params, bool apply) -> json
        {
            const std::string rel = CheckRel(params, "path", "material_graph_edit");
            std::string graphRel = rel;
            if (EndsWith(rel, ".dxmat")) graphRel = ResolveRef(params, "material_graph_edit").graphRel;
            else if (!EndsWith(rel, ".dxmg"))
                ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", "material_graph_edit: path は .dxmg か .dxmat です: " + rel, "例: materials/_graphs/x.dxmg");
            if (!params.contains("ops") || !params["ops"].is_array())
                ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", "material_graph_edit: ops（配列）が必要です", "例: ops:[{op:'set', id:'pRough', prop:'default', value:0.4}]");

            mg::MaterialGraph before;
            std::string err;
            const bool exists = LoadGraph(graphRel, before, err);
            mg::MaterialGraph g;
            if (exists) g = before.Clone();
            else if (params.value("create", false))
            {
                g.AddNode(mg::kOutputNodeType, 400.0f, 100.0f, "out");
                std::string nm = fs::path(graphRel).stem().string();
                g.SetName(nm);
            }
            else
                ThrowMg(McpErr::NotFound, "E_NOT_FOUND", "material_graph_edit: グラフを読めません: " + graphRel + "（" + err + "）",
                        "無ければ create:true で空のグラフから作る", err);

            const mg::CompileResult crBefore = exists ? mg::CompileGraph(before) : mg::CompileResult{};
            const json results = ApplyOps(g, params["ops"], "material_graph_edit");
            json validation = ValidateJson(g, false, nullptr);
            const mg::CompileResult crAfter = mg::CompileGraph(g);
            const bool structural = !exists || crAfter.hash != crBefore.hash;
            const std::string newText = mg::SaveDxmg(g, false);
            std::string oldText;
            if (exists) oldText = mg::SaveDxmg(before, false);
            const bool changed = newText != oldText;

            json r = {{"path", graphRel}, {"applied", results.size()}, {"results", results}, {"changed", changed},
                      {"structureChanged", structural}, {"recompileRequired", structural && changed},
                      {"validation", std::move(validation)}, {"newHash", mg::Hex16(crAfter.hash)}, {"oldHash", mg::Hex16(crBefore.hash)}};
            const bool wantSave = params.value("save", true);
            if (!apply || !wantSave)
            {
                r["wrote"] = false;
                r["wouldWrite"] = changed;
                return r;
            }
            bool wrote = false;
            if (changed)
            {
                mcpsafety::JournalBackup(AssetAbs(graphRel));
                std::string werr;
                if (!WriteBytesAtomic(AssetAbs(graphRel), newText, werr))
                    ThrowMg(McpErr::FileIo, "E_FILE_IO", "material_graph_edit: " + werr, "assets/ に書けるか確かめる（配布ゲーム(pak)では保存できない）", werr);
                wrote = true;
                if (m_graphMaterials) m_graphMaterials->NotifyGraphFileChanged(graphRel);
            }
            r["wrote"] = wrote;
            return r;
        };
        McpDefine("material_graph_edit", McpMeta(m), [this, runEdit](const json& params, json& resp, const std::string&, McpDeferred&, bool&, bool) -> void
            {
                resp["ok"] = true;
                resp["result"] = runEdit(params, /*apply=*/true);
            });
        mcpsafety::PreviewTable()["material_graph_edit"] = [runEdit](const std::string& argsJson) -> std::string
        {
            const json p = json::parse(argsJson, nullptr, false);
            if (!p.is_object()) return json{{"error", "args が JSON オブジェクトではない"}}.dump();
            try { return runEdit(p, /*apply=*/false).dump(); }
            catch (const McpError& e) { return json{{"error", e.what()}, {"hint", e.hint}}.dump(); }
        };
    }

    // ------------------------------------------------------------------ material_graph_compile
    {
        McpMeta m;
        m.summary   = "グラフ材質のビルドを（再）要求して、エンジンでの状況を返す: 描画に使っている HLSL のハッシュ・コンパイル中か（旧版で描画中）・失敗（DXC / PSO のエラー + nodeId 逆引き）・所要時間・キャッシュヒット。"
                      "材質がシーンで描かれて初めてビルドが始まる（未描画の材質は known:false）。force:true でキャッシュを使わず DXC からやり直す";
        m.keywords  = "material graph compile build status dxc pso cache マテリアル グラフ コンパイル 状態 キャッシュ 非同期";
        m.category  = "material";
        m.effect    = McpEffect::Runtime;
        m.timeoutMs = 10000;
        m.aliases   = {"dx12_material_graph_compile"};
        m.params    = {P("path", "assetPath", false, nullptr, nullptr, nullptr, nullptr, ".dxmat（そのインスタンス）または .dxmg（それを使う全インスタンス）。省略で全部"),
                       P("force", "bool", false, nullptr, nullptr, nullptr, "false", "true: ディスクキャッシュを使わず DXC からやり直す"),
                       P("rebuild", "bool", false, nullptr, nullptr, nullptr, "true", "false: 状況を読むだけ（material_graph_status と同じ）")};
        m.next      = {{"step_frames", "数フレーム進めるとビルドが進む（ワーカーで非同期）"}, {"material_graph_status", "状況だけ読む"}};
        McpDefine("material_graph_compile", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_graphMaterials || !m_graphMaterials->IsAvailable())
                    ThrowMg(McpErr::Unsupported, "E_UNSUPPORTED", "material_graph_compile: マテリアルグラフが使えません",
                            "バインドレス（SM 6.6 + Resource Binding Tier 3）非対応、または DX12_DISABLE_MAIN_BINDLESS=1。従来の材質（.dxmat の 4 枚）で描かれます");
                const bool force = params.value("force", false);
                const bool rebuild = params.value("rebuild", true);
                std::string graphRel, dxmatRel;
                if (params.contains("path"))
                {
                    const std::string rel = CheckRel(params, "path", "material_graph_compile");
                    if (EndsWith(rel, ".dxmat")) dxmatRel = rel; else graphRel = rel;
                }
                json arr = json::array();
                bool any = false;
                for (const auto& st : m_graphMaterials->ListInstances())
                {
                    auto norm = [](std::string s) { for (char& c : s) { if (c == '\\') c = '/'; c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); } return s; };
                    if (!dxmatRel.empty() && norm(dxmatRel) != st.dxmat) continue;
                    if (!graphRel.empty() && norm(graphRel) != norm(st.graph)) continue;
                    any = true;
                    if (rebuild) m_graphMaterials->Rebuild(st.dxmat, force);
                    arr.push_back(StatusJson(st));
                }
                json r = {{"known", any}, {"rebuildRequested", any && rebuild}, {"instances", std::move(arr)}, {"counters", CountersJson(m_graphMaterials->GetCounters())}};
                if (!any)
                    r["note"] = "この材質はまだ描画されていない（インスタンスが無い）。材質を割り当てたエンティティが視界に入ると、次のフレームにビルドが始まる（material_graph_apply → step_frames）";
                if (any && rebuild)
                    r["note"] = "再ビルドを要求した。HLSL が変わらなければ値の更新だけ（再コンパイル無し）。数フレーム進めてから material_graph_status で結果を読む";
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
    }

    // ------------------------------------------------------------------ material_graph_status
    {
        McpMeta m;
        m.summary   = "グラフ材質のランタイム状況: インスタンスごとのビルド状態（旧版で描画中か・失敗か・所要時間）と、カウンタ（DXC を呼んだ回数・キャッシュヒット・PSO 作成数・パラメータプールの使用量・再コンパイル無しの値更新回数）";
        m.keywords  = "material graph status counters cache pool マテリアル グラフ 状況 カウンタ";
        m.category  = "material";
        m.effect    = McpEffect::Read;
        m.timeoutMs = 8000;
        m.idempotent = true;
        m.aliases   = {"dx12_material_graph_status"};
        m.params    = {P("path", "assetPath", false, nullptr, nullptr, nullptr, nullptr, "絞り込む .dxmat（省略で全部）")};
        McpDefine("material_graph_status", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_graphMaterials)
                    ThrowMg(McpErr::Internal, "E_INTERNAL", "material_graph_status: グラフ材質のランタイムが作られていない", "エンジンの起動が終わっていない");
                std::string only;
                if (params.contains("path")) only = CheckRel(params, "path", "material_graph_status");
                for (char& c : only) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                json arr = json::array();
                for (const auto& st : m_graphMaterials->ListInstances())
                    if (only.empty() || st.dxmat == only) arr.push_back(StatusJson(st));
                resp["ok"] = true;
                resp["result"] = {{"available", m_graphMaterials->IsAvailable()}, {"instances", std::move(arr)}, {"counters", CountersJson(m_graphMaterials->GetCounters())}};
            });
    }

    // ------------------------------------------------------------------ material_graph_panel
    {
        McpMeta m;
        m.summary   = "マテリアルグラフ窓（エディタ）を操作・観測する（G2c）: 開く / 新規 / グラフの編集（Undo 可）/ プレビューの形状・環境・カメラ / プレビューの撮影 / 編集 → 見た目更新の実測ベンチ / 状態"
                      "（デバウンス・生成中・高速版 → 最適化版・DXC 逆引きのエラー・ノード内サムネイルの統計・フレーム時間）。窓が閉じていれば開く。エディタ専用（GameRuntime では使えない）";
        m.keywords  = "material graph panel preview thumbnail latency bench マテリアル グラフ 窓 プレビュー サムネイル 実測";
        m.category  = "material";
        m.effect    = McpEffect::Runtime;
        m.timeoutMs = 10000;
        m.aliases   = {"dx12_material_graph_panel"};
        m.params    = {P("op", "enum", false, "status|open|new|edit|preview|screenshot|bench", nullptr, nullptr, "\"status\"", "status = 状態 / open = .dxmg を開く / new = 新規 / edit = ops で編集 / preview = 形状・環境・カメラ / screenshot = プレビューを PNG（linear:true で PFM）/ bench = 連続編集の実測"),
                       P("path", "string", false, nullptr, nullptr, nullptr, nullptr, "open: assets 相対の .dxmg / screenshot: 出力先（絶対パス）"),
                       P("template", "enum", false, "sample|pbr|empty|big", nullptr, nullptr, "\"sample\"", "new: ひな形（big = 約 200 ノードの実測用）"),
                       P("shape", "enum", false, "sphere|plane|cylinder|cube", nullptr, nullptr, nullptr, "preview: 形状"),
                       P("env", "enum", false, "studio|outdoor|dark", nullptr, nullptr, nullptr, "preview: 環境"),
                       P("yaw", "number", false, nullptr, nullptr, nullptr, nullptr, "preview: オブジェクトの回転（ラジアン）"),
                       P("pitch", "number", false, nullptr, nullptr, nullptr, nullptr, "preview: オブジェクトの傾き（ラジアン）"),
                       P("dist", "number", false, nullptr, "1.8", "8", nullptr, "preview: カメラ距離"),
                       P("lightYaw", "number", false, nullptr, nullptr, nullptr, nullptr, "preview: ライトの向き（ラジアン）"),
                       P("lightPitch", "number", false, nullptr, nullptr, nullptr, nullptr, "preview: ライトの高さ（ラジアン）"),
                       P("autoRotate", "bool", false, nullptr, nullptr, nullptr, nullptr, "preview: 自動回転"),
                       P("nodeThumbs", "bool", false, nullptr, nullptr, nullptr, nullptr, "preview: ノード内サムネイルの表示"),
                       P("linear", "bool", false, nullptr, nullptr, nullptr, "false", "screenshot: true でリニア HDR（PFM。ポスト前・露出前）"),
                       P("ops", "array", false, nullptr, nullptr, nullptr, nullptr, "edit: [{op:'set', id, prop, value} / {op:'literal', id, pin, value} / {op:'connect', from:'id.pin', to:'id.pin'} / {op:'disconnect', to:'id.pin'} / {op:'add', type, id, pos:[x,y]} / {op:'undo'} / {op:'redo'}]"),
                       P("edits", "int", false, nullptr, "1", "200", "12", "bench: 編集の回数"),
                       P("intervalMs", "int", false, nullptr, "50", "5000", "400", "bench: 編集の間隔（ms）"),
                       P("kind", "enum", false, "structure|value", nullptr, nullptr, "\"structure\"", "bench: structure = HLSL が変わる編集 / value = 値のみの変更"),
                       P("nodes", "int", false, nullptr, nullptr, nullptr, nullptr, "new(big): 目標ノード数（既定 200）")};
        m.next      = {{"step_frames", "数フレーム進めてから status で読む"}, {"imgui_screenshot", "窓全体の見た目（仮想入力で撮る）"}};
        McpDefine("material_graph_panel", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_editorCtx)
                    ThrowMg(McpErr::Unsupported, "E_UNSUPPORTED", "material_graph_panel: エディタの窓がありません（ゲームモード）", "エディタで開いたプロジェクトから使う");
                const std::string out = MaterialGraphPanel::DebugCall(*m_editorCtx, params.dump());
                json r = json::parse(out, nullptr, false);
                if (r.is_discarded()) r = json{{"raw", out}};
                resp["ok"] = r.value("ok", true);
                resp["result"] = std::move(r);
            });
    }

    // ------------------------------------------------------------------ material_graph_graphize
    {
        McpMeta m;
        m.summary   = "従来の PBR 材質（.dxmat の 4 枚テクスチャ + 係数）を、等価な標準テンプレートグラフのインスタンス（graph + params）へ変換する。手動のみ（自動変換は無い）。"
                      "元ファイルは <name>.dxmat.bak に退避（既にあれば守る）。構造が同じ材質は同じテンプレート = 同じ PSO を共有。dryRun で変換後の中身だけ返す";
        m.keywords  = "material graph graphize convert dxmat pbr グラフ化 変換 マテリアル テンプレート";
        m.category  = "material";
        m.effect    = McpEffect::WriteFile;
        m.timeoutMs = 15000;
        m.dryRun    = "preview";
        m.journal   = true;
        m.aliases   = {"dx12_material_graph_graphize"};
        m.params    = {P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "変換する従来の .dxmat（assets 相対）")};
        m.next      = {{"material_graph_apply", "エンティティへ割り当てて見る"}, {"material_graph_get", "変換後のグラフを見る"}};
        auto runGraphize = [this](const json& params, bool apply) -> json
        {
            const std::string rel = CheckRel(params, "path", "material_graph_graphize");
            if (!EndsWith(rel, ".dxmat"))
                ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", "material_graph_graphize: path は .dxmat です: " + rel, "従来の材質（assets/materials/*.dxmat）を指定する");
            if (apply)
            {
                // 書く前に元の内容をジャーナルへ（journal_restore で戻せる）
                mcpsafety::JournalBackup(AssetAbs(rel));
                mcpsafety::JournalBackup(AssetAbs(rel + ".bak"));
            }
            const GraphizeFileResult fr = GraphizeMaterialFile(PathResolver::AssetsDir(), rel, /*dryRun=*/!apply);
            if (!fr.conv.ok)
                ThrowMg(McpErr::InvalidParam, "E_GRAPHIZE", "material_graph_graphize: " + fr.conv.error, "従来の材質（graph キーの無い .dxmat）だけが対象");
            json params2 = json::object();
            for (const auto& [name, p] : fr.conv.instance.graphParams)
            {
                if (p.kind == MaterialAssetData::GraphParam::Kind::Texture) params2[name] = p.texture;
                else if (p.kind == MaterialAssetData::GraphParam::Kind::Scalar) params2[name] = p.v[0];
                else { json a = json::array(); for (int i = 0; i < p.n; ++i) a.push_back(p.v[i]); params2[name] = a; }
            }
            json r = {{"path", rel}, {"template", fr.conv.templateRel}, {"params", std::move(params2)},
                      {"wroteTemplate", fr.wroteTemplate}, {"wroteBackup", fr.wroteBackup}, {"wroteInstance", fr.wroteInstance},
                      {"backup", rel + ".bak"}};
            if (!apply) { r["instanceText"] = fr.conv.instanceText; r["wouldWrite"] = {rel, rel + ".bak", fr.conv.templateRel}; }
            else if (m_materialAssetManager)
                m_materialAssetManager->Invalidate(rel);   // 次の描画で新しい内容を読み直す
            return r;
        };
        McpDefine("material_graph_graphize", McpMeta(m), [this, runGraphize](const json& params, json& resp, const std::string&, McpDeferred&, bool&, bool) -> void
            {
                resp["ok"] = true;
                resp["result"] = runGraphize(params, /*apply=*/true);
            });
        mcpsafety::PreviewTable()["material_graph_graphize"] = [runGraphize](const std::string& argsJson) -> std::string
        {
            const json p = json::parse(argsJson, nullptr, false);
            if (!p.is_object()) return json{{"error", "args が JSON オブジェクトではない"}}.dump();
            try { return runGraphize(p, /*apply=*/false).dump(); }
            catch (const McpError& e) { return json{{"error", e.what()}, {"hint", e.hint}}.dump(); }
        };
    }

    // ------------------------------------------------------------------ material_graph_set_param
    {
        McpMeta m;
        m.summary   = "グラフ材質（.dxmat）のパラメータ上書きを書き換える。再コンパイルしない（パラメータプールのレコードを書き直すだけ）。数値 = Scalar / 配列 = Vector / 文字列 = テクスチャのパス。"
                      "save:false でファイルを書かずメモリ上だけ変える。value:null で上書きを消す（グラフの既定値へ戻る）。dryRun あり";
        m.keywords  = "material graph parameter param set override instance マテリアル グラフ パラメータ 設定 インスタンス 上書き";
        m.category  = "material";
        m.effect    = McpEffect::WriteFile;
        m.timeoutMs = 10000;
        m.dryRun    = "preview";
        m.journal   = true;
        m.aliases   = {"dx12_set_material_param"};
        m.params    = {P("path", "assetPath", true, nullptr, nullptr, nullptr, nullptr, "グラフ材質の .dxmat"),
                       P("name", "string", true, nullptr, nullptr, nullptr, nullptr, "パラメータ名（グラフの Scalar / Vector / TextureParameter の name。大文字小文字を区別）"),
                       P("value", "any", true, nullptr, nullptr, nullptr, nullptr, "数値 / [r,g,b(,a)] の配列 / テクスチャの assets 相対パス。null で上書きを消す"),
                       P("save", "bool", false, nullptr, nullptr, nullptr, "true", "false: ファイルへ書かない（メモリ上の材質だけ。保存されない）")};
        m.next      = {{"material_graph_status", "step_frames の後に generation が増えていない（再コンパイル無し）ことを確かめる"}};
        m.examples  = {{"{\"path\":\"materials/rock.dxmat\",\"name\":\"Roughness\",\"value\":0.3}", "ラフネスだけ書き換える"}};
        auto runSet = [this](const json& params, bool apply) -> json
        {
            const GraphRef ref = ResolveRef(params, "material_graph_set_param");
            const std::string name = params.value("name", std::string());
            if (name.empty()) ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", "material_graph_set_param: name が必要です", "material_graph_get の parameters に名前が出る");
            if (!params.contains("value")) ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", "material_graph_set_param: value が必要です", "数値 / 配列 / テクスチャのパス（消すときは null）");
            const json& v = params["value"];
            const bool remove = v.is_null();
            MaterialAssetData::GraphParam gp;
            if (!remove)
            {
                if (v.is_number()) { gp.kind = MaterialAssetData::GraphParam::Kind::Scalar; gp.n = 1; gp.v[0] = v.get<float>(); }
                else if (v.is_array() && v.size() >= 2 && v.size() <= 4)
                {
                    gp.kind = MaterialAssetData::GraphParam::Kind::Vector;
                    gp.n = static_cast<int>(v.size());
                    for (size_t i = 0; i < v.size(); ++i)
                    {
                        if (!v[i].is_number()) ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", "material_graph_set_param: 配列は数値だけ", "例: [1, 0.5, 0.2]");
                        gp.v[i] = v[i].get<float>();
                    }
                }
                else if (v.is_string()) { gp.kind = MaterialAssetData::GraphParam::Kind::Texture; gp.texture = v.get<std::string>(); }
                else ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", "material_graph_set_param: value は 数値 / 配列(2〜4) / 文字列 / null", "例: 0.3 / [1,0.5,0.2] / \"textures/x.png\"");
            }
            // グラフに無い名前は警告（黙って無効にしない）
            json warnings = json::array();
            mg::MaterialGraph g;
            std::string err;
            bool known = false;
            if (LoadGraph(ref.graphRel, g, err))
            {
                for (const mg::ParamDecl& p : mg::CollectParameters(g)) if (p.name == name) known = true;
                if (!known) warnings.push_back("グラフにパラメータ「" + name + "」が無い（上書きは無視される）。material_graph_get の parameters を確かめる");
            }
            json r = {{"path", ref.dxmatRel}, {"name", name}, {"removed", remove}, {"knownParameter", known}, {"warnings", warnings}};
            GraphMaterialSystem::InstanceStatus st;
            const bool haveInst = m_graphMaterials && m_graphMaterials->GetInstanceStatus(ref.dxmatRel, st);
            if (haveInst) r["before"] = {{"generation", st.generation}, {"valueUpdates", st.valueUpdates}};
            r["recompile"] = false;   // 値の更新は再コンパイルしない（G1 の保証）
            if (!apply) { r["wouldWrite"] = params.value("save", true); return r; }

            MaterialAssetData data = ref.dxmat;
            if (remove) data.graphParams.erase(name); else data.graphParams[name] = gp;
            const bool save = params.value("save", true);
            if (save)
            {
                mcpsafety::JournalBackup(AssetAbs(ref.dxmatRel));
                std::string werr;
                if (!WriteBytesAtomic(AssetAbs(ref.dxmatRel), SerializeMaterialAsset(data), werr))
                    ThrowMg(McpErr::FileIo, "E_FILE_IO", "material_graph_set_param: " + werr, "assets/ に書けるか確かめる（配布ゲーム(pak)では保存できない）", werr);
            }
            bool live = false;
            if (m_materialAssetManager)
            {
                // 読み込み済みならメモリ上の Entry を直接更新（loadSerial が進み、次のフレームでレコードだけ書き直す）。
                // 未ロードなら次の GetOrLoad がファイルから読む（save:true のとき）。
                live = m_materialAssetManager->SetGraphParam(ref.dxmatRel, name, gp, remove);
            }
            r["live"] = live;
            r["saved"] = save;
            return r;
        };
        McpDefine("material_graph_set_param", McpMeta(m), [this, runSet](const json& params, json& resp, const std::string&, McpDeferred&, bool&, bool) -> void
            {
                resp["ok"] = true;
                resp["result"] = runSet(params, /*apply=*/true);
            });
        mcpsafety::PreviewTable()["material_graph_set_param"] = [runSet](const std::string& argsJson) -> std::string
        {
            const json p = json::parse(argsJson, nullptr, false);
            if (!p.is_object()) return json{{"error", "args が JSON オブジェクトではない"}}.dump();
            try { return runSet(p, /*apply=*/false).dump(); }
            catch (const McpError& e) { return json{{"error", e.what()}, {"hint", e.hint}}.dump(); }
        };
    }

    // ------------------------------------------------------------------ material_graph_apply
    {
        McpMeta m;
        m.summary   = "エンティティ（のサブメッシュ）へ材質（.dxmat。グラフ材質でも従来の材質でも）を割り当てる。path:\"\" で解除。Undo できる";
        m.keywords  = "material graph apply assign entity submesh dxmat マテリアル 割り当て エンティティ";
        m.category  = "material";
        m.effect    = McpEffect::WriteScene;
        m.timeoutMs = 10000;
        m.aliases   = {"dx12_material_graph_apply"};
        m.params    = {P("entity", "entityRef", false, nullptr, nullptr, nullptr, nullptr, "エンティティ id(int)。name と排他。"),
                       P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名(完全一致)。"),
                       P("path", "string", true, nullptr, nullptr, nullptr, nullptr, "assets 相対の .dxmat。空文字で割り当て解除"),
                       P("submesh", "int", false, nullptr, "0", "64", "0", "サブメッシュ番号")};
        m.next      = {{"step_frames", "数フレーム進めるとグラフのビルドが始まる"}, {"material_graph_status", "ビルド状況"}};
        McpDefine("material_graph_apply", McpMeta(m), DX12E_MCP_HANDLER
            {
                const auto e = ResolveMcpEntity(*m_scene, params);
                auto& reg = m_scene->GetRegistry();
                if (!reg.all_of<MeshRenderer>(e))
                    throw McpError(McpErr::NotFound, "entity has no MeshRenderer", "材質はメッシュ（モデル / プリミティブ）にだけ割り当てられる");
                std::string rel = params.value("path", std::string());
                for (char& c : rel) if (c == '\\') c = '/';
                if (rel.rfind("assets/", 0) == 0) rel.erase(0, 7);
                if (!rel.empty())
                {
                    if (rel.find("..") != std::string::npos || rel[0] == '/' || !EndsWith(rel, ".dxmat"))
                        ThrowMg(McpErr::InvalidParam, "E_INVALID_PARAM", "material_graph_apply: path は assets 相対の .dxmat です: " + rel, "例: materials/rock.dxmat");
                    if (!vfs::Exists(rel))
                        ThrowMg(McpErr::NotFound, "E_NOT_FOUND", "material_graph_apply: 材質が無い: " + rel, "dx12_list_assets で確かめる");
                }
                const u32 sub = static_cast<u32>(params.value("submesh", 0));
                McpUndo().Track<MeshRenderer>(e);
                auto& mr = reg.get<MeshRenderer>(e);
                if (mr.materialAsset.size() <= sub) mr.materialAsset.resize(static_cast<size_t>(sub) + 1);
                mr.materialAsset[sub] = rel;
                bool anySet = false;
                for (const auto& s : mr.materialAsset) anySet = anySet || !s.empty();
                if (!anySet) mr.materialAsset.clear();
                resp["ok"] = true;
                resp["result"] = {{"entity", static_cast<uint32_t>(e)}, {"submesh", sub}, {"path", rel}, {"cleared", rel.empty()}};
            });
    }
}

} // namespace dx12e
