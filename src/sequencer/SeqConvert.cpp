#include "sequencer/SeqConvert.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <nlohmann/json.hpp>

#include "sequencer/SeqCurve.h"

namespace dx12e::seq
{

int ConvertResult::Count(ConvertNote::Level l) const
{
    int n = 0;
    for (const auto& x : notes) if (x.level == l) ++n;
    return n;
}

namespace
{
using json = nlohmann::json;
using Level = ConvertNote::Level;
using Vec3 = std::array<double, 3>;

const char* const kTrackTypes[] = { "camera", "fade", "post", "timeScale", "shake", "vfx", "vfxPlay", "vfxStop",
                                    "shaderParam", "sound", "move", "rotate", "light", "event", "scene", "log" };

double DefaultDur(const std::string& type)
{
    if (type == "camera") return 2.0;
    if (type == "fade") return 0.6;
    if (type == "post") return 1.0;
    if (type == "shake") return 0.4;
    if (type == "move" || type == "rotate" || type == "shaderParam") return 1.0;
    if (type == "light") return 0.5;
    return 0.0;   // timeScale ほか
}

double NumOr(const json& j, const char* key, double def)
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : def;
}
std::string StrOr(const json& j, const char* key, const std::string& def = {})
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}
bool ReadVec3(const json& j, const char* key, Vec3& out)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 3) return false;
    for (std::size_t i = 0; i < 3; ++i)
    {
        if (!(*it)[i].is_number()) return false;
        out[i] = (*it)[i].get<double>();
    }
    return true;
}

SeqValue VecValue(const Vec3& v)
{
    SeqValue a = SeqValue::MakeArray();
    for (double d : v) a.arr.push_back(SeqValue::MakeNumber(d));
    return a;
}

struct Converter
{
    const ConvertOptions& opt;
    ConvertResult& res;
    Sequence seq;
    IdAllocator ids;
    std::int64_t tps = kDefaultTicksPerSecond;

    std::map<std::string, int> entityBinding;
    int sceneBinding = -1;
    std::map<std::string, std::pair<int, int>> trackCache;   // key → (binding, track)
    std::map<std::string, double> last;                      // 「直前トラックの終点値」= 旧式の grab が拾う値
    std::map<std::string, Tick> segEnd;                      // チャンネルごとの直前区間の終端(重なり検出)
    std::map<std::string, std::pair<int, std::pair<int, int>>> openEmitter;   // "target|layer" → (binding, (track, clip))

    Converter(const ConvertOptions& o, ConvertResult& r, std::uint64_t seed) : opt(o), res(r), ids(seed) {}

    void Note(Level l, int idx, std::string m) { res.notes.push_back({ l, idx, std::move(m) }); }
    Tick Ticks(double sec) const { return SecondsToTicks(sec, tps); }

    int Entity(const std::string& name)
    {
        const auto it = entityBinding.find(name);
        if (it != entityBinding.end()) return it->second;
        Binding b;
        b.id = ids.New('b');
        b.name = name;
        b.kind = BindingKind::Entity;
        b.hint.name = name;
        b.params = SeqValue::MakeObject();
        seq.bindings.push_back(std::move(b));
        const int idx = static_cast<int>(seq.bindings.size()) - 1;
        entityBinding[name] = idx;
        return idx;
    }
    int Scene()
    {
        if (sceneBinding >= 0) return sceneBinding;
        Binding b;
        b.id = "b_scene";
        b.name = "Scene";
        b.kind = BindingKind::Scene;
        b.params = SeqValue::MakeObject();
        seq.bindings.push_back(std::move(b));
        sceneBinding = static_cast<int>(seq.bindings.size()) - 1;
        return sceneBinding;
    }

    Track& GetOrMakeTrack(int b, TrackType type, const std::string& path = {}, ValueType vt = ValueType::Float)
    {
        const std::string key = std::to_string(b) + "|" + TrackTypeName(type) + "|" + path;
        const auto it = trackCache.find(key);
        if (it != trackCache.end())
            return seq.bindings[static_cast<std::size_t>(it->second.first)].tracks[static_cast<std::size_t>(it->second.second)];
        Track t;
        t.id = ids.New('t');
        t.type = type;
        t.path = path;
        t.valueType = vt;
        t.params = SeqValue::MakeObject();
        auto& tracks = seq.bindings[static_cast<std::size_t>(b)].tracks;
        tracks.push_back(std::move(t));
        trackCache[key] = { b, static_cast<int>(tracks.size()) - 1 };
        return tracks.back();
    }
    Track& NewTrack(int b, TrackType type)   // 毎回新規(Aim など)
    {
        Track t;
        t.id = ids.New('t');
        t.type = type;
        t.params = SeqValue::MakeObject();
        auto& tracks = seq.bindings[static_cast<std::size_t>(b)].tracks;
        tracks.push_back(std::move(t));
        return tracks.back();
    }

