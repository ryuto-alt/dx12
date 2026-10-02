#include "renderer/matgraph/GraphIO.h"
#include "core/AtomicFile.h"   // 原子的な保存（標準ライブラリ + Win32 だけ。MatGraph は AtomicFile.cpp を自前で持つ）

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace dx12e::matgraph
{
namespace
{

std::string JsonStr(const std::string& s) { return Json(s).dump(-1, ' ', false); }

// float → JSON の double（0.6f が 0.6000000238… にならないよう、最短表記経由）
double FloatToJsonDouble(float f) { return std::strtod(FormatFloatShort(f).c_str(), nullptr); }

// 1 行の JSON（キーは辞書順）
void Inline(const Json& j, std::string& out)
{
    switch (j.type())
    {
    case Json::value_t::object:
    {
        if (j.empty()) { out += "{}"; return; }
        out += "{ ";
        bool first = true;
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            if (!first) out += ", ";
            first = false;
            out += JsonStr(it.key());
            out += ": ";
            Inline(it.value(), out);
        }
        out += " }";
        return;
    }
    case Json::value_t::array:
    {
        out += "[";
        for (size_t k = 0; k < j.size(); ++k)
        {
            if (k) out += ", ";
            Inline(j[k], out);
        }
        out += "]";
        return;
    }
    case Json::value_t::string:          out += JsonStr(j.get<std::string>()); return;
    case Json::value_t::boolean:         out += j.get<bool>() ? "true" : "false"; return;
    case Json::value_t::number_integer:  out += std::to_string(j.get<int64_t>()); return;
    case Json::value_t::number_unsigned: out += std::to_string(j.get<uint64_t>()); return;
    case Json::value_t::number_float:    out += FormatJsonDouble(j.get<double>()); return;
    default:                             out += "null"; return;
    }
}

Json ValueToJson(const Value& v)
{
    switch (v.type)
    {
    case ValueType::Bool: return Json(v.b);
    case ValueType::Int:  return Json(static_cast<int64_t>(v.i));
    case ValueType::F1:   return Json(FloatToJsonDouble(v.f[0]));
    default:
    {
        Json a = Json::array();
        for (int k = 0; k < Dim(v.type); ++k) a.push_back(FloatToJsonDouble(v.f[k]));
        return a;
    }
    }
}

std::string ShortPair(float a, float b) { return FormatFloatShort(a) + " " + FormatFloatShort(b); }

bool ParseFloats(const std::string& s, float* out, int n)
{
    const char* p = s.c_str();
    for (int k = 0; k < n; ++k)
    {
        char* end = nullptr;
        out[k] = std::strtof(p, &end);
        if (end == p) return false;
        p = end;
    }
    return true;
}

} // namespace

std::string FormatJsonDouble(double d)
{
    if (std::isnan(d)) d = 0.0;
    if (std::isinf(d)) d = d > 0 ? 1.7976931348623157e308 : -1.7976931348623157e308;
    if (d == 0.0) d = 0.0;
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), d);
    std::string s(buf, res.ptr);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------
