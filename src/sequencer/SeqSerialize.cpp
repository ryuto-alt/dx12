#include "sequencer/SeqSerialize.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "sequencer/SeqCurve.h"

namespace dx12e::seq
{

using OJson = nlohmann::ordered_json;

// ===========================================================================
// 書き出し
// ===========================================================================
namespace
{

void AppendEscaped(std::string& o, std::string_view s)
{
    static const char* hex = "0123456789abcdef";
    o.push_back('"');
    for (const char c : s)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (c)
        {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        default:
            if (u < 0x20) { o += "\\u00"; o.push_back(hex[u >> 4]); o.push_back(hex[u & 0xF]); }
            else o.push_back(c);
        }
    }
    o.push_back('"');
}

void AppendNum(std::string& o, double v)
{
    if (v == 0.0) v = 0.0;                 // -0 → 0
    if (!std::isfinite(v)) v = 0.0;        // 持てない値は 0(モデル側で拒否しているので通常は来ない)
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);   // 最短の往復可能表現
    o.append(buf, r.ptr);
}

void AppendInt(std::string& o, std::int64_t v)
{
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    o.append(buf, r.ptr);
}

// SeqValue を 1 行のコンパクトな JSON として出す(キーは辞書順)
void AppendValue(std::string& o, const SeqValue& v)
{
    switch (v.kind)
    {
    case SeqValue::Kind::Null:   o += "null"; break;
    case SeqValue::Kind::Bool:   o += v.b ? "true" : "false"; break;
    case SeqValue::Kind::Number: AppendNum(o, v.n); break;
    case SeqValue::Kind::String: AppendEscaped(o, v.s); break;
    case SeqValue::Kind::Array:
        o.push_back('[');
        for (std::size_t i = 0; i < v.arr.size(); ++i)
        {
            if (i) o += ", ";
            AppendValue(o, v.arr[i]);
        }
        o.push_back(']');
        break;
    case SeqValue::Kind::Object:
        o.push_back('{');
        for (std::size_t i = 0; i < v.obj.size(); ++i)
        {
            if (i) o += ", ";
            AppendEscaped(o, v.obj[i].key);
            o += ": ";
            AppendValue(o, v.obj[i].value);
        }
        o.push_back('}');
        break;
    }
}

std::string Pad(int n) { return std::string(static_cast<std::size_t>(n), ' '); }

// ---- 1 行オブジェクト(カット/マーカー/クリップ/イベント) ---------------------
struct LineObj
{
    std::string text = "{";
    bool first = true;
    void Key(std::string_view k)
    {
        if (!first) text += ", ";
        first = false;
        AppendEscaped(text, k);
        text += ": ";
    }
    void Str(std::string_view k, std::string_view v) { Key(k); AppendEscaped(text, v); }
    void Int(std::string_view k, std::int64_t v) { Key(k); AppendInt(text, v); }
    void Val(std::string_view k, const SeqValue& v) { Key(k); AppendValue(text, v); }
    void Params(const SeqValue& p)
    {
        for (const auto& m : p.obj) Val(m.key, m.value);
    }
    std::string End() { return text + "}"; }
};

// ---- 複数行オブジェクトのメンバ集め -----------------------------------------
struct Members
{
    std::vector<std::string> lines;   // すでにインデント済み・末尾カンマ無し
    int ind;
    explicit Members(int indent) : ind(indent) {}
    void Add(std::string_view key, const std::string& valueText)
    {
        std::string l = Pad(ind);
        AppendEscaped(l, key);
        l += ": ";
        l += valueText;
        lines.push_back(std::move(l));
    }
    void Str(std::string_view key, std::string_view v) { std::string t; AppendEscaped(t, v); Add(key, t); }
    void Int(std::string_view key, std::int64_t v) { std::string t; AppendInt(t, v); Add(key, t); }
    void Bool(std::string_view key, bool v) { Add(key, v ? "true" : "false"); }
    void Val(std::string_view key, const SeqValue& v) { std::string t; AppendValue(t, v); Add(key, t); }
    std::string Join() const
    {
        std::string o;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (i) o += ",\n";
            o += lines[i];
        }
        return o;
    }
};

// "[\n  <item>,\n  <item>\n<ind>]" 形式の配列(空なら "[]")。itemInd = 各要素のインデント。
std::string ListBlock(const std::vector<std::string>& items, int closeInd)
{
    if (items.empty()) return "[]";
    std::string o = "[\n";
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        o += Pad(closeInd + 2);
        o += items[i];
        o += (i + 1 < items.size()) ? ",\n" : "\n";
    }
    o += Pad(closeInd);
    o += "]";
    return o;
}