    static Channel& Chan(Track& t, const std::string& name)
    {
        const int i = FindChannel(t, name);
        if (i >= 0) return t.channels[static_cast<std::size_t>(i)];
        Channel c;
        c.name = name;
        t.channels.push_back(std::move(c));
        return t.channels.back();
    }
    static void PutKey(Channel& ch, Key k)
    {
        NormalizeKey(k);
        const auto it = std::upper_bound(ch.keys.begin(), ch.keys.end(), k.t, [](Tick v, const Key& kk) { return v < kk.t; });
        ch.keys.insert(it, k);
    }

    // 旧式の「from → to を dur かけてイージング」を 1 チャンネルのキーへ。
    //  from あり: [t0, from, ease] + [t1, to, Step]   from なし: [t1, to, Step](開始値不明 = 終点にホールド)
    //  dur == 0: [t0, to, Step]
    void Segment(Channel& ch, const std::string& segKey, int trackIdx, Tick t0, Tick t1, std::optional<double> from, double to,
                 EaseId ease)
    {
        const auto se = segEnd.find(segKey);
        if (se != segEnd.end() && t0 < se->second)
            Note(Level::Warning, trackIdx, "同じ対象(" + segKey + ")の区間が重なっている。旧式は後のトラックが毎フレーム上書きするので、前の動きは見えなくなる");
        segEnd[segKey] = t1;

        Key end;
        end.t = t1; end.v = to; end.ip = Interp::Step;
        if (t1 <= t0)
        {
            end.t = t0;
            PutKey(ch, end);
            return;
        }
        if (from)
        {
            Key start;
            start.t = t0; start.v = *from;
            if (ease == EaseId::Linear) start.ip = Interp::Linear;
            else { start.ip = Interp::Ease; start.ease = ease; }
            PutKey(ch, start);
        }
        PutKey(ch, end);
    }

    static EaseId ParseEase(const json& j, EaseId def)
    {
        const auto it = j.find("ease");
        if (it == j.end() || !it->is_string()) return def;
        EaseId e;
        return EaseFromName(it->get<std::string>(), e) ? e : def;
    }

    std::optional<double> LastOr(const std::string& key, const std::map<std::string, double>& init, const std::string& initKey)
    {
        const auto l = last.find(key);
        if (l != last.end()) return l->second;
        const auto i = init.find(initKey);
        if (i != init.end()) return i->second;
        return std::nullopt;
    }
    std::optional<Vec3> LastVec(const std::string& prefix, const std::string& name,
                                const std::map<std::string, Vec3>& init)
    {
        Vec3 v{};
        bool all = true;
        static const char* axes[3] = { "x", "y", "z" };
        for (int i = 0; i < 3; ++i)
        {
            const auto l = last.find(prefix + ":" + name + ":" + axes[i]);
            if (l == last.end()) { all = false; break; }
            v[static_cast<std::size_t>(i)] = l->second;
        }
        if (all) return v;
        const auto it = init.find(name);
        if (it != init.end()) return it->second;
        return std::nullopt;
    }
    void SetLastVec(const std::string& prefix, const std::string& name, const Vec3& v)
    {
        static const char* axes[3] = { "x", "y", "z" };
        for (int i = 0; i < 3; ++i) last[prefix + ":" + name + ":" + axes[i]] = v[static_cast<std::size_t>(i)];
    }

    // 3 成分の位置/回転を 1 区間ぶん置く
    void Vec3Segment(Track& t, const char* base, const std::string& segPrefix, int idx, Tick t0, Tick t1,
                     const std::optional<Vec3>& from, const Vec3& to, EaseId ease)
    {
        static const char* axes[3] = { "x", "y", "z" };
        for (int i = 0; i < 3; ++i)
        {
            const std::string name = std::string(base) + "." + axes[i];
            std::optional<double> f;
            if (from) f = (*from)[static_cast<std::size_t>(i)];
            Segment(Chan(t, name), segPrefix + name, idx, t0, t1, f, to[static_cast<std::size_t>(i)], ease);
        }
    }
};

} // namespace

