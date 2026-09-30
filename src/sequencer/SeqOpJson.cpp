#include "sequencer/SeqOpJson.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include <nlohmann/json.hpp>

#include "sequencer/SeqCurve.h"
#include "sequencer/SeqSerialize.h"

namespace dx12e::seq
{

namespace
{
using J = nlohmann::ordered_json;

struct PErr
{
    std::string msg;
};

[[noreturn]] void Fail(const std::string& m) { throw PErr{ m }; }

struct Ctx
{
    const Sequence& cur;
    const IdIssuer& ids;
    std::int64_t tps;
    std::string NewId(char prefix) const
    {
        if (ids) return ids(prefix);
        Fail(std::string("ID の発行元が無いので、id を省略できない(") + prefix + ")");
    }
};

// ---- 小道具 ----------------------------------------------------------------
std::string Norm(std::string_view s)
{
    std::string o;
    for (const char c : s)
    {
        if (c == '_' || c == '-' || c == ' ') continue;
        o.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return o;
}

const J* Field(const J& o, const char* k)
{
    const auto it = o.find(k);
    return it == o.end() ? nullptr : &*it;
}

const J& Need(const J& o, const char* k)
{
    const J* f = Field(o, k);
    if (!f) Fail(std::string("フィールド \"") + k + "\" が無い");
    return *f;
}

std::string NeedStr(const J& o, const char* k)
{
    const J& f = Need(o, k);
    if (!f.is_string()) Fail(std::string("\"") + k + "\" は文字列でなければならない");
    return f.get<std::string>();
}

std::string OptStr(const J& o, const char* k, const std::string& def = {})
{
    const J* f = Field(o, k);
    if (!f) return def;
    if (!f->is_string()) Fail(std::string("\"") + k + "\" は文字列でなければならない");
    return f->get<std::string>();
}

double NumOf(const J& j, const std::string& what)
{
    if (!j.is_number()) Fail(what + " は数値でなければならない");
    const double d = j.get<double>();
    if (!std::isfinite(d)) Fail(what + " は有限の数値でなければならない");
    return d;
}

int IntOf(const J& j, const std::string& what)
{
    const double d = NumOf(j, what);
    if (d != std::floor(d) || std::fabs(d) > 2147483647.0) Fail(what + " は整数でなければならない");
    return static_cast<int>(d);
}

Tick TickFromJ(const J& j, const std::string& what)
{
    const double d = NumOf(j, what);
    if (d != std::floor(d)) Fail(what + " は整数(ティック)でなければならない。秒で書くなら〜Sec を使う");
    if (std::fabs(d) > static_cast<double>(kMaxTick)) Fail(what + " が範囲外");
    return static_cast<Tick>(d);
}

// tickKey(整数ティック)または secKey(秒。四捨五入)のどちらかから読む。
bool TickField(const J& o, const char* tickKey, const char* secKey, const Ctx& c, Tick& out)
{
    if (const J* f = Field(o, tickKey)) { out = TickFromJ(*f, tickKey); return true; }
    if (secKey)
        if (const J* f = Field(o, secKey)) { out = SecondsToTicks(NumOf(*f, secKey), c.tps); return true; }
    return false;
}

Tick NeedTick(const J& o, const char* tickKey, const char* secKey, const Ctx& c)
{
    Tick t = 0;
    if (!TickField(o, tickKey, secKey, c, t))
        Fail(std::string("フィールド \"") + tickKey + "\"" + (secKey ? std::string("(または \"") + secKey + "\")" : std::string()) + " が無い");
    return t;
}

SeqValue ToSeqValue(const J& j, int depth = 0)
{
    if (depth > 32) Fail("入れ子が深すぎる");
    switch (j.type())
    {
    case J::value_t::null: return SeqValue();
    case J::value_t::boolean: return SeqValue::MakeBool(j.get<bool>());
    case J::value_t::number_integer:
    case J::value_t::number_unsigned:
    case J::value_t::number_float: return SeqValue::MakeNumber(NumOf(j, "数値"));
    case J::value_t::string: return SeqValue::MakeString(j.get<std::string>());
    case J::value_t::array:
    {
        SeqValue v = SeqValue::MakeArray();
        for (const auto& e : j) v.arr.push_back(ToSeqValue(e, depth + 1));
        return v;
    }
    case J::value_t::object:
    {
        SeqValue v = SeqValue::MakeObject();
        for (auto it = j.begin(); it != j.end(); ++it) v.Set(it.key(), ToSeqValue(it.value(), depth + 1));
        return v;
    }
    default: Fail("扱えない JSON 型");
    }
}

// 予約キー以外を params へ集める(オブジェクトのメンバをそのまま)
SeqValue CollectParams(const J& o, std::initializer_list<const char*> reserved)
{
    SeqValue p = SeqValue::MakeObject();
    for (auto it = o.begin(); it != o.end(); ++it)
    {
        bool r = false;
        for (const char* k : reserved) if (it.key() == k) { r = true; break; }
        if (!r) p.Set(it.key(), ToSeqValue(it.value()));
    }
    return p;
}

// ---- 断片のパース(S0 のパーサを「1 要素だけ入れた仮のシーケンス」に通して使う)---------------------
std::string EmbedSequenceJson(const std::string& bindingsArrayInner)
{
    return "{\"format\":\"dxseq\",\"version\":1,\"bindings\":[" + bindingsArrayInner + "]}";
}

Sequence ParseEmbedded(const std::string& text)
{
    Sequence s;
    const ParseResult r = ParseSequence(text, s);
    if (!r) Fail(r.error);
    return s;
}

// キー 1 個(タプルまたはオブジェクト)→ Key
Key KeyFrom(const J& kj, const Ctx& c)
{
    J tuple;
    if (kj.is_array())
    {
        tuple = kj;
    }
    else if (kj.is_object())
    {
        Tick t = 0;
        if (!TickField(kj, "t", "sec", c, t)) Fail("キーに t(ティック)か sec(秒)が無い");
        const J& v = Need(kj, "v");
        std::string ip = "a";
        if (const J* f = Field(kj, "ip")) { if (!f->is_string()) Fail("ip は文字列"); ip = f->get<std::string>(); }
        if (ip == "step") ip = "s";
        else if (ip == "linear") ip = "l";
        else if (ip == "auto") ip = "a";
        else if (ip == "bezier") ip = "b";
        else if (ip.rfind("ease:", 0) == 0) ip = "e:" + ip.substr(5);
        tuple = J::array({ t, v, ip });
        if (ip == "b")
        {
            tuple.push_back(TickFromJ(Need(kj, "inDt"), "inDt"));
            tuple.push_back(Need(kj, "inDv"));
            tuple.push_back(TickFromJ(Need(kj, "outDt"), "outDt"));
            tuple.push_back(Need(kj, "outDv"));
        }
    }
    else Fail("キーは [t, v, ip] の配列か {t|sec, v, ip} のオブジェクト");

    const std::string text = EmbedSequenceJson(
        "{\"id\":\"b_k\",\"tracks\":[{\"id\":\"t_k\",\"type\":\"property\",\"channels\":{\"v\":{\"keys\":[" + tuple.dump() + "]}}}]}");
    const Sequence s = ParseEmbedded(text);
    return s.bindings[0].tracks[0].channels[0].keys[0];
}

// チャンネル JSON({pre?, post?, keys:[…]})→ Channel。keys の要素はタプル/オブジェクトどちらでも
Channel ChannelFrom(const std::string& name, const J& cj, const Ctx& c)
{
    if (!cj.is_object()) Fail("channel はオブジェクトでなければならない");
    J norm = J::object();
    for (auto it = cj.begin(); it != cj.end(); ++it)
    {
        if (it.key() == "keys")
        {
            if (!it.value().is_array()) Fail("keys は配列でなければならない");
            J keys = J::array();
            for (const auto& k : it.value())
            {
                const Key kk = KeyFrom(k, c);
                J tup = J::array({ kk.t, kk.v });
                switch (kk.ip)
                {
                case Interp::Step: tup.push_back("s"); break;
                case Interp::Linear: tup.push_back("l"); break;
                case Interp::Auto: tup.push_back("a"); break;
                case Interp::Bezier: tup.push_back("b"); tup.push_back(kk.inDt); tup.push_back(kk.inDv); tup.push_back(kk.outDt); tup.push_back(kk.outDv); break;
                case Interp::Ease: tup.push_back(std::string("e:") + EaseName(kk.ease)); break;
                }
                keys.push_back(std::move(tup));
            }
            norm["keys"] = std::move(keys);
        }
        else norm[it.key()] = it.value();
    }
    J chs = J::object();
    chs[name] = std::move(norm);
    const std::string text = EmbedSequenceJson(
        "{\"id\":\"b_k\",\"tracks\":[{\"id\":\"t_k\",\"type\":\"property\",\"channels\":" + chs.dump() + "}]}");
    const Sequence s = ParseEmbedded(text);
    return s.bindings[0].tracks[0].channels[0];
}

// トラック JSON の正規化: id 補完・クリップ/イベントの秒指定→ティック・キーのオブジェクト→タプル
void NormalizeTrackJson(J& tj, const Ctx& c)
{
    if (!tj.is_object()) Fail("track はオブジェクトでなければならない");
    if (!Field(tj, "id")) tj["id"] = c.NewId('t');
    if (J* chs = tj.contains("channels") ? &tj["channels"] : nullptr)
    {
        if (!chs->is_object()) Fail("channels はオブジェクトでなければならない");
        for (auto it = chs->begin(); it != chs->end(); ++it)
        {
            const Channel ch = ChannelFrom(it.key(), it.value(), c);
            J o = J::object();
            if (ch.pre != Extrap::Hold) o["pre"] = ExtrapName(ch.pre);
            if (ch.post != Extrap::Hold) o["post"] = ExtrapName(ch.post);
            J keys = J::array();
            for (const Key& k : ch.keys)
            {
                J tup = J::array({ k.t, k.v });
                switch (k.ip)
                {
                case Interp::Step: tup.push_back("s"); break;
                case Interp::Linear: tup.push_back("l"); break;
                case Interp::Auto: tup.push_back("a"); break;
                case Interp::Bezier: tup.push_back("b"); tup.push_back(k.inDt); tup.push_back(k.inDv); tup.push_back(k.outDt); tup.push_back(k.outDv); break;
                case Interp::Ease: tup.push_back(std::string("e:") + EaseName(k.ease)); break;
                }
                keys.push_back(std::move(tup));
            }
            o["keys"] = std::move(keys);
            it.value() = std::move(o);
        }
    }
    if (J* cl = tj.contains("clips") ? &tj["clips"] : nullptr)
    {
        if (!cl->is_array()) Fail("clips は配列でなければならない");
        for (J& cj : *cl)
        {
            if (!cj.is_object()) Fail("clip はオブジェクトでなければならない");
            if (!Field(cj, "id")) cj["id"] = c.NewId('k');
            Tick t = 0;
            if (const J* f = Field(cj, "startSec")) { t = SecondsToTicks(NumOf(*f, "startSec"), c.tps); cj.erase("startSec"); cj["start"] = t; }
            if (const J* f = Field(cj, "durSec")) { t = SecondsToTicks(NumOf(*f, "durSec"), c.tps); cj.erase("durSec"); cj["dur"] = t; }
        }
    }
    if (J* ev = tj.contains("events") ? &tj["events"] : nullptr)
    {
        if (!ev->is_array()) Fail("events は配列でなければならない");
        for (J& ej : *ev)
        {
            if (!ej.is_object()) Fail("event はオブジェクトでなければならない");
            if (!Field(ej, "id")) ej["id"] = c.NewId('e');
            if (const J* f = Field(ej, "tSec")) { const Tick t = SecondsToTicks(NumOf(*f, "tSec"), c.tps); ej.erase("tSec"); ej["t"] = t; }
        }
    }
}

Track TrackFromJson(J tj, const Ctx& c)
{
    NormalizeTrackJson(tj, c);
    const std::string text = EmbedSequenceJson("{\"id\":\"b_k\",\"tracks\":[" + tj.dump() + "]}");
    Sequence s = ParseEmbedded(text);
    return std::move(s.bindings[0].tracks[0]);
}

Binding BindingFromJson(J bj, const Ctx& c)
{
    if (!bj.is_object()) Fail("binding はオブジェクトでなければならない");
    if (!Field(bj, "id")) bj["id"] = c.NewId('b');
    if (J* tr = bj.contains("tracks") ? &bj["tracks"] : nullptr)
    {
        if (!tr->is_array()) Fail("tracks は配列でなければならない");
        for (J& t : *tr) { NormalizeTrackJson(t, c); }
    }
    const std::string text = EmbedSequenceJson(bj.dump());
    Sequence s = ParseEmbedded(text);
    return std::move(s.bindings[0]);
}

// ---- ヘッダ ------------------------------------------------------------------
BindingHeader BindingHeaderFrom(const J& hj, const BindingHeader& base)
{
    if (!hj.is_object()) Fail("header はオブジェクトでなければならない");
    BindingHeader h = base;
    if (const J* f = Field(hj, "name")) { if (!f->is_string()) Fail("name は文字列"); h.name = f->get<std::string>(); }
    if (const J* f = Field(hj, "kind"))
    {
        if (!f->is_string() || !BindingKindFromName(f->get<std::string>(), h.kind)) Fail("kind は entity / scene / spawnable");
    }
    if (const J* f = Field(hj, "hint"))
    {
        if (!f->is_object()) Fail("hint はオブジェクト");
        if (const J* g = Field(*f, "guid")) { if (!g->is_string()) Fail("hint.guid は文字列"); h.hint.guid = g->get<std::string>(); }
        if (const J* g = Field(*f, "path")) { if (!g->is_string()) Fail("hint.path は文字列"); h.hint.path = g->get<std::string>(); }
        if (const J* g = Field(*f, "name")) { if (!g->is_string()) Fail("hint.name は文字列"); h.hint.name = g->get<std::string>(); }
    }
    if (const J* f = Field(hj, "params")) { h.params = ToSeqValue(*f); if (h.params.IsNull()) h.params = SeqValue::MakeObject(); }
    return h;
}

TrackHeader TrackHeaderFrom(const J& hj, const TrackHeader& base)
{
    if (!hj.is_object()) Fail("header はオブジェクトでなければならない");
    TrackHeader h = base;
    if (const J* f = Field(hj, "name")) { if (!f->is_string()) Fail("name は文字列"); h.name = f->get<std::string>(); }
    if (const J* f = Field(hj, "mute")) { if (!f->is_boolean()) Fail("mute は真偽値"); h.mute = f->get<bool>(); }
    if (const J* f = Field(hj, "lock")) { if (!f->is_boolean()) Fail("lock は真偽値"); h.lock = f->get<bool>(); }
    if (const J* f = Field(hj, "path")) { if (!f->is_string()) Fail("path は文字列"); h.path = f->get<std::string>(); }
    if (const J* f = Field(hj, "valueType"))
    {
        const std::string r = f->is_string() ? f->get<std::string>() : std::string();
        if (r == "float") h.valueType = ValueType::Float;
        else if (r == "bool") h.valueType = ValueType::Bool;
        else if (r == "int") h.valueType = ValueType::Int;
        else if (r == "color") h.valueType = ValueType::Color;
        else Fail("valueType は float / bool / int / color");
    }
    if (const J* f = Field(hj, "colorSpace"))
    {
        const std::string r = f->is_string() ? f->get<std::string>() : std::string();
        if (r == "linear") h.colorSpace = ColorSpace::Linear;
        else if (r == "srgb") h.colorSpace = ColorSpace::Srgb;
        else Fail("colorSpace は linear / srgb");
    }
    if (const J* f = Field(hj, "rotation"))
    {
        const std::string r = f->is_string() ? f->get<std::string>() : std::string();
        if (r == "euler") h.rotation = RotationMode::Euler;
        else if (r == "quat") h.rotation = RotationMode::Quat;
        else Fail("rotation は euler / quat");
    }
    if (const J* f = Field(hj, "params")) { h.params = ToSeqValue(*f); if (h.params.IsNull()) h.params = SeqValue::MakeObject(); }
    return h;
}

// ---- クリップ / イベント / マーカー / カット ------------------------------------------
Clip ClipFrom(const J& cj, const Ctx& c)
{
    if (!cj.is_object()) Fail("clip はオブジェクトでなければならない");
    Clip k;
    k.id = OptStr(cj, "id");
    if (k.id.empty()) k.id = c.NewId('k');
    k.start = NeedTick(cj, "start", "startSec", c);
    TickField(cj, "dur", "durSec", c, k.dur);
    k.params = CollectParams(cj, { "id", "start", "startSec", "dur", "durSec" });
    return k;
}

EventItem EventFrom(const J& ej, const Ctx& c)
{
    if (!ej.is_object()) Fail("event はオブジェクトでなければならない");
    EventItem e;
    e.id = OptStr(ej, "id");
    if (e.id.empty()) e.id = c.NewId('e');
    e.t = NeedTick(ej, "t", "tSec", c);
    e.kind = NeedStr(ej, "kind");
    e.name = OptStr(ej, "name");
    e.params = CollectParams(ej, { "id", "t", "tSec", "kind", "name" });
    return e;
}

Marker MarkerFrom(const J& mj, const Ctx& c)
{
    if (!mj.is_object()) Fail("marker はオブジェクトでなければならない");
    Marker m;
    m.id = OptStr(mj, "id");
    if (m.id.empty()) m.id = c.NewId('m');
    m.t = NeedTick(mj, "t", "tSec", c);
    m.name = OptStr(mj, "name");
    m.params = CollectParams(mj, { "id", "t", "tSec", "name" });
    return m;
}

Cut CutFrom(const J& cj, const Ctx& c)
{
    if (!cj.is_object()) Fail("cut はオブジェクトでなければならない");
    Cut k;
    k.id = OptStr(cj, "id");
    if (k.id.empty()) k.id = c.NewId('c');
    k.start = NeedTick(cj, "start", "startSec", c);
    k.end = NeedTick(cj, "end", "endSec", c);
    k.camera = NeedStr(cj, "camera");
    TickField(cj, "blend", "blendSec", c, k.blend);
    return k;
}

// ---- 補間 -----------------------------------------------------------------------
void InterpFrom(const J& j, const J& opj, Interp& ip, EaseId& ease)
{
    if (!j.is_string()) Fail("ip は文字列(s / l / a / b / e:<名前>)");
    std::string s = j.get<std::string>();
    ease = EaseId::Linear;
    if (s == "step") s = "s";
    else if (s == "linear") s = "l";
    else if (s == "auto") s = "a";
    else if (s == "bezier") s = "b";
    else if (s.rfind("ease:", 0) == 0) s = "e:" + s.substr(5);
    if (s == "s") ip = Interp::Step;
    else if (s == "l") ip = Interp::Linear;
    else if (s == "a") ip = Interp::Auto;
    else if (s == "b") ip = Interp::Bezier;
    else if (s.size() > 2 && s[0] == 'e' && s[1] == ':')
    {
        ip = Interp::Ease;
        if (!EaseFromName(std::string_view(s).substr(2), ease)) Fail("知らないイージング名: " + s);
    }
    else if (s == "e")
    {
        ip = Interp::Ease;
        const std::string en = OptStr(opj, "ease");
        if (en.empty() || !EaseFromName(en, ease)) Fail("イージング名(ease)が要る");
    }
    else Fail("知らない補間種別: " + s);
}

Extrap ExtrapFrom(const J& j, const char* what)
{
    Extrap e = Extrap::Hold;
    if (!j.is_string() || !ExtrapFromName(j.get<std::string>(), e)) Fail(std::string(what) + " は hold / linear / loop / pingpong");
    return e;
}

// ---- op --------------------------------------------------------------------------
SeqOp ParseOne(const J& oj, const Ctx& c)
{
    if (!oj.is_object()) Fail("op はオブジェクトでなければならない");
    const J* opf = Field(oj, "op");
    if (!opf || !opf->is_string()) Fail("\"op\" (op 名)が無い");
    const std::string want = Norm(opf->get<std::string>());

    static const char* const kNames[] = {
        "SetName", "SetFrameRate", "SetRange", "SetRender", "SetMeta",
        "AddBinding", "DeleteBinding", "MoveBinding", "SetBindingHeader",
        "AddTrack", "DeleteTrack", "MoveTrack", "SetTrackHeader",
        "AddChannel", "DeleteChannel", "SetChannelExtrap",
        "AddKey", "DeleteKey", "MoveKey", "SetKeyValue", "SetInterp", "SetTangent", "SetKey",
        "AddClip", "DeleteClip", "SetClip", "AddEvent", "DeleteEvent", "SetEvent",
        "AddMarker", "DeleteMarker", "SetMarker", "AddCut", "DeleteCut", "SetCut",
    };
    int which = -1;
    for (std::size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        if (Norm(kNames[i]) == want) { which = static_cast<int>(i); break; }
    if (which < 0)
    {
        std::string all;
        for (const char* n : kNames) { if (!all.empty()) all += ", "; all += n; }
        Fail("知らない op: " + opf->get<std::string>() + "(有効: " + all + ")");
    }

    const auto idx = [&](const char* k, int def) -> int {
        const J* f = Field(oj, k);
        return f ? IntOf(*f, k) : def;
    };

    switch (which)
    {
    case 0: { OpSetName o; o.name = NeedStr(oj, "name"); return o; }
    case 1: { OpSetFrameRate o; o.fps = IntOf(Need(oj, "fps"), "fps"); return o; }
    case 2:
    {
        OpSetRange o;
        Tick s = 0, e = 0;
        const bool hs = TickField(oj, "start", "startSec", c, s);
        const bool he = TickField(oj, "end", "endSec", c, e);
        o.has = (hs || he);
        if (const J* f = Field(oj, "has")) { if (!f->is_boolean()) Fail("has は真偽値"); o.has = f->get<bool>(); }
        if (o.has)
        {
            if (!hs || !he) Fail("範囲には start と end(または startSec / endSec)が要る");
            o.start = s; o.end = e;
        }
        return o;
    }
    case 3:
    {
        OpSetRender o;
        const J& r = Need(oj, "render");
        if (!r.is_null())
        {
            if (!r.is_object()) Fail("render はオブジェクトか null");
            RenderSettings rs = c.cur.render.value_or(RenderSettings{});
            if (const J* f = Field(r, "width")) rs.width = IntOf(*f, "render.width");
            if (const J* f = Field(r, "height")) rs.height = IntOf(*f, "render.height");
            if (const J* f = Field(r, "shutter")) rs.shutter = NumOf(*f, "render.shutter");
            if (const J* f = Field(r, "warmup")) rs.warmup = IntOf(*f, "render.warmup");
            o.render = rs;
        }
        return o;
    }
    case 4:
    {
        OpSetMeta o;
        o.meta = ToSeqValue(Need(oj, "meta"));
        if (o.meta.IsNull()) o.meta = SeqValue::MakeObject();
        if (!o.meta.IsObject()) Fail("meta はオブジェクト");
        return o;
    }
    case 5: { OpAddBinding o; o.binding = BindingFromJson(Need(oj, "binding"), c); o.index = idx("index", -1); return o; }
    case 6: { OpDeleteBinding o; o.id = NeedStr(oj, "id"); return o; }
    case 7: { OpMoveBinding o; o.id = NeedStr(oj, "id"); o.toIndex = IntOf(Need(oj, "toIndex"), "toIndex"); return o; }
    case 8:
    {
        OpSetBindingHeader o;
        o.id = NeedStr(oj, "id");
        BindingHeader base;
        const int bi = FindBinding(c.cur, o.id);
        if (bi >= 0) base = static_cast<const BindingHeader&>(c.cur.bindings[static_cast<std::size_t>(bi)]);
        o.header = BindingHeaderFrom(Need(oj, "header"), base);
        return o;
    }
    case 9:
    {
        OpAddTrack o;
        o.bindingId = NeedStr(oj, "bindingId");
        o.track = TrackFromJson(Need(oj, "track"), c);
        o.index = idx("index", -1);
        return o;
    }
    case 10: { OpDeleteTrack o; o.trackId = NeedStr(oj, "trackId"); return o; }
    case 11:
    {
        OpMoveTrack o;
        o.trackId = NeedStr(oj, "trackId");
        o.toBindingId = NeedStr(oj, "toBindingId");
        o.toIndex = IntOf(Need(oj, "toIndex"), "toIndex");
        return o;
    }
    case 12:
    {
        OpSetTrackHeader o;
        o.trackId = NeedStr(oj, "trackId");
        TrackHeader base;
        if (const Track* t = GetTrack(c.cur, o.trackId)) base = static_cast<const TrackHeader&>(*t);
        o.header = TrackHeaderFrom(Need(oj, "header"), base);
        return o;
    }
    case 13:
    {
        OpAddChannel o;
        o.trackId = NeedStr(oj, "trackId");
        const J& cj = Need(oj, "channel");
        if (!cj.is_object()) Fail("channel はオブジェクトでなければならない");
        const std::string name = NeedStr(cj, "name");
        J body = cj;
        body.erase("name");
        o.channel = ChannelFrom(name, body, c);
        o.index = idx("index", -1);
        return o;
    }
    case 14: { OpDeleteChannel o; o.trackId = NeedStr(oj, "trackId"); o.channel = NeedStr(oj, "channel"); return o; }
    case 15:
    {
        OpSetChannelExtrap o;
        o.trackId = NeedStr(oj, "trackId");
        o.channel = NeedStr(oj, "channel");
        // 省略した側は現在の値を保つ
        if (const Track* t = GetTrack(c.cur, o.trackId))
        {
            const int ci = FindChannel(*t, o.channel);
            if (ci >= 0) { o.pre = t->channels[static_cast<std::size_t>(ci)].pre; o.post = t->channels[static_cast<std::size_t>(ci)].post; }
        }
        if (const J* f = Field(oj, "pre")) o.pre = ExtrapFrom(*f, "pre");
        if (const J* f = Field(oj, "post")) o.post = ExtrapFrom(*f, "post");
        return o;
    }
    case 16:
    {
        OpAddKey o;
        o.trackId = NeedStr(oj, "trackId");
        o.channel = NeedStr(oj, "channel");
        o.key = KeyFrom(Need(oj, "key"), c);
        o.index = idx("index", -1);
        return o;
    }
    case 17:
    {
        OpDeleteKey o;
        o.trackId = NeedStr(oj, "trackId");
        o.channel = NeedStr(oj, "channel");
        o.index = IntOf(Need(oj, "index"), "index");
        return o;
    }
    case 18:
    {
        OpMoveKey o;
        o.trackId = NeedStr(oj, "trackId");
        o.channel = NeedStr(oj, "channel");
        o.index = IntOf(Need(oj, "index"), "index");
        o.newT = NeedTick(oj, "newT", "newSec", c);
        o.newIndex = idx("newIndex", -1);
        return o;
    }
    case 19:
    {
        OpSetKeyValue o;
        o.trackId = NeedStr(oj, "trackId");
        o.channel = NeedStr(oj, "channel");
        o.index = IntOf(Need(oj, "index"), "index");
        o.v = NumOf(Need(oj, "v"), "v");
        return o;
    }
    case 20:
    {
        OpSetInterp o;
        o.trackId = NeedStr(oj, "trackId");
        o.channel = NeedStr(oj, "channel");
        o.index = IntOf(Need(oj, "index"), "index");
        InterpFrom(Need(oj, "ip"), oj, o.ip, o.ease);
        return o;
    }
    case 21:
    {
        OpSetTangent o;
        o.trackId = NeedStr(oj, "trackId");
        o.channel = NeedStr(oj, "channel");
        o.index = IntOf(Need(oj, "index"), "index");
        o.inDt = TickFromJ(Need(oj, "inDt"), "inDt");
        o.inDv = NumOf(Need(oj, "inDv"), "inDv");
        o.outDt = TickFromJ(Need(oj, "outDt"), "outDt");
        o.outDv = NumOf(Need(oj, "outDv"), "outDv");
        return o;
    }
    case 22:
    {
        OpSetKey o;
        o.trackId = NeedStr(oj, "trackId");
        o.channel = NeedStr(oj, "channel");
        o.index = IntOf(Need(oj, "index"), "index");
        o.key = KeyFrom(Need(oj, "key"), c);
        return o;
    }
    case 23: { OpAddClip o; o.trackId = NeedStr(oj, "trackId"); o.clip = ClipFrom(Need(oj, "clip"), c); o.index = idx("index", -1); return o; }
    case 24: { OpDeleteClip o; o.trackId = NeedStr(oj, "trackId"); o.clipId = NeedStr(oj, "clipId"); return o; }
    case 25: { OpSetClip o; o.trackId = NeedStr(oj, "trackId"); o.clip = ClipFrom(Need(oj, "clip"), c); return o; }
    case 26: { OpAddEvent o; o.trackId = NeedStr(oj, "trackId"); o.event = EventFrom(Need(oj, "event"), c); o.index = idx("index", -1); return o; }
    case 27: { OpDeleteEvent o; o.trackId = NeedStr(oj, "trackId"); o.eventId = NeedStr(oj, "eventId"); return o; }
    case 28: { OpSetEvent o; o.trackId = NeedStr(oj, "trackId"); o.event = EventFrom(Need(oj, "event"), c); return o; }
    case 29: { OpAddMarker o; o.marker = MarkerFrom(Need(oj, "marker"), c); o.index = idx("index", -1); return o; }
    case 30: { OpDeleteMarker o; o.id = NeedStr(oj, "id"); return o; }
    case 31: { OpSetMarker o; o.marker = MarkerFrom(Need(oj, "marker"), c); return o; }
    case 32: { OpAddCut o; o.cut = CutFrom(Need(oj, "cut"), c); o.index = idx("index", -1); return o; }
    case 33: { OpDeleteCut o; o.id = NeedStr(oj, "id"); return o; }
    case 34: { OpSetCut o; o.cut = CutFrom(Need(oj, "cut"), c); return o; }
    default: break;
    }
    Fail("内部エラー: op の番号");
}

J ParseJsonText(std::string_view text)
{
    J j = J::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded()) Fail("JSON として読めない");
    return j;
}

} // namespace

OpParseResult ParseOpJson(std::string_view text, const Sequence& cur, const IdIssuer& ids, SeqOp& out)
{
    OpParseResult r;
    try
    {
        const J j = ParseJsonText(text);
        const Ctx c{ cur, ids, cur.ticksPerSecond };
        out = ParseOne(j, c);
        r.ok = true;
    }
    catch (const PErr& e) { r.error = e.msg; }
    return r;
}

OpParseResult ParseTxnJson(std::string_view text, const Sequence& cur, const IdIssuer& ids, SeqTxn& out)
{
    OpParseResult r;
    try
    {
        const J j = ParseJsonText(text);
        const J* arr = &j;
        SeqTxn txn;
        if (j.is_object())
        {
            if (const J* l = Field(j, "label")) { if (!l->is_string()) Fail("label は文字列"); txn.label = l->get<std::string>(); }
            arr = Field(j, "ops");
            if (!arr) Fail("\"ops\" が無い");
        }
        if (!arr->is_array()) Fail("op の配列でなければならない");
        const Ctx c{ cur, ids, cur.ticksPerSecond };
        std::size_t i = 0;
        for (const J& oj : *arr)
        {
            try { txn.ops.push_back(ParseOne(oj, c)); }
            catch (const PErr& e)
            {
                std::string nm = "op";
                if (oj.is_object()) if (const J* f = Field(oj, "op")) if (f->is_string()) nm = f->get<std::string>();
                Fail("ops[" + std::to_string(i) + "] " + nm + ": " + e.msg);
            }
            ++i;
        }
        out = std::move(txn);
        r.ok = true;
    }
    catch (const PErr& e) { r.error = e.msg; }
    return r;
}

std::vector<std::string> OpJsonNames()
{
    return {
        "SetName", "SetFrameRate", "SetRange", "SetRender", "SetMeta",
        "AddBinding", "DeleteBinding", "MoveBinding", "SetBindingHeader",
        "AddTrack", "DeleteTrack", "MoveTrack", "SetTrackHeader",
        "AddChannel", "DeleteChannel", "SetChannelExtrap",
        "AddKey", "DeleteKey", "MoveKey", "SetKeyValue", "SetInterp", "SetTangent", "SetKey",
        "AddClip", "DeleteClip", "SetClip", "AddEvent", "DeleteEvent", "SetEvent",
        "AddMarker", "DeleteMarker", "SetMarker", "AddCut", "DeleteCut", "SetCut",
    };
}

OpParseResult ParseBindingJson(std::string_view text, const Sequence& cur, const IdIssuer& ids, Binding& out)
{
    OpParseResult r;
    try
    {
        const J j = ParseJsonText(text);
        const Ctx c{ cur, ids, cur.ticksPerSecond };
        out = BindingFromJson(j, c);
        r.ok = true;
    }
    catch (const PErr& e) { r.error = e.msg; }
    return r;
}

OpParseResult ParseTrackJson(std::string_view text, const Sequence& cur, const IdIssuer& ids, Track& out)
{
    OpParseResult r;
    try
    {
        const J j = ParseJsonText(text);
        const Ctx c{ cur, ids, cur.ticksPerSecond };
        out = TrackFromJson(j, c);
        r.ok = true;
    }
    catch (const PErr& e) { r.error = e.msg; }
    return r;
}

} // namespace dx12e::seq