std::string KeyTuple(const Key& k)
{
    std::string o = "[";
    AppendInt(o, k.t);
    o += ", ";
    AppendNum(o, k.v);
    o += ", ";
    switch (k.ip)
    {
    case Interp::Step:   o += "\"s\""; break;
    case Interp::Linear: o += "\"l\""; break;
    case Interp::Auto:   o += "\"a\""; break;
    case Interp::Ease:   o += "\"e:"; o += EaseName(k.ease); o += "\""; break;
    case Interp::Bezier:
        o += "\"b\", ";
        AppendInt(o, k.inDt); o += ", ";
        AppendNum(o, k.inDv); o += ", ";
        AppendInt(o, k.outDt); o += ", ";
        AppendNum(o, k.outDv);
        break;
    }
    o += "]";
    return o;
}

std::string WriteChannel(const Channel& ch, int ind)   // ind = このオブジェクトの `{` を含む行のインデント
{
    Members m(ind + 2);
    if (ch.pre != Extrap::Hold) m.Str("pre", ExtrapName(ch.pre));
    if (ch.post != Extrap::Hold) m.Str("post", ExtrapName(ch.post));
    std::vector<std::string> keys;
    keys.reserve(ch.keys.size());
    for (const Key& k : ch.keys) keys.push_back(KeyTuple(k));
    m.Add("keys", ListBlock(keys, ind + 2));
    return "{\n" + m.Join() + "\n" + Pad(ind) + "}";
}

std::string WriteTrack(const Track& tr, int ind)
{
    Members m(ind + 2);
    m.Str("id", tr.id);
    m.Str("type", TrackTypeName(tr.type));
    if (!tr.name.empty()) m.Str("name", tr.name);
    if (tr.mute) m.Bool("mute", true);
    if (tr.lock) m.Bool("lock", true);
    if (tr.type == TrackType::Transform) m.Str("rotation", tr.rotation == RotationMode::Quat ? "quat" : "euler");
    if (!tr.path.empty()) m.Str("path", tr.path);
    if (tr.valueType != ValueType::Float)
        m.Str("valueType", tr.valueType == ValueType::Bool ? "bool" : (tr.valueType == ValueType::Int ? "int" : "color"));
    if (tr.colorSpace != ColorSpace::Linear) m.Str("colorSpace", "srgb");
    for (const auto& p : tr.params.obj) m.Val(p.key, p.value);

    switch (FamilyOf(tr.type))
    {
    case TrackFamily::Curve:
    {
        std::string body = "{";
        if (tr.channels.empty()) body += "}";
        else
        {
            body += "\n";
            const int ci = ind + 4;
            for (std::size_t i = 0; i < tr.channels.size(); ++i)
            {
                body += Pad(ci);
                AppendEscaped(body, tr.channels[i].name);
                body += ": ";
                body += WriteChannel(tr.channels[i], ci);
                body += (i + 1 < tr.channels.size()) ? ",\n" : "\n";
            }
            body += Pad(ind + 2) + "}";
        }
        m.Add("channels", body);
        break;
    }
    case TrackFamily::Clip:
    {
        std::vector<std::string> items;
        for (const Clip& c : tr.clips)
        {
            LineObj o;
            o.Str("id", c.id);
            o.Int("start", c.start);
            o.Int("dur", c.dur);
            o.Params(c.params);
            items.push_back(o.End());
        }
        m.Add("clips", ListBlock(items, ind + 2));
        break;
    }
    case TrackFamily::Event:
    {
        std::vector<std::string> items;
        for (const EventItem& e : tr.events)
        {
            LineObj o;
            o.Str("id", e.id);
            o.Int("t", e.t);
            o.Str("kind", e.kind);
            if (!e.name.empty()) o.Str("name", e.name);
            o.Params(e.params);
            items.push_back(o.End());
        }
        m.Add("events", ListBlock(items, ind + 2));
        break;
    }
    case TrackFamily::Constraint: break;
    }
    return "{\n" + m.Join() + "\n" + Pad(ind) + "}";
}