std::string SaveDxmg(const MaterialGraph& g, bool includeView)
{
    const bool wantView = includeView && g.View().valid;
    std::string o;
    o += "{\n";
    o += "  \"version\": " + std::to_string(kDxmgVersion) + ",\n";
    o += "  \"kind\": \"material\",\n";
    o += "  \"guid\": " + JsonStr(g.Guid()) + ",\n";
    o += "  \"name\": " + JsonStr(g.Name()) + ",\n";

    {
        const GraphSettings& s = g.Settings();
        Json j = Json::object();
        j["blendMode"] = s.blendMode;
        j["shadingModel"] = s.shadingModel;
        j["twoSided"] = s.twoSided;
        j["maskClip"] = FloatToJsonDouble(s.maskClip);
        o += "  \"settings\": ";
        Inline(j, o);
        o += ",\n";
    }

    // nodes
    if (g.Nodes().empty())
        o += "  \"nodes\": {},\n";
    else
    {
        o += "  \"nodes\": {\n";
        size_t idx = 0;
        for (const auto& kv : g.Nodes())
        {
            const Node& n = kv.second;
            std::string line = "{ \"type\": " + JsonStr(n.type);
            Json in = Json::object();
            for (const auto& ik : n.inputs)
            {
                if (ik.second.link) in[ik.first] = ik.second.link->node + "." + ik.second.link->pin;
                else if (ik.second.literal) in[ik.first] = ValueToJson(*ik.second.literal);
            }
            if (!in.empty()) { line += ", \"in\": "; Inline(in, line); }
            if (!n.props.empty())
            {
                Json pj = Json::object();
                for (const auto& pk : n.props) pj[pk.first] = pk.second;
                line += ", \"props\": ";
                Inline(pj, line);
            }
            line += " }";
            o += "    " + JsonStr(kv.first) + ": " + line;
            o += (++idx < g.Nodes().size()) ? ",\n" : "\n";
        }
        o += "  },\n";
    }

    // comments
    if (g.Comments().empty())
        o += "  \"comments\": {},\n";
    else
    {
        o += "  \"comments\": {\n";
        size_t idx = 0;
        for (const auto& kv : g.Comments())
        {
            const Comment& c = kv.second;
            Json j = Json::object();
            j["color"] = c.color;
            j["rect"] = FormatFloatShort(c.x) + " " + FormatFloatShort(c.y) + " " + FormatFloatShort(c.w) + " " + FormatFloatShort(c.h);
            j["text"] = c.text;
            std::string line;
            Inline(j, line);
            o += "    " + JsonStr(kv.first) + ": " + line;
            o += (++idx < g.Comments().size()) ? ",\n" : "\n";
        }
        o += "  },\n";
    }

    // layout
    if (g.Nodes().empty())
        o += wantView ? "  \"layout\": {},\n" : "  \"layout\": {}\n";
    else
    {
        o += "  \"layout\": {\n";
        size_t idx = 0;
        for (const auto& kv : g.Nodes())
        {
            o += "    " + JsonStr(kv.first) + ": " + JsonStr(ShortPair(kv.second.x, kv.second.y));
            o += (++idx < g.Nodes().size()) ? ",\n" : "\n";
        }
        o += wantView ? "  },\n" : "  }\n";
    }
    // view（エディタのパン・ズーム。UI 状態。無ければ書かない = 既存ファイルのバイト列は変わらない）
    if (wantView)
    {
        const ViewInfo& v = g.View();
        o += "  \"view\": " + JsonStr(FormatFloatShort(v.panX) + " " + FormatFloatShort(v.panY) + " " + FormatFloatShort(v.zoom)) + "\n";
    }
    o += "}\n";
    return o;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
bool LoadDxmg(const std::string& text, MaterialGraph& out, std::string* error)
{
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    const Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("JSON として読めません");
    if (!j.contains("version") || !j["version"].is_number_integer()) return fail("version がありません");
    const int version = j["version"].get<int>();
    if (version > kDxmgVersion)
        return fail("このエンジンより新しい .dxmg（version " + std::to_string(version) + "）です。エンジンを更新してください");
    if (version < 1) return fail("version が不正です");
    if (j.contains("kind") && j["kind"].is_string() && j["kind"].get<std::string>() != "material")
        return fail("kind \"" + j["kind"].get<std::string>() + "\" は未対応です（material のみ）");

    out.Clear();
    if (j.contains("guid") && j["guid"].is_string()) out.SetGuid(j["guid"].get<std::string>());
    if (j.contains("name") && j["name"].is_string()) out.SetName(j["name"].get<std::string>());

    GraphSettings st;
    if (j.contains("settings") && j["settings"].is_object())
    {
        const Json& s = j["settings"];
        if (s.contains("blendMode") && s["blendMode"].is_string()) st.blendMode = s["blendMode"].get<std::string>();
        if (s.contains("shadingModel") && s["shadingModel"].is_string()) st.shadingModel = s["shadingModel"].get<std::string>();
        if (s.contains("twoSided") && s["twoSided"].is_boolean()) st.twoSided = s["twoSided"].get<bool>();
        if (s.contains("maskClip") && s["maskClip"].is_number()) st.maskClip = s["maskClip"].get<float>();
    }
    out.SetSettings(st);

    const NodeLibrary& lib = out.Library();

    std::map<NodeId, Node> nodes;
    if (j.contains("nodes"))
    {
        if (!j["nodes"].is_object()) return fail("nodes がオブジェクトではありません");
        for (auto it = j["nodes"].begin(); it != j["nodes"].end(); ++it)
        {
            const std::string id = it.key();
            const Json& e = it.value();
            if (id.empty() || id.find('.') != std::string::npos) return fail("ノード ID \"" + id + "\" が不正です");
            if (!e.is_object() || !e.contains("type") || !e["type"].is_string()) return fail("ノード " + id + " に type がありません");
            Node n;
            n.id = id;
            n.type = e["type"].get<std::string>();
            const NodeDef* def = lib.Find(n.type);
            if (e.contains("props"))
            {
                if (!e["props"].is_object()) return fail("ノード " + id + " の props がオブジェクトではありません");
                for (auto pit = e["props"].begin(); pit != e["props"].end(); ++pit)
                {
                    const PropDecl* pd = def ? def->FindProp(pit.key()) : nullptr;
                    Json norm;
                    if (pd && NormalizeProp(*pd, pit.value(), norm)) n.props[pit.key()] = std::move(norm);
                    else n.props[pit.key()] = pit.value();   // 未知 / 不正はそのまま保持（診断で報告される）
                }
            }
            if (e.contains("in"))
            {
                if (!e["in"].is_object()) return fail("ノード " + id + " の in がオブジェクトではありません");
                for (auto iit = e["in"].begin(); iit != e["in"].end(); ++iit)
                {
                    const std::string pin = iit.key();
                    const Json& v = iit.value();
                    const PinDecl* pd = def ? def->FindInput(pin) : nullptr;
                    InputBinding b;
                    if (v.is_string())
                    {
                        // "node.pin"（pin 省略は後で先頭の出力へ補う）
                        const std::string s = v.get<std::string>();
                        const size_t dot = s.find('.');
                        PinRef r;
                        r.node = dot == std::string::npos ? s : s.substr(0, dot);
                        r.pin  = dot == std::string::npos ? std::string() : s.substr(dot + 1);
                        b.link = r;
                    }
                    else if (v.is_boolean())
                        b.literal = Value::Bool1(v.get<bool>());
                    else if (v.is_number())
                    {
                        if (pd && pd->mode == PinMode::Fixed && pd->type == ValueType::Int)
                            b.literal = Value::Int32(static_cast<int32_t>(std::llround(v.get<double>())));
                        else
                            b.literal = Value::Float(v.get<float>());
                    }
                    else if (v.is_array() && v.size() >= 2 && v.size() <= 4)
                    {
                        float f[4] = {0, 0, 0, 0};
                        for (size_t k = 0; k < v.size(); ++k)
                        {
                            if (!v[k].is_number()) return fail("ノード " + id + " の入力 " + pin + " の値が不正です");
                            f[k] = v[k].get<float>();
                        }
                        b.literal = Value::Vec(static_cast<int>(v.size()), f);
                    }
                    else
                        return fail("ノード " + id + " の入力 " + pin + " の値が不正です");
                    n.inputs[pin] = std::move(b);
                }
            }
            nodes[id] = std::move(n);
        }
    }
    // pin 省略の接続を補う
    for (auto& kv : nodes)
        for (auto& ik : kv.second.inputs)
            if (ik.second.link && ik.second.link->pin.empty())
            {
                auto sn = nodes.find(ik.second.link->node);
                const NodeDef* sd = sn != nodes.end() ? lib.Find(sn->second.type) : nullptr;
                ik.second.link->pin = (sd && !sd->outputs.empty()) ? sd->outputs[0].name : "Out";
            }

    // layout
    if (j.contains("layout") && j["layout"].is_object())
        for (auto it = j["layout"].begin(); it != j["layout"].end(); ++it)
        {
            auto nn = nodes.find(it.key());
            if (nn == nodes.end() || !it.value().is_string()) continue;
            float xy[2];
            if (ParseFloats(it.value().get<std::string>(), xy, 2)) { nn->second.x = xy[0]; nn->second.y = xy[1]; }
        }
    for (auto& kv : nodes) out.LoadNode(std::move(kv.second));

    // view（UI 状態。無くてもよい）
    if (j.contains("view") && j["view"].is_string())
    {
        float p[3];
        if (ParseFloats(j["view"].get<std::string>(), p, 3))
        {
            ViewInfo v;
            v.valid = true; v.panX = p[0]; v.panY = p[1]; v.zoom = p[2];
            out.SetView(v);
        }
    }

    if (j.contains("comments") && j["comments"].is_object())
        for (auto it = j["comments"].begin(); it != j["comments"].end(); ++it)
        {
            if (!it.value().is_object()) continue;
            Comment c;
            c.id = it.key();
            const Json& e = it.value();
            if (e.contains("text") && e["text"].is_string()) c.text = e["text"].get<std::string>();
            if (e.contains("color") && e["color"].is_string()) c.color = e["color"].get<std::string>();
            if (e.contains("rect") && e["rect"].is_string())
            {
                float r[4];
                if (ParseFloats(e["rect"].get<std::string>(), r, 4)) { c.x = r[0]; c.y = r[1]; c.w = r[2]; c.h = r[3]; }
            }
            out.LoadComment(std::move(c));
        }

    out.LoadFinish();
    return true;
}

// ---------------------------------------------------------------------------
// ファイル
// ---------------------------------------------------------------------------
static bool ReadAll(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool LoadDxmgFile(const std::string& path, MaterialGraph& out, std::string* error)
{
    std::string text;
    if (!ReadAll(path, text))
    {
        if (error) *error = "ファイルを開けません: " + path;
        return false;
    }
    // UTF-8 BOM を許す
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    return LoadDxmg(text, out, error);
}

bool SaveDxmgFileIfChanged(const std::string& path, const MaterialGraph& g, std::string* error)
{
    const std::string text = SaveDxmg(g);
    std::string old;
    if (ReadAll(path, old) && old == text) return false;
    // 一時ファイル→flush→検証→置き換え（途中で落ちても元の .dxmg は無傷）
    const auto wr = atomicfile::WriteFile(std::filesystem::path(path), text);
    if (!wr)
    {
        if (error) *error = "ファイルを書けません: " + path + " (" + wr.error + ")";
        return false;
    }
    return true;
}

} // namespace dx12e::matgraph
