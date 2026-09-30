#include "sequencer/SeqModel.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace dx12e::seq
{

// ---------------------------------------------------------------------------
// 比較
// ---------------------------------------------------------------------------
bool operator==(const Key& a, const Key& b)
{
    return a.t == b.t && a.v == b.v && a.ip == b.ip && a.ease == b.ease
        && a.inDt == b.inDt && a.outDt == b.outDt && a.inDv == b.inDv && a.outDv == b.outDv;
}
bool operator==(const BindingHint& a, const BindingHint& b)
{
    return a.guid == b.guid && a.path == b.path && a.name == b.name;
}
bool operator==(const RenderSettings& a, const RenderSettings& b)
{
    return a.width == b.width && a.height == b.height && a.shutter == b.shutter && a.warmup == b.warmup;
}

// ---------------------------------------------------------------------------
// キー
// ---------------------------------------------------------------------------
void NormalizeKey(Key& k)
{
    if (k.v == 0.0) k.v = 0.0;               // -0 → +0
    if (k.ip == Interp::Ease && k.ease == EaseId::Linear) k.ip = Interp::Linear;
    if (k.ip != Interp::Ease) k.ease = EaseId::Linear;
    if (k.ip != Interp::Bezier)
    {
        k.inDt = 0; k.outDt = 0; k.inDv = 0.0; k.outDv = 0.0;
    }
    else
    {
        if (k.inDv == 0.0) k.inDv = 0.0;
        if (k.outDv == 0.0) k.outDv = 0.0;
    }
}

std::string CheckKey(const Key& k)
{
    if (k.t < -kMaxTick || k.t > kMaxTick) return "キーの時刻が範囲外";
    if (!std::isfinite(k.v)) return "キーの値が有限でない(NaN / inf)";
    if (static_cast<int>(k.ip) > static_cast<int>(Interp::Ease)) return "キーの補間種別が不正";
    if (static_cast<int>(k.ease) >= static_cast<int>(EaseId::Count)) return "キーのイージングが不正";
    if (k.ip == Interp::Bezier)
    {
        if (k.inDt < 0 || k.outDt < 0) return "ベジェのハンドル dt は 0 以上";
        if (k.inDt > kMaxTick || k.outDt > kMaxTick) return "ベジェのハンドル dt が範囲外";
        if (!std::isfinite(k.inDv) || !std::isfinite(k.outDv)) return "ベジェのハンドル dv が有限でない";
    }
    return {};
}

// ---------------------------------------------------------------------------
// 種別名
// ---------------------------------------------------------------------------
namespace
{
struct TypeInfo { TrackType t; const char* name; TrackFamily fam; };
constexpr TypeInfo kTypes[] = {
    { TrackType::Transform,     "transform",  TrackFamily::Curve },
    { TrackType::Property,      "property",   TrackFamily::Curve },
    { TrackType::Camera,        "camera",     TrackFamily::Curve },
    { TrackType::Post,          "post",       TrackFamily::Curve },
    { TrackType::TimeScale,     "timeScale",  TrackFamily::Curve },
    { TrackType::Light,         "light",      TrackFamily::Curve },
    { TrackType::AnimationClip, "animation",  TrackFamily::Clip },
    { TrackType::Audio,         "audio",      TrackFamily::Clip },
    { TrackType::VfxSpawn,      "vfx",        TrackFamily::Clip },
    { TrackType::Shake,         "shake",      TrackFamily::Clip },
    { TrackType::Subsequence,   "subsequence",TrackFamily::Clip },
    { TrackType::Event,         "event",      TrackFamily::Event },
    { TrackType::Aim,           "aim",        TrackFamily::Constraint },
};
static_assert(sizeof(kTypes) / sizeof(kTypes[0]) == static_cast<std::size_t>(TrackType::Count),
              "TrackType を足したら kTypes も更新すること");
} // namespace

TrackFamily FamilyOf(TrackType t)
{
    for (const auto& i : kTypes) if (i.t == t) return i.fam;
    return TrackFamily::Curve;
}
const char* TrackTypeName(TrackType t)
{
    for (const auto& i : kTypes) if (i.t == t) return i.name;
    return "transform";
}
bool TrackTypeFromName(std::string_view s, TrackType& out)
{
    for (const auto& i : kTypes) if (s == i.name) { out = i.t; return true; }
    return false;
}

const char* BindingKindName(BindingKind k)
{
    switch (k)
    {
    case BindingKind::Scene:     return "scene";
    case BindingKind::Spawnable: return "spawnable";
    default:                     return "entity";
    }
}
bool BindingKindFromName(std::string_view s, BindingKind& out)
{
    if (s == "entity")    { out = BindingKind::Entity;    return true; }
    if (s == "scene")     { out = BindingKind::Scene;     return true; }
    if (s == "spawnable") { out = BindingKind::Spawnable; return true; }
    return false;
}
const char* ExtrapName(Extrap e)
{
    switch (e)
    {
    case Extrap::Linear:   return "linear";
    case Extrap::Loop:     return "loop";
    case Extrap::PingPong: return "pingpong";
    default:               return "hold";
    }
}
bool ExtrapFromName(std::string_view s, Extrap& out)
{
    if (s == "hold")     { out = Extrap::Hold;     return true; }
    if (s == "linear")   { out = Extrap::Linear;   return true; }
    if (s == "loop")     { out = Extrap::Loop;     return true; }
    if (s == "pingpong") { out = Extrap::PingPong; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// 検索
// ---------------------------------------------------------------------------
int FindBinding(const Sequence& s, std::string_view id)
{
    for (std::size_t i = 0; i < s.bindings.size(); ++i)
        if (s.bindings[i].id == id) return static_cast<int>(i);
    return -1;
}

TrackLoc FindTrack(const Sequence& s, std::string_view trackId)
{
    for (std::size_t b = 0; b < s.bindings.size(); ++b)
        for (std::size_t t = 0; t < s.bindings[b].tracks.size(); ++t)
            if (s.bindings[b].tracks[t].id == trackId)
                return { static_cast<int>(b), static_cast<int>(t) };
    return {};
}

Track* GetTrack(Sequence& s, std::string_view trackId)
{
    const TrackLoc l = FindTrack(s, trackId);
    return l.Valid() ? &s.bindings[static_cast<std::size_t>(l.binding)].tracks[static_cast<std::size_t>(l.track)] : nullptr;
}
const Track* GetTrack(const Sequence& s, std::string_view trackId)
{
    const TrackLoc l = FindTrack(s, trackId);
    return l.Valid() ? &s.bindings[static_cast<std::size_t>(l.binding)].tracks[static_cast<std::size_t>(l.track)] : nullptr;
}

int FindChannel(const Track& t, std::string_view name)
{
    for (std::size_t i = 0; i < t.channels.size(); ++i)
        if (t.channels[i].name == name) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------------------
// ID
// ---------------------------------------------------------------------------
void CollectIds(const Sequence& s, std::unordered_set<std::string>& out)
{
    if (!s.id.empty()) out.insert(s.id);
    for (const auto& c : s.cuts) out.insert(c.id);
    for (const auto& m : s.markers) out.insert(m.id);
    for (const auto& b : s.bindings)
    {
        out.insert(b.id);
        for (const auto& t : b.tracks)
        {
            out.insert(t.id);
            for (const auto& c : t.clips) out.insert(c.id);
            for (const auto& e : t.events) out.insert(e.id);
        }
    }
}

bool IdExists(const Sequence& s, std::string_view id)
{
    if (s.id == id) return true;
    for (const auto& c : s.cuts) if (c.id == id) return true;
    for (const auto& m : s.markers) if (m.id == id) return true;
    for (const auto& b : s.bindings)
    {
        if (b.id == id) return true;
        for (const auto& t : b.tracks)
        {
            if (t.id == id) return true;
            for (const auto& c : t.clips) if (c.id == id) return true;
            for (const auto& e : t.events) if (e.id == id) return true;
        }
    }
    return false;
}

void ReserveAllIds(const Sequence& s, IdAllocator& alloc)
{
    std::unordered_set<std::string> ids;
    CollectIds(s, ids);
    for (const auto& id : ids) alloc.Reserve(id);
}

// ---------------------------------------------------------------------------
// 範囲
// ---------------------------------------------------------------------------
Tick SequenceExtent(const Sequence& s)
{
    Tick e = 0;
    for (const auto& c : s.cuts) e = std::max(e, c.end);
    for (const auto& m : s.markers) e = std::max(e, m.t);
    for (const auto& b : s.bindings)
        for (const auto& t : b.tracks)
        {
            for (const auto& ch : t.channels)
                if (!ch.keys.empty()) e = std::max(e, ch.keys.back().t);
            for (const auto& c : t.clips) e = std::max(e, c.start + c.dur);
            for (const auto& ev : t.events) e = std::max(e, ev.t);
        }
    return e;
}

void GetRange(const Sequence& s, Tick& start, Tick& end)
{
    if (s.hasRange) { start = s.rangeStart; end = s.rangeEnd; return; }
    start = 0;
    end = SequenceExtent(s);
}

// ---------------------------------------------------------------------------
// 予約語(ファイルの型付きフィールド名)
// ---------------------------------------------------------------------------
bool IsReservedTrackParam(std::string_view k)
{
    return k == "id" || k == "type" || k == "name" || k == "mute" || k == "lock" || k == "path"
        || k == "valueType" || k == "colorSpace" || k == "rotation" || k == "channels" || k == "clips" || k == "events";
}
bool IsReservedClipParam(std::string_view k) { return k == "id" || k == "start" || k == "dur"; }
bool IsReservedEventParam(std::string_view k) { return k == "id" || k == "t" || k == "kind" || k == "name"; }
bool IsReservedBindingParam(std::string_view k)
{
    return k == "id" || k == "name" || k == "kind" || k == "hint" || k == "tracks";
}
bool IsReservedMarkerParam(std::string_view k) { return k == "id" || k == "t" || k == "name"; }

// ---------------------------------------------------------------------------
// トラックの構造検査 + 正準化
// ---------------------------------------------------------------------------
namespace
{
std::string CheckParams(SeqValue& p, bool (*reserved)(std::string_view), const char* what)
{
    if (p.IsNull()) p = SeqValue::MakeObject();
    if (!p.IsObject()) return std::string(what) + " の params はオブジェクトでなければならない";
    for (const auto& m : p.obj)
        if (reserved(m.key)) return std::string(what) + " の params に予約語 \"" + m.key + "\" は使えない";
    if (!p.AllFinite()) return std::string(what) + " の params に有限でない数値がある";
    return {};
}

} // namespace

// クォータニオンの 4 チャンネルが同じキー時刻を持つか(問題なければ空)
std::string QuatAlignmentProblem(const Track& tr)
{
    static const char* names[4] = { "rotation.x", "rotation.y", "rotation.z", "rotation.w" };
    const Channel* ch[4] = {};
    int have = 0;
    for (int i = 0; i < 4; ++i)
    {
        const int idx = FindChannel(tr, names[i]);
        if (idx >= 0) { ch[i] = &tr.channels[static_cast<std::size_t>(idx)]; ++have; }
    }
    if (have == 0) return {};
    if (have != 4) return "クォータニオン回転は rotation.x/y/z/w の 4 チャンネルが揃っていなければならない";
    for (int i = 1; i < 4; ++i)
    {
        if (ch[i]->keys.size() != ch[0]->keys.size()) return "クォータニオンの 4 チャンネルでキー数が違う";
        for (std::size_t k = 0; k < ch[0]->keys.size(); ++k)
            if (ch[i]->keys[k].t != ch[0]->keys[k].t) return "クォータニオンの 4 チャンネルでキー時刻が揃っていない";
    }
    return {};
}

std::string CanonicalizeTrack(Track& tr)
{
    if (!IsValidId(tr.id)) return "トラック ID が不正: \"" + tr.id + "\"";
    if (static_cast<int>(tr.type) >= static_cast<int>(TrackType::Count)) return "トラック種別が不正";
    if (std::string e = CheckParams(tr.params, IsReservedTrackParam, "トラック"); !e.empty()) return e;
    if (tr.type != TrackType::Transform) tr.rotation = RotationMode::Euler;   // 正準化: rotation は Transform だけがファイルに書かれる

    const TrackFamily fam = FamilyOf(tr.type);
    if (fam != TrackFamily::Curve && !tr.channels.empty()) return "カーブ族以外のトラックはチャンネルを持てない";
    if (fam != TrackFamily::Clip && !tr.clips.empty()) return "クリップ族以外のトラックはクリップを持てない";
    if (fam != TrackFamily::Event && !tr.events.empty()) return "イベント族以外のトラックはイベントを持てない";

    for (std::size_t i = 0; i < tr.channels.size(); ++i)
    {
        Channel& ch = tr.channels[i];
        if (ch.name.empty()) return "チャンネル名が空";
        for (std::size_t j = 0; j < i; ++j)
            if (tr.channels[j].name == ch.name) return "チャンネル名が重複: " + ch.name;
        for (std::size_t k = 0; k < ch.keys.size(); ++k)
        {
            NormalizeKey(ch.keys[k]);
            if (std::string e = CheckKey(ch.keys[k]); !e.empty()) return ch.name + "[" + std::to_string(k) + "]: " + e;
            if (k > 0 && ch.keys[k].t < ch.keys[k - 1].t) return ch.name + ": キーが時刻昇順でない";
            if (tr.type == TrackType::Property && (tr.valueType == ValueType::Bool || tr.valueType == ValueType::Int)
                && ch.keys[k].ip != Interp::Step)
                return ch.name + ": bool / int のプロパティのキーは Step 補間でなければならない";
        }
    }

    for (auto& c : tr.clips)
    {
        if (!IsValidId(c.id)) return "クリップ ID が不正: \"" + c.id + "\"";
        if (c.dur < 0) return "クリップの dur は 0 以上";
        if (c.start < -kMaxTick || c.start > kMaxTick || c.dur > kMaxTick) return "クリップの時刻が範囲外";
        if (std::string e = CheckParams(c.params, IsReservedClipParam, "クリップ"); !e.empty()) return e;
    }
    for (auto& ev : tr.events)
    {
        if (!IsValidId(ev.id)) return "イベント ID が不正: \"" + ev.id + "\"";
        if (ev.t < -kMaxTick || ev.t > kMaxTick) return "イベントの時刻が範囲外";
        if (ev.kind.empty()) return "イベントの kind が空";
        if (std::string e = CheckParams(ev.params, IsReservedEventParam, "イベント"); !e.empty()) return e;
    }
    return {};
}

// ---------------------------------------------------------------------------
// シーケンス全体の検査
// ---------------------------------------------------------------------------
std::vector<SeqIssue> ValidateSequence(const Sequence& s)
{
    std::vector<SeqIssue> out;
    auto err = [&](std::string path, std::string msg) {
        out.push_back({ SeqIssue::Severity::Error, std::move(path), std::move(msg) });
    };
    auto warn = [&](std::string path, std::string msg) {
        out.push_back({ SeqIssue::Severity::Warning, std::move(path), std::move(msg) });
    };

    if (s.ticksPerSecond <= 0) err("ticksPerSecond", "ticksPerSecond は正でなければならない");
    if (s.frameRate <= 0 || s.frameRate > 1000) err("frameRate", "frameRate は 1..1000 の整数");
    else if (s.ticksPerSecond > 0 && !FrameRateIsExact(s.frameRate, s.ticksPerSecond))
        warn("frameRate", "frameRate がティックに整数で乗らない(フレーム境界に丸め誤差が出る)");
    if (!s.id.empty() && !IsValidId(s.id)) err("id", "シーケンス ID が不正");
    if (s.hasRange && s.rangeEnd < s.rangeStart) err("range", "range の終端が始端より前");
    if (s.hasRange && (s.rangeStart < -kMaxTick || s.rangeEnd > kMaxTick)) err("range", "range が範囲外");

    // ID の一意・形式
    std::unordered_map<std::string, int> seen;
    auto reg = [&](const std::string& id, const std::string& path) {
        if (!IsValidId(id)) { err(path, "ID が不正: \"" + id + "\""); return; }
        if (++seen[id] == 2) err(path, "ID が重複: " + id);
    };
    if (!s.id.empty()) seen[s.id] = 1;

    for (std::size_t i = 0; i < s.cuts.size(); ++i)
    {
        const Cut& c = s.cuts[i];
        const std::string p = "cuts[" + std::to_string(i) + "]";
        reg(c.id, p);
        if (c.end < c.start) err(p, "カットの end が start より前");
        if (c.start < -kMaxTick || c.end > kMaxTick) err(p, "カットの時刻が範囲外");
        if (c.blend < 0) err(p, "カットの blend は 0 以上");
        if (FindBinding(s, c.camera) < 0) err(p, "カットのカメラが存在しないバインディングを指す: " + c.camera);
    }
    for (std::size_t i = 0; i < s.cuts.size(); ++i)
        for (std::size_t j = i + 1; j < s.cuts.size(); ++j)
            if (s.cuts[i].start < s.cuts[j].end && s.cuts[j].start < s.cuts[i].end)
                warn("cuts[" + std::to_string(j) + "]", "カットが cuts[" + std::to_string(i) + "] と重なっている(後ろの配列要素が優先)");
    for (std::size_t i = 0; i < s.markers.size(); ++i)
    {
        reg(s.markers[i].id, "markers[" + std::to_string(i) + "]");
        if (s.markers[i].t < -kMaxTick || s.markers[i].t > kMaxTick) err("markers[" + std::to_string(i) + "]", "マーカーの時刻が範囲外");
    }
    for (std::size_t b = 0; b < s.bindings.size(); ++b)
    {
        const Binding& bd = s.bindings[b];
        const std::string bp = "bindings[" + std::to_string(b) + "]";
        reg(bd.id, bp);
        if (static_cast<int>(bd.kind) > static_cast<int>(BindingKind::Spawnable)) err(bp, "バインディング種別が不正");
        for (const auto& m : bd.params.obj)
            if (IsReservedBindingParam(m.key)) err(bp, "バインディングの params に予約語: " + m.key);
        for (std::size_t t = 0; t < bd.tracks.size(); ++t)
        {
            const std::string tp = bp + ".tracks[" + std::to_string(t) + "]";
            Track copy = bd.tracks[t];
            if (std::string e = CanonicalizeTrack(copy); !e.empty()) { err(tp, e); continue; }
            if (bd.tracks[t].type == TrackType::Transform && bd.tracks[t].rotation == RotationMode::Quat)
                if (std::string q = QuatAlignmentProblem(bd.tracks[t]); !q.empty()) warn(tp, q);
            reg(bd.tracks[t].id, tp);
            for (const auto& c : bd.tracks[t].clips) reg(c.id, tp + ".clips");
            for (const auto& e : bd.tracks[t].events) reg(e.id, tp + ".events");
        }
    }
    return out;
}

std::string CheckSequenceStructure(const Sequence& s)
{
    for (const auto& i : ValidateSequence(s))
        if (i.severity == SeqIssue::Severity::Error) return i.path + ": " + i.message;
    return {};
}

} // namespace dx12e::seq