std::string WriteBinding(const Binding& b, int ind)
{
    Members m(ind + 2);
    m.Str("id", b.id);
    m.Str("name", b.name);
    m.Str("kind", BindingKindName(b.kind));
    if (!b.hint.guid.empty() || !b.hint.path.empty() || !b.hint.name.empty())
    {
        LineObj h;
        if (!b.hint.guid.empty()) h.Str("guid", b.hint.guid);
        if (!b.hint.path.empty()) h.Str("path", b.hint.path);
        if (!b.hint.name.empty()) h.Str("name", b.hint.name);
        m.Add("hint", h.End());
    }
    for (const auto& p : b.params.obj) m.Val(p.key, p.value);
    std::vector<std::string> tracks;
    for (const Track& t : b.tracks) tracks.push_back(WriteTrack(t, ind + 4));
    m.Add("tracks", ListBlock(tracks, ind + 2));
    return "{\n" + m.Join() + "\n" + Pad(ind) + "}";
}

} // namespace

std::string SerializeSequence(const Sequence& s)
{
    Members m(2);
    m.Str("format", "dxseq");
    m.Int("version", kDxseqVersion);
    if (!s.id.empty()) m.Str("id", s.id);
    m.Str("name", s.name);
    m.Int("ticksPerSecond", s.ticksPerSecond);
    m.Int("frameRate", s.frameRate);
    if (s.hasRange)
    {
        std::string t = "[";
        AppendInt(t, s.rangeStart); t += ", "; AppendInt(t, s.rangeEnd); t += "]";
        m.Add("range", t);
    }
    if (s.render)
    {
        LineObj o;
        o.Int("width", s.render->width);
        o.Int("height", s.render->height);
        o.Key("shutter"); AppendNum(o.text, s.render->shutter);
        o.Int("warmup", s.render->warmup);
        m.Add("render", o.End());
    }
    if (!s.meta.IsEmptyContainer()) m.Val("meta", s.meta);

    std::vector<std::string> cuts;
    for (const Cut& c : s.cuts)
    {
        LineObj o;
        o.Str("id", c.id);
        o.Int("start", c.start);
        o.Int("end", c.end);
        o.Str("camera", c.camera);
        if (c.blend != 0) o.Int("blend", c.blend);
        cuts.push_back(o.End());
    }
    m.Add("cuts", ListBlock(cuts, 2));

    std::vector<std::string> markers;
    for (const Marker& mk : s.markers)
    {
        LineObj o;
        o.Str("id", mk.id);
        o.Int("t", mk.t);
        o.Str("name", mk.name);
        o.Params(mk.params);
        markers.push_back(o.End());
    }
    m.Add("markers", ListBlock(markers, 2));

    std::vector<std::string> bindings;
    for (const Binding& b : s.bindings) bindings.push_back(WriteBinding(b, 4));
    m.Add("bindings", ListBlock(bindings, 2));

    return "{\n" + m.Join() + "\n}\n";
}

// ===========================================================================
// 読み込み
// ===========================================================================
namespace
{

struct PErr
{
    std::string msg;
};

[[noreturn]] void Throw(const std::string& path, const std::string& msg)
{
    throw PErr{ path.empty() ? msg : path + ": " + msg };
}

std::string Sub(const std::string& base, const std::string& key)
{
    return base.empty() ? key : base + "." + key;
}
std::string Idx(const std::string& base, std::size_t i)
{
    return base + "[" + std::to_string(i) + "]";
}

Tick ToTick(const OJson& j, const std::string& path)
{
    if (j.is_number_integer())
    {
        const std::int64_t v = j.get<std::int64_t>();
        if (v < -kMaxTick || v > kMaxTick) Throw(path, "時刻が範囲外");
        return v;
    }
    if (j.is_number_unsigned()) Throw(path, "時刻が範囲外");
    if (j.is_number_float())
    {
        const double d = j.get<double>();
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) <= static_cast<double>(kMaxTick)) return static_cast<Tick>(d);
    }
    Throw(path, "整数(ティック)でなければならない");
}

double ToNum(const OJson& j, const std::string& path)
{
    if (!j.is_number()) Throw(path, "数値でなければならない");
    const double d = j.get<double>();
    if (!std::isfinite(d)) Throw(path, "有限の数値でなければならない");
    return d;
}

const std::string& ToStr(const OJson& j, const std::string& path)
{
    if (!j.is_string()) Throw(path, "文字列でなければならない");
    return j.get_ref<const std::string&>();
}