ConvertResult ConvertLegacySpec(std::string_view specJson, const ConvertOptions& opt, Sequence& out)
{
    ConvertResult res;
    json spec = json::parse(specJson.begin(), specJson.end(), nullptr, false);
    if (spec.is_discarded() || !spec.is_object())
    {
        res.error = "台本が JSON オブジェクトとして読めない";
        return res;
    }
    const std::string name = StrOr(spec, "name");
    {
        bool ok = !name.empty() && !(name[0] >= '0' && name[0] <= '9');
        for (char c : name)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) ok = false;
        if (!ok) { res.error = "name は英数字とアンダースコアだけ(先頭は数字以外)"; return res; }
    }
    const auto tracksIt = spec.find("tracks");
    if (tracksIt == spec.end() || !tracksIt->is_array() || tracksIt->empty())
    {
        res.error = "tracks が空";
        return res;
    }
    if (opt.fps < 1 || opt.fps > 1000) { res.error = "fps が不正"; return res; }

    Converter cv(opt, res, Fnv1a64(name));
    cv.ids.Reserve("b_scene");
    cv.seq.name = name;
    cv.seq.id = cv.ids.New('q');
    cv.seq.frameRate = opt.fps;
    cv.seq.meta = SeqValue::MakeObject();
    const std::string camName = StrOr(spec, "camera");

    // 台本を t 昇順(安定)に並べる。不正なトラックはここで捨てる。
    struct Item { int index; double t; const json* j; std::string type; double dur; };
    std::vector<Item> items;
    double duration = 0.0;
    for (std::size_t i = 0; i < tracksIt->size(); ++i)
    {
        const json& tj = (*tracksIt)[i];
        const int idx = static_cast<int>(i);
        if (!tj.is_object()) { cv.Note(Level::Error, idx, "トラックがオブジェクトでない"); continue; }
        const std::string type = StrOr(tj, "type");
        if (std::find(std::begin(kTrackTypes), std::end(kTrackTypes), type) == std::end(kTrackTypes))
        {
            cv.Note(Level::Error, idx, "知らない type: " + type);
            continue;
        }
        const double t = NumOr(tj, "t", -1.0);
        if (!(t >= 0.0) || !std::isfinite(t)) { cv.Note(Level::Error, idx, "t は 0 以上の秒でなければならない"); continue; }
        const double dur = std::max(0.0, NumOr(tj, "dur", DefaultDur(type)));
        duration = std::max(duration, t + dur);
        items.push_back({ idx, t, &tj, type, dur });
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.t < b.t; });
    const Tick totalTicks = cv.Ticks(duration);

    bool seenFade = false;
    bool usedCamera = false;
    std::string camBindingId;

    for (const Item& it : items)
    {
        const json& j = *it.j;
        const int idx = it.index;
        const Tick t0 = cv.Ticks(it.t);
        const Tick t1 = cv.Ticks(it.t + it.dur);
        const std::string& type = it.type;

        if (type == "camera")
        {
            if (camName.empty()) { cv.Note(Level::Error, idx, "camera トラックがあるのに spec.camera が無い"); continue; }
            Vec3 to{};
            if (!ReadVec3(j, "to", to)) { cv.Note(Level::Error, idx, "to は [x,y,z]"); continue; }
            const int cb = cv.Entity(camName);
            usedCamera = true;
            camBindingId = cv.seq.bindings[static_cast<std::size_t>(cb)].id;
            Track& tr = cv.GetOrMakeTrack(cb, TrackType::Transform);
            Vec3 fv{};
            std::optional<Vec3> from;
            if (ReadVec3(j, "from", fv)) from = fv;
            else from = cv.LastVec("pos", camName, opt.initialPosition);
            if (!from)
                cv.Note(Level::Lossy, idx, "camera の from が無く、開始位置(旧式は再生開始時のカメラ位置)を知る手段が無いため開始キーを打たなかった。"
                                           "ConvertOptions::initialPosition[\"" + camName + "\"] を渡すと焼ける(このままだと終点に留まる)");
            cv.Vec3Segment(tr, "position", "pos:" + camName + ":", idx, t0, t1, from, to, Converter::ParseEase(j, EaseId::InOutQuad));
            cv.SetLastVec("pos", camName, to);

            // 注視: lookAtName が優先(旧式と同じ)。区間 [t0,t1] の間だけ効く制約として aim トラックへ。
            Vec3 look{};
            const std::string lookName = StrOr(j, "lookAtName");
            const bool hasLookPoint = ReadVec3(j, "lookAt", look);
            if (!lookName.empty() && hasLookPoint)
                cv.Note(Level::Warning, idx, "lookAt と lookAtName の両方がある。lookAtName を優先した");
            if (!lookName.empty() || hasLookPoint)
            {
                // ★Entity() は bindings を伸ばして Track& を無効にし得るので、先にバインディングを作ってからトラックを取る
                SeqValue target = SeqValue::MakeObject();
                if (!lookName.empty())
                {
                    const int lb = cv.Entity(lookName);
                    target.Set("binding", SeqValue::MakeString(cv.seq.bindings[static_cast<std::size_t>(lb)].id));
                }
                else target.Set("point", VecValue(look));
                Track& aim = cv.NewTrack(cb, TrackType::Aim);
                aim.params.Set("target", std::move(target));
                aim.params.Set("offset", VecValue({ 0.0, 0.0, 0.0 }));
                aim.params.Set("start", SeqValue::MakeNumber(static_cast<double>(t0)));
                aim.params.Set("end", SeqValue::MakeNumber(static_cast<double>(t1)));
                cv.Note(Level::Info, idx, "注視は aim トラック(制約族。S2a で評価を実装)。start/end はこの旧トラックの区間(ティック)。"
                                           "旧式は区間が終わるとその向きを保持するが、新式の aim は区間後の扱いを S2a で決める");
            }
            else
            {
                cv.Note(Level::Warning, idx, "注視点が無い(旧式でも向きは変わらない)");
            }
        }
        else if (type == "fade")
        {
            const std::string to = StrOr(j, "to");
            if (to != "black" && to != "white" && to != "clear") { cv.Note(Level::Error, idx, "to は black / white / clear"); continue; }
            const double toV = (to == "black") ? 0.0 : (to == "white" ? 8.0 : 1.0);
            const bool openFromBlack = (to == "clear") && !seenFade;
            seenFade = true;
            const int sb = cv.Scene();
            Track& tr = cv.GetOrMakeTrack(sb, TrackType::Post);
            std::optional<double> from;
            if (openFromBlack) from = 0.0;
            else
            {
                from = cv.LastOr("post:exposure", opt.initialPost, "exposure");
                if (!from)
                {
                    from = opt.initialExposure;
                    cv.Note(Level::Info, idx, "fade の開始露出は再生時のシーン値だが不明なので " + std::to_string(opt.initialExposure) + " と仮定した");
                }
            }
            cv.Segment(Converter::Chan(tr, "exposure"), "post:exposure", idx, t0, t1, from, toV, EaseId::Linear);
            cv.last["post:exposure"] = toV;
        }
        else if (type == "post")
        {
            const auto setIt = j.find("set");
            if (setIt == j.end() || !setIt->is_object() || setIt->empty()) { cv.Note(Level::Error, idx, "set が空"); continue; }
            const int sb = cv.Scene();
            const EaseId ease = Converter::ParseEase(j, EaseId::InOutQuad);
            for (auto p = setIt->begin(); p != setIt->end(); ++p)
            {
                if (!p.value().is_number()) { cv.Note(Level::Error, idx, "post の " + p.key() + " は数値のみ"); continue; }
                Track& tr = cv.GetOrMakeTrack(sb, TrackType::Post);
                const auto from = cv.LastOr("post:" + p.key(), opt.initialPost, p.key());
                if (!from)
                    cv.Note(Level::Lossy, idx, "post." + p.key() + " の開始値(旧式は再生時のシーン値)が不明なため開始キーを打たなかった。"
                                               "ConvertOptions::initialPost[\"" + p.key() + "\"] で焼ける");
                cv.Segment(Converter::Chan(tr, p.key()), "post:" + p.key(), idx, t0, t1, from, p.value().get<double>(), ease);
                cv.last["post:" + p.key()] = p.value().get<double>();
            }
        }
        else if (type == "timeScale")
        {
            const double value = NumOr(j, "value", -1.0);
            if (!(value >= 0.0)) { cv.Note(Level::Error, idx, "value は 0 以上"); continue; }
            Track& tr = cv.GetOrMakeTrack(cv.Scene(), TrackType::TimeScale);
            const std::optional<double> from = cv.last.count("timeScale") ? std::optional<double>(cv.last["timeScale"])
                                                                          : std::optional<double>(opt.initialTimeScale);
            cv.Segment(Converter::Chan(tr, "value"), "timeScale", idx, t0, t1, from, value, EaseId::Linear);
            cv.last["timeScale"] = value;
        }
        else if (type == "shake")
        {
            if (camName.empty())
            {
                cv.Note(Level::Warning, idx, "shake は spec.camera が無いと旧式でも何も起きない。変換しなかった");
                continue;
            }
            const int cb = cv.Entity(camName);
            Track& tr = cv.GetOrMakeTrack(cb, TrackType::Shake);
            Clip c;
            c.id = cv.ids.New('k');
            c.start = t0;
            c.dur = cv.Ticks(it.dur > 0.0 ? it.dur : 0.4);
            c.params = SeqValue::MakeObject();
            c.params.Set("amp", SeqValue::MakeNumber(NumOr(j, "amp", 0.25)));
            c.params.Set("freq", SeqValue::MakeNumber(NumOr(j, "freq", 22.0)));
            c.params.Set("seed", SeqValue::MakeNumber(0.0));
            tr.clips.push_back(std::move(c));
            cv.Note(Level::Lossy, idx, "shake の波形: 旧式は sin の位相をシーケンスの経過時間から取る(揺れ幅は残り時間の 2 乗で減衰)。"
                                       "新式は seed 固定の純関数で、位相・減衰式が同一とは限らない(S2a で式を旧式に合わせる予定)");
        }
        else if (type == "vfx")
        {
            const std::string preset = StrOr(j, "preset");
            const std::string atName = StrOr(j, "atName");
            Vec3 at{};
            const bool hasAt = ReadVec3(j, "at", at);
            if (preset.empty()) { cv.Note(Level::Error, idx, "preset が無い"); continue; }
            if (atName.empty() && !hasAt) { cv.Note(Level::Error, idx, "at か atName が要る"); continue; }
            const int b = atName.empty() ? cv.Scene() : cv.Entity(atName);
            Track& tr = cv.GetOrMakeTrack(b, TrackType::VfxSpawn);
            Clip c;
            c.id = cv.ids.New('k');
            c.start = t0;
            c.dur = 0;
            c.params = SeqValue::MakeObject();
            c.params.Set("mode", SeqValue::MakeString("burst"));
            c.params.Set("preset", SeqValue::MakeString(preset));
            c.params.Set("scale", SeqValue::MakeNumber(NumOr(j, "scale", 1.0)));
            if (atName.empty()) c.params.Set("position", VecValue(at));
            tr.clips.push_back(std::move(c));
        }
        else if (type == "vfxPlay" || type == "vfxStop")
        {
            const std::string target = StrOr(j, "target");
            if (target.empty()) { cv.Note(Level::Error, idx, "target が要る"); continue; }
            const std::string layer = StrOr(j, "layer");
            const std::string key = target + "|" + layer;
            // ★vfxStop では Entity() を呼ばない(対応する vfxPlay が無いのにバインディングだけ増えるのを避ける)
            if (type == "vfxPlay")
            {
                const int b = cv.Entity(target);
                Track& tr = cv.GetOrMakeTrack(b, TrackType::VfxSpawn);
                Clip c;
                c.id = cv.ids.New('k');
                c.start = t0;
                c.dur = 0;
                c.params = SeqValue::MakeObject();
                c.params.Set("mode", SeqValue::MakeString("emitter"));
                if (!layer.empty()) c.params.Set("layer", SeqValue::MakeString(layer));
                tr.clips.push_back(std::move(c));
                // GetOrMakeTrack は既存トラックを返すことがあるので、実際の位置を引き直す
                const TrackLoc loc = FindTrack(cv.seq, tr.id);
                cv.openEmitter[key] = { loc.binding, { loc.track, static_cast<int>(tr.clips.size()) - 1 } };
            }
            else
            {
                const auto o = cv.openEmitter.find(key);
                if (o == cv.openEmitter.end())
                {
                    cv.Note(Level::Warning, idx, "vfxStop(" + target + ")に対応する vfxPlay が無い。変換しなかった");
                    continue;
                }
                Clip& c = cv.seq.bindings[static_cast<std::size_t>(o->second.first)]
                              .tracks[static_cast<std::size_t>(o->second.second.first)]
                              .clips[static_cast<std::size_t>(o->second.second.second)];
                c.dur = std::max<Tick>(0, t0 - c.start);
                cv.openEmitter.erase(o);
            }
        }
        else if (type == "shaderParam")
        {
            const std::string target = StrOr(j, "target");
            const std::string param = StrOr(j, "param");
            if (target.empty()) { cv.Note(Level::Error, idx, "target が要る"); continue; }
            static const char* const params[] = { "effect", "p1", "p2", "p3", "p4", "b1", "b2", "b3" };
            if (std::find(std::begin(params), std::end(params), param) == std::end(params)) { cv.Note(Level::Error, idx, "param が不正: " + param); continue; }
            const auto toIt = j.find("to");
            if (toIt == j.end() || !toIt->is_number()) { cv.Note(Level::Error, idx, "to は数値"); continue; }
            const int b = cv.Entity(target);
            Track& tr = cv.GetOrMakeTrack(b, TrackType::Property, "MeshRenderer." + param);
            std::optional<double> from;
            const auto f = j.find("from");
            if (f != j.end() && f->is_number()) from = f->get<double>();
            else from = cv.LastOr("shader:" + target + "." + param, opt.initialShader, target + "." + param);
            if (!from)
                cv.Note(Level::Lossy, idx, "shaderParam の from が無く開始値(旧式は shader.get の現在値)が不明なため開始キーを打たなかった。"
                                           "ConvertOptions::initialShader[\"" + target + "." + param + "\"] で焼ける");
            cv.Segment(Converter::Chan(tr, "value"), "shader:" + target + "." + param, idx, t0, t1, from, toIt->get<double>(),
                       Converter::ParseEase(j, EaseId::InOutQuad));
            cv.last["shader:" + target + "." + param] = toIt->get<double>();
        }
        else if (type == "sound")
        {
            const std::string path = StrOr(j, "path");
            if (path.empty()) { cv.Note(Level::Error, idx, "path が空"); continue; }
            Track& tr = cv.GetOrMakeTrack(cv.Scene(), TrackType::Audio);
            const bool bgm = j.contains("bgm") && j["bgm"].is_boolean() && j["bgm"].get<bool>();
            Clip c;
            c.id = cv.ids.New('k');
            c.start = t0;
            c.dur = 0;
            c.params = SeqValue::MakeObject();
            c.params.Set("path", SeqValue::MakeString(path));
            if (bgm)
            {
                c.params.Set("bus", SeqValue::MakeString("music"));
                const bool loop = !(j.contains("loop") && j["loop"].is_boolean() && !j["loop"].get<bool>());   // 旧式: loop !== false
                c.params.Set("loop", SeqValue::MakeBool(loop));
            }
            else
            {
                c.params.Set("volume", SeqValue::MakeNumber(NumOr(j, "volume", 1.0)));
                const bool loop = j.contains("loop") && j["loop"].is_boolean() && j["loop"].get<bool>();       // 旧式: loop === true
                c.params.Set("loop", SeqValue::MakeBool(loop));
            }
            tr.clips.push_back(std::move(c));
            cv.Note(Level::Info, idx, "sound: 音声の長さが不明なので dur=0(点)。S4 で音声アセットの長さに解決する。bgm は bus=\"music\"(旧式の audio:playBGM 相当)、sfx は既定バス");
        }
        else if (type == "move")
        {
            const std::string target = StrOr(j, "target");
            Vec3 to{};
            if (target.empty()) { cv.Note(Level::Error, idx, "target が要る"); continue; }
            if (!ReadVec3(j, "to", to)) { cv.Note(Level::Error, idx, "to は [x,y,z]"); continue; }
            const int b = cv.Entity(target);
            Track& tr = cv.GetOrMakeTrack(b, TrackType::Transform);
            Vec3 fv{};
            std::optional<Vec3> from;
            if (ReadVec3(j, "from", fv)) from = fv;
            else from = cv.LastVec("pos", target, opt.initialPosition);
            if (!from)
                cv.Note(Level::Lossy, idx, "move の from が無く開始位置が不明なため開始キーを打たなかった。ConvertOptions::initialPosition[\"" + target + "\"] で焼ける");
            cv.Vec3Segment(tr, "position", "pos:" + target + ":", idx, t0, t1, from, to, Converter::ParseEase(j, EaseId::InOutQuad));
            cv.SetLastVec("pos", target, to);
        }
        else if (type == "rotate")
        {
            const std::string target = StrOr(j, "target");
            Vec3 to{};
            if (target.empty()) { cv.Note(Level::Error, idx, "target が要る"); continue; }
            if (!ReadVec3(j, "to", to)) { cv.Note(Level::Error, idx, "to は [x,y,z]"); continue; }
            const int b = cv.Entity(target);
            Track& tr = cv.GetOrMakeTrack(b, TrackType::Transform);
            const auto from = cv.LastVec("rot", target, opt.initialRotation);
            if (!from)
                cv.Note(Level::Lossy, idx, "rotate の開始回転が不明なため開始キーを打たなかった。ConvertOptions::initialRotation[\"" + target + "\"] で焼ける");
            cv.Vec3Segment(tr, "rotation", "rot:" + target + ":", idx, t0, t1, from, to, Converter::ParseEase(j, EaseId::InOutQuad));
            cv.SetLastVec("rot", target, to);
        }
        else if (type == "light")
        {
            const std::string target = StrOr(j, "target");
            if (target.empty()) { cv.Note(Level::Error, idx, "target が要る"); continue; }
            const bool hasI = j.contains("intensity") && j["intensity"].is_number();
            Vec3 col{};
            const bool hasC = ReadVec3(j, "color", col);
            if (!hasI && !hasC) { cv.Note(Level::Error, idx, "intensity か color が要る"); continue; }
            const int b = cv.Entity(target);
            const Tick lt1 = cv.Ticks(it.t + (it.dur > 0.0 ? it.dur : 0.5));
            if (hasI)
            {
                Track& tr = cv.GetOrMakeTrack(b, TrackType::Property, "Light.intensity");
                cv.Note(Level::Lossy, idx, "light.intensity の開始値(旧式は Tween の開始時の現在値)が不明なため開始キーを打たなかった");
                cv.Segment(Converter::Chan(tr, "value"), "light:" + target + ".intensity", idx, t0, lt1, cv.LastOr("light:" + target + ".intensity", {}, ""),
                           j["intensity"].get<double>(), EaseId::OutQuad);
                cv.last["light:" + target + ".intensity"] = j["intensity"].get<double>();
            }
            if (hasC)
            {
                Track& tr = cv.GetOrMakeTrack(b, TrackType::Property, "Light.color", ValueType::Color);
                static const char* rgb[3] = { "r", "g", "b" };
                for (int i = 0; i < 3; ++i)
                    cv.Segment(Converter::Chan(tr, rgb[i]), std::string("light:") + target + ".color." + rgb[i], idx, t0, lt1,
                               cv.LastOr(std::string("light:") + target + ".color." + rgb[i], {}, ""), col[static_cast<std::size_t>(i)],
                               EaseId::OutQuad);
                for (int i = 0; i < 3; ++i) cv.last[std::string("light:") + target + ".color." + rgb[i]] = col[static_cast<std::size_t>(i)];
                cv.Note(Level::Lossy, idx, "light.color の開始値が不明なため開始キーを打たなかった");
            }
            cv.Note(Level::Info, idx, "light: 旧式の Lighting.tweenIntensity/Color は outQuad(スケール済み dt = timeScale の影響を受ける)。"
                                       "ライトの種類は Light.* の総称パスで持つ(S5 で PointLight/SpotLight/DirectionalLight へ解決)");
        }
        else if (type == "event")
        {
            const std::string en = StrOr(j, "name");
            if (en.empty()) { cv.Note(Level::Error, idx, "name(イベント名)が要る"); continue; }
            Track& tr = cv.GetOrMakeTrack(cv.Scene(), TrackType::Event);
            EventItem e;
            e.id = cv.ids.New('e');
            e.t = t0;
            e.kind = "emit";
            e.name = en;
            e.params = SeqValue::MakeObject();
            SeqValue data = SeqValue::MakeObject();
            data.Set("value", SeqValue::MakeNumber(NumOr(j, "value", 0.0)));
            e.params.Set("data", std::move(data));
            tr.events.push_back(std::move(e));
        }
        else if (type == "scene")
        {
            const std::string path = StrOr(j, "path");
            if (path.empty()) { cv.Note(Level::Error, idx, "path が空"); continue; }
            Track& tr = cv.GetOrMakeTrack(cv.Scene(), TrackType::Event);
            EventItem e;
            e.id = cv.ids.New('e');
            e.t = t0;
            e.kind = "loadScene";
            e.name = path;
            e.params = SeqValue::MakeObject();
            e.params.Set("fade", SeqValue::MakeNumber(NumOr(j, "fade", 0.6)));
            tr.events.push_back(std::move(e));
        }
        else if (type == "log")
        {
            const std::string text = StrOr(j, "text");
            if (text.empty()) { cv.Note(Level::Error, idx, "text が空"); continue; }
            Track& tr = cv.GetOrMakeTrack(cv.Scene(), TrackType::Event);
            EventItem e;
            e.id = cv.ids.New('e');
            e.t = t0;
            e.kind = "log";
            e.name = text;
            e.params = SeqValue::MakeObject();
            tr.events.push_back(std::move(e));
        }
    }

    // 閉じられなかった emitter は、旧式では終了後も放出し続ける → 終端までにする
    for (const auto& o : cv.openEmitter)
    {
        Clip& c = cv.seq.bindings[static_cast<std::size_t>(o.second.first)]
                      .tracks[static_cast<std::size_t>(o.second.second.first)]
                      .clips[static_cast<std::size_t>(o.second.second.second)];
        c.dur = std::max<Tick>(0, totalTicks - c.start);
        cv.Note(Level::Lossy, -1, "vfxPlay(" + o.first.substr(0, o.first.find('|')) + ")に対応する vfxStop が無い。旧式は終了後も放出し続けるが、"
                                   "新式はシーケンス終端で止める(dur を終端までにした)");
    }

    cv.seq.hasRange = true;
    cv.seq.rangeStart = 0;
    cv.seq.rangeEnd = totalTicks;
    if (usedCamera)
    {
        Cut c;
        c.id = cv.ids.New('c');
        c.start = 0;
        c.end = totalTicks;
        c.camera = camBindingId;
        cv.seq.cuts.push_back(std::move(c));
        cv.Note(Level::Info, -1, "camera トラックがあるので、全区間を 1 つのカット(カメラ = " + camName + ")にした(旧式は isActive を触らない。設計 §7.2 の写像)");
    }
    SeqValue legacy = SeqValue::MakeObject();
    if (!camName.empty()) legacy.Set("camera", SeqValue::MakeString(camName));
    if (spec.contains("loop") && spec["loop"].is_boolean()) legacy.Set("loop", SeqValue::MakeBool(spec["loop"].get<bool>()));
    const std::string done = StrOr(spec, "doneEvent");
    if (!done.empty()) legacy.Set("doneEvent", SeqValue::MakeString(done));
    cv.seq.meta.Set("legacy", std::move(legacy));
    cv.Note(Level::Info, -1, "旧式の時計は time.realDt(スケール非適用)。SequencePlayer は clock=\"real\" で作ること。終了時の timeScale=1.0 復帰・"
                              "<name>:done イベントは SequencePlayer(restoreOnEnd / doneEvent)の責務");

    // 決定性のため、トラックの中のクリップ/イベントを時刻順に整える(同時刻は生成順のまま)
    for (auto& b : cv.seq.bindings)
        for (auto& t : b.tracks)
        {
            std::stable_sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& c) { return a.start < c.start; });
            std::stable_sort(t.events.begin(), t.events.end(), [](const EventItem& a, const EventItem& c) { return a.t < c.t; });
        }

    if (std::string e = CheckSequenceStructure(cv.seq); !e.empty())
    {
        res.error = "変換結果が構造検査に通らない(バグ): " + e;
        return res;
    }
    out = std::move(cv.seq);
    res.ok = true;
    return res;
}

} // namespace dx12e::seq