const OJson* Get(const OJson& obj, const char* key)
{
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

void RequireObject(const OJson& j, const std::string& path)
{
    if (!j.is_object()) Throw(path, "オブジェクトでなければならない");
}

SeqValue ToValue(const OJson& j, const std::string& path, int depth)
{
    if (depth > 32) Throw(path, "入れ子が深すぎる");
    switch (j.type())
    {
    case OJson::value_t::null: return SeqValue();
    case OJson::value_t::boolean: return SeqValue::MakeBool(j.get<bool>());
    case OJson::value_t::number_integer:
    case OJson::value_t::number_unsigned:
    case OJson::value_t::number_float: return SeqValue::MakeNumber(ToNum(j, path));
    case OJson::value_t::string: return SeqValue::MakeString(j.get<std::string>());
    case OJson::value_t::array:
    {
        SeqValue v = SeqValue::MakeArray();
        std::size_t i = 0;
        for (const auto& e : j) v.arr.push_back(ToValue(e, Idx(path, i++), depth + 1));
        return v;
    }
    case OJson::value_t::object:
    {
        SeqValue v = SeqValue::MakeObject();
        for (auto it = j.begin(); it != j.end(); ++it) v.Set(it.key(), ToValue(it.value(), Sub(path, it.key()), depth + 1));
        return v;
    }
    default: Throw(path, "扱えない JSON 型");
    }
}

Key ParseKey(const OJson& j, const std::string& path)
{
    if (!j.is_array() || j.size() < 2 || j.size() > 7) Throw(path, "キーは [時刻, 値, 補間, …] の配列でなければならない");
    Key k;
    k.t = ToTick(j[0], Idx(path, 0));
    k.v = ToNum(j[1], Idx(path, 1));
    std::string ip = "a";
    if (j.size() >= 3) ip = ToStr(j[2], Idx(path, 2));
    if (ip == "s") k.ip = Interp::Step;
    else if (ip == "l") k.ip = Interp::Linear;
    else if (ip == "a") k.ip = Interp::Auto;
    else if (ip == "b") k.ip = Interp::Bezier;
    else if (ip.size() > 2 && ip[0] == 'e' && ip[1] == ':')
    {
        k.ip = Interp::Ease;
        if (!EaseFromName(std::string_view(ip).substr(2), k.ease)) Throw(Idx(path, 2), "知らないイージング名: " + ip);
    }
    else Throw(Idx(path, 2), "知らない補間種別: " + ip);

    if (k.ip == Interp::Bezier)
    {
        if (j.size() != 7) Throw(path, "ベジェのキーは [t, v, \"b\", inDt, inDv, outDt, outDv]");
        k.inDt = ToTick(j[3], Idx(path, 3));
        k.inDv = ToNum(j[4], Idx(path, 4));
        k.outDt = ToTick(j[5], Idx(path, 5));
        k.outDv = ToNum(j[6], Idx(path, 6));
    }
    else if (j.size() > 3)
    {
        Throw(path, "ベジェ以外のキーに余分な要素がある");
    }
    NormalizeKey(k);
    if (std::string e = CheckKey(k); !e.empty()) Throw(path, e);
    return k;
}

Channel ParseChannel(const std::string& name, const OJson& j, const std::string& path, std::vector<std::string>& warnings)
{
    RequireObject(j, path);
    Channel ch;
    ch.name = name;
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        const std::string& k = it.key();
        if (k == "pre" || k == "post")
        {
            Extrap e;
            if (!ExtrapFromName(ToStr(it.value(), Sub(path, k)), e)) Throw(Sub(path, k), "知らない範囲外挙動");
            (k == "pre" ? ch.pre : ch.post) = e;
        }
        else if (k == "keys")
        {
            if (!it.value().is_array()) Throw(Sub(path, k), "配列でなければならない");
            std::size_t i = 0;
            for (const auto& kj : it.value()) { ch.keys.push_back(ParseKey(kj, Idx(Sub(path, "keys"), i))); ++i; }
        }
        else warnings.push_back(path + ": 未知のフィールド \"" + k + "\" を無視した");
    }
    // 昇順でなければ安定ソートで直す
    const bool sorted = std::is_sorted(ch.keys.begin(), ch.keys.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
    if (!sorted)
    {
        std::stable_sort(ch.keys.begin(), ch.keys.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
        warnings.push_back(path + ": キーが時刻昇順でなかったので並べ替えた");
    }
    return ch;
}

// 予約語以外を params へ集める
template <typename ReservedFn>
void CollectParams(const OJson& j, ReservedFn reserved, SeqValue& params, const std::string& path)
{
    params = SeqValue::MakeObject();
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!reserved(it.key())) params.Set(it.key(), ToValue(it.value(), Sub(path, it.key()), 0));
}

Track ParseTrack(const OJson& j, const std::string& path, std::vector<std::string>& warnings)
{
    RequireObject(j, path);
    Track tr;
    const OJson* v;
    if (!(v = Get(j, "id"))) Throw(path, "id が無い");
    tr.id = ToStr(*v, Sub(path, "id"));
    if (!(v = Get(j, "type"))) Throw(path, "type が無い");
    if (!TrackTypeFromName(ToStr(*v, Sub(path, "type")), tr.type)) Throw(Sub(path, "type"), "知らないトラック種別: " + v->get<std::string>());
    if ((v = Get(j, "name"))) tr.name = ToStr(*v, Sub(path, "name"));
    if ((v = Get(j, "mute"))) { if (!v->is_boolean()) Throw(Sub(path, "mute"), "真偽値でなければならない"); tr.mute = v->get<bool>(); }
    if ((v = Get(j, "lock"))) { if (!v->is_boolean()) Throw(Sub(path, "lock"), "真偽値でなければならない"); tr.lock = v->get<bool>(); }
    if ((v = Get(j, "path"))) tr.path = ToStr(*v, Sub(path, "path"));
    if ((v = Get(j, "rotation")))
    {
        const std::string& r = ToStr(*v, Sub(path, "rotation"));
        if (r == "euler") tr.rotation = RotationMode::Euler;
        else if (r == "quat") tr.rotation = RotationMode::Quat;
        else Throw(Sub(path, "rotation"), "rotation は euler / quat");
    }
    if ((v = Get(j, "valueType")))
    {
        const std::string& r = ToStr(*v, Sub(path, "valueType"));
        if (r == "float") tr.valueType = ValueType::Float;
        else if (r == "bool") tr.valueType = ValueType::Bool;
        else if (r == "int") tr.valueType = ValueType::Int;
        else if (r == "color") tr.valueType = ValueType::Color;
        else Throw(Sub(path, "valueType"), "valueType は float / bool / int / color");
    }
    if ((v = Get(j, "colorSpace")))
    {
        const std::string& r = ToStr(*v, Sub(path, "colorSpace"));
        if (r == "linear") tr.colorSpace = ColorSpace::Linear;
        else if (r == "srgb") tr.colorSpace = ColorSpace::Srgb;
        else Throw(Sub(path, "colorSpace"), "colorSpace は linear / srgb");
    }
    CollectParams(j, IsReservedTrackParam, tr.params, path);

    if ((v = Get(j, "channels")))
    {
        const std::string cp = Sub(path, "channels");
        RequireObject(*v, cp);
        for (auto it = v->begin(); it != v->end(); ++it)
            tr.channels.push_back(ParseChannel(it.key(), it.value(), Sub(cp, it.key()), warnings));
    }
    if ((v = Get(j, "clips")))
    {
        const std::string cp = Sub(path, "clips");
        if (!v->is_array()) Throw(cp, "配列でなければならない");
        std::size_t i = 0;
        for (const auto& cj : *v)
        {
            const std::string p = Idx(cp, i++);
            RequireObject(cj, p);
            Clip c;
            const OJson* f;
            if (!(f = Get(cj, "id"))) Throw(p, "id が無い");
            c.id = ToStr(*f, Sub(p, "id"));
            if (!(f = Get(cj, "start"))) Throw(p, "start が無い");
            c.start = ToTick(*f, Sub(p, "start"));
            if ((f = Get(cj, "dur"))) c.dur = ToTick(*f, Sub(p, "dur"));
            CollectParams(cj, IsReservedClipParam, c.params, p);
            tr.clips.push_back(std::move(c));
        }
    }
    if ((v = Get(j, "events")))
    {
        const std::string cp = Sub(path, "events");
        if (!v->is_array()) Throw(cp, "配列でなければならない");
        std::size_t i = 0;
        for (const auto& ej : *v)
        {
            const std::string p = Idx(cp, i++);
            RequireObject(ej, p);
            EventItem e;
            const OJson* f;
            if (!(f = Get(ej, "id"))) Throw(p, "id が無い");
            e.id = ToStr(*f, Sub(p, "id"));
            if (!(f = Get(ej, "t"))) Throw(p, "t が無い");
            e.t = ToTick(*f, Sub(p, "t"));
            if (!(f = Get(ej, "kind"))) Throw(p, "kind が無い");
            e.kind = ToStr(*f, Sub(p, "kind"));
            if ((f = Get(ej, "name"))) e.name = ToStr(*f, Sub(p, "name"));
            CollectParams(ej, IsReservedEventParam, e.params, p);
            tr.events.push_back(std::move(e));
        }
    }
    if (tr.type != TrackType::Transform) tr.rotation = RotationMode::Euler;   // 正準化(SeqModel の CanonicalizeTrack と同じ)
    return tr;
}

void WarnUnknown(const OJson& j, std::initializer_list<const char*> known, const std::string& path,
                 std::vector<std::string>& warnings)
{
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        bool ok = false;
        for (const char* k : known) if (it.key() == k) { ok = true; break; }
        if (!ok) warnings.push_back(path + ": 未知のフィールド \"" + it.key() + "\" を無視した");
    }
}

void ParseInto(const OJson& root, Sequence& out, std::vector<std::string>& warnings)
{
    RequireObject(root, "");
    const OJson* v;
    if (!(v = Get(root, "format")) || !v->is_string() || v->get<std::string>() != "dxseq") Throw("format", "\"dxseq\" でなければならない");
    if (!(v = Get(root, "version")) || !v->is_number_integer()) Throw("version", "整数のバージョンが無い");
    const std::int64_t ver = v->get<std::int64_t>();
    if (ver != kDxseqVersion) Throw("version", "未対応のバージョン " + std::to_string(ver) + "(対応: " + std::to_string(kDxseqVersion) + ")");

    Sequence s;
    if ((v = Get(root, "id"))) s.id = ToStr(*v, "id");
    if ((v = Get(root, "name"))) s.name = ToStr(*v, "name");
    if ((v = Get(root, "ticksPerSecond")))
    {
        s.ticksPerSecond = ToTick(*v, "ticksPerSecond");
    }
    if ((v = Get(root, "frameRate")))
    {
        const Tick fr = ToTick(*v, "frameRate");
        if (fr < 1 || fr > 1000) Throw("frameRate", "frameRate は 1..1000");
        s.frameRate = static_cast<int>(fr);
    }
    if ((v = Get(root, "range")))
    {
        if (!v->is_array() || v->size() != 2) Throw("range", "[開始, 終了] でなければならない");
        s.hasRange = true;
        s.rangeStart = ToTick((*v)[0], "range[0]");
        s.rangeEnd = ToTick((*v)[1], "range[1]");
    }
    if ((v = Get(root, "render")))
    {
        RequireObject(*v, "render");
        RenderSettings r;
        const OJson* f;
        if ((f = Get(*v, "width")))   r.width = static_cast<int>(ToTick(*f, "render.width"));
        if ((f = Get(*v, "height")))  r.height = static_cast<int>(ToTick(*f, "render.height"));
        if ((f = Get(*v, "shutter"))) r.shutter = ToNum(*f, "render.shutter");
        if ((f = Get(*v, "warmup")))  r.warmup = static_cast<int>(ToTick(*f, "render.warmup"));
        s.render = r;
    }
    s.meta = SeqValue::MakeObject();
    if ((v = Get(root, "meta")))
    {
        s.meta = ToValue(*v, "meta", 0);
        if (s.meta.IsNull()) s.meta = SeqValue::MakeObject();
        if (!s.meta.IsObject()) Throw("meta", "オブジェクトでなければならない");
    }
    WarnUnknown(root, { "format", "version", "id", "name", "ticksPerSecond", "frameRate", "range", "render", "meta",
                        "cuts", "markers", "bindings" }, "", warnings);

    if ((v = Get(root, "cuts")))
    {
        if (!v->is_array()) Throw("cuts", "配列でなければならない");
        std::size_t i = 0;
        for (const auto& cj : *v)
        {
            const std::string p = Idx("cuts", i++);
            RequireObject(cj, p);
            Cut c;
            const OJson* f;
            if (!(f = Get(cj, "id"))) Throw(p, "id が無い");
            c.id = ToStr(*f, Sub(p, "id"));
            if (!(f = Get(cj, "start"))) Throw(p, "start が無い");
            c.start = ToTick(*f, Sub(p, "start"));
            if (!(f = Get(cj, "end"))) Throw(p, "end が無い");
            c.end = ToTick(*f, Sub(p, "end"));
            if (!(f = Get(cj, "camera"))) Throw(p, "camera が無い");
            c.camera = ToStr(*f, Sub(p, "camera"));
            if ((f = Get(cj, "blend"))) c.blend = ToTick(*f, Sub(p, "blend"));
            WarnUnknown(cj, { "id", "start", "end", "camera", "blend" }, p, warnings);
            s.cuts.push_back(std::move(c));
        }
    }
    if ((v = Get(root, "markers")))
    {
        if (!v->is_array()) Throw("markers", "配列でなければならない");
        std::size_t i = 0;
        for (const auto& mj : *v)
        {
            const std::string p = Idx("markers", i++);
            RequireObject(mj, p);
            Marker m;
            const OJson* f;
            if (!(f = Get(mj, "id"))) Throw(p, "id が無い");
            m.id = ToStr(*f, Sub(p, "id"));
            if (!(f = Get(mj, "t"))) Throw(p, "t が無い");
            m.t = ToTick(*f, Sub(p, "t"));
            if ((f = Get(mj, "name"))) m.name = ToStr(*f, Sub(p, "name"));
            CollectParams(mj, IsReservedMarkerParam, m.params, p);
            s.markers.push_back(std::move(m));
        }
    }
    if ((v = Get(root, "bindings")))
    {
        if (!v->is_array()) Throw("bindings", "配列でなければならない");
        std::size_t i = 0;
        for (const auto& bj : *v)
        {
            const std::string p = Idx("bindings", i++);
            RequireObject(bj, p);
            Binding b;
            const OJson* f;
            if (!(f = Get(bj, "id"))) Throw(p, "id が無い");
            b.id = ToStr(*f, Sub(p, "id"));
            if ((f = Get(bj, "name"))) b.name = ToStr(*f, Sub(p, "name"));
            if ((f = Get(bj, "kind")))
            {
                if (!BindingKindFromName(ToStr(*f, Sub(p, "kind")), b.kind)) Throw(Sub(p, "kind"), "kind は entity / scene / spawnable");
            }
            if ((f = Get(bj, "hint")))
            {
                const std::string hp = Sub(p, "hint");
                RequireObject(*f, hp);
                const OJson* h;
                if ((h = Get(*f, "guid"))) b.hint.guid = ToStr(*h, Sub(hp, "guid"));
                if ((h = Get(*f, "path"))) b.hint.path = ToStr(*h, Sub(hp, "path"));
                if ((h = Get(*f, "name"))) b.hint.name = ToStr(*h, Sub(hp, "name"));
                WarnUnknown(*f, { "guid", "path", "name" }, hp, warnings);
            }
            CollectParams(bj, IsReservedBindingParam, b.params, p);
            if ((f = Get(bj, "tracks")))
            {
                const std::string tp = Sub(p, "tracks");
                if (!f->is_array()) Throw(tp, "配列でなければならない");
                std::size_t ti = 0;
                for (const auto& tj : *f) b.tracks.push_back(ParseTrack(tj, Idx(tp, ti++), warnings));
            }
            s.bindings.push_back(std::move(b));
        }
    }

    if (std::string e = CheckSequenceStructure(s); !e.empty()) Throw("", e);
    out = std::move(s);
}

} // namespace

ParseResult ParseSequence(std::string_view text, Sequence& out)
{
    ParseResult r;
    OJson root = OJson::parse(text.begin(), text.end(), nullptr, false);
    if (root.is_discarded())
    {
        r.error = "JSON として読めない";
        return r;
    }
    try
    {
        ParseInto(root, out, r.warnings);
        r.ok = true;
    }
    catch (const PErr& e)
    {
        r.error = e.msg;
        r.ok = false;
    }
    return r;
}

ParseResult LoadSequenceFile(const std::string& path, Sequence& out)
{
    ParseResult r;
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        r.error = "ファイルを開けない: " + path;
        return r;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ParseSequence(ss.str(), out);
}

bool SaveSequenceFile(const std::string& path, const Sequence& seq, std::string* error)
{
    namespace fs = std::filesystem;
    const std::string text = SerializeSequence(seq);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (error) *error = "書き込めない: " + tmp; return false; }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) { if (error) *error = "書き込みに失敗: " + tmp; return false; }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        if (error) *error = "リネームに失敗: " + path;
        return false;
    }
    return true;
}

} // namespace dx12e::seq
