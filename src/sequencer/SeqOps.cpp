#include "sequencer/SeqOps.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "sequencer/SeqCurve.h"

namespace dx12e::seq
{

namespace
{

ApplyResult Fail(std::string msg)
{
    ApplyResult r;
    r.ok = false;
    r.error = std::move(msg);
    return r;
}

// 新しく入れるオブジェクトが持つ ID を全部集めて、形式・重複(自身内と既存)を検査する。
std::string CheckNewIds(const Sequence& s, const std::vector<std::string>& ids)
{
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        if (!IsValidId(ids[i])) return "ID が不正: \"" + ids[i] + "\"";
        for (std::size_t j = 0; j < i; ++j)
            if (ids[j] == ids[i]) return "ID が重複: " + ids[i];
        if (IdExists(s, ids[i])) return "ID が既に存在する: " + ids[i];
    }
    return {};
}

void TrackIds(const Track& t, std::vector<std::string>& out)
{
    out.push_back(t.id);
    for (const auto& c : t.clips) out.push_back(c.id);
    for (const auto& e : t.events) out.push_back(e.id);
}

std::string CheckKeyForTrack(const Track& tr, const Key& k)
{
    if (std::string e = CheckKey(k); !e.empty()) return e;
    if (tr.type == TrackType::Property && (tr.valueType == ValueType::Bool || tr.valueType == ValueType::Int)
        && k.ip != Interp::Step)
        return "bool / int のプロパティのキーは Step 補間でなければならない";
    return {};
}

bool InRangeInsert(int index, std::size_t size) { return index >= 0 && static_cast<std::size_t>(index) <= size; }
bool InRangeIndex(int index, std::size_t size) { return index >= 0 && static_cast<std::size_t>(index) < size; }

// 順序を保ったまま tKey を入れられる位置か
bool SortedPosOk(const std::vector<Key>& keys, int index, Tick t)
{
    const std::size_t i = static_cast<std::size_t>(index);
    if (i > 0 && keys[i - 1].t > t) return false;
    if (i < keys.size() && keys[i].t < t) return false;
    return true;
}
int SortedInsertPos(const std::vector<Key>& keys, Tick t)
{
    const auto it = std::upper_bound(keys.begin(), keys.end(), t, [](Tick v, const Key& k) { return v < k.t; });
    return static_cast<int>(it - keys.begin());
}

struct Applier
{
    Sequence& s;
    SeqOp* inv;   // null = 逆 op 不要

    ApplyResult Done(SeqOp&& i) { if (inv) *inv = std::move(i); return {}; }

    // ---- 参照の解決 ------------------------------------------------------
    struct KeyRef { Track* tr = nullptr; Channel* ch = nullptr; std::string err; };
    KeyRef ResolveChannel(const std::string& trackId, const std::string& channel)
    {
        KeyRef r;
        r.tr = GetTrack(s, trackId);
        if (!r.tr) { r.err = "トラックが見つからない: " + trackId; return r; }
        const int ci = FindChannel(*r.tr, channel);
        if (ci < 0) { r.err = "チャンネルが見つからない: " + channel; r.tr = nullptr; return r; }
        r.ch = &r.tr->channels[static_cast<std::size_t>(ci)];
        return r;
    }

    // ---- シーケンス自体 --------------------------------------------------
    ApplyResult operator()(const OpSetName& op)
    {
        OpSetName old{ s.name };
        s.name = op.name;
        return Done(std::move(old));
    }
    ApplyResult operator()(const OpSetFrameRate& op)
    {
        if (op.fps < 1 || op.fps > 1000) return Fail("フレームレートは 1..1000");
        OpSetFrameRate old{ s.frameRate };
        s.frameRate = op.fps;
        return Done(std::move(old));
    }
    ApplyResult operator()(const OpSetRange& op)
    {
        if (op.has && (op.end < op.start || op.start < -kMaxTick || op.end > kMaxTick)) return Fail("range が不正");
        OpSetRange old{ s.hasRange, s.rangeStart, s.rangeEnd };
        s.hasRange = op.has;
        s.rangeStart = op.has ? op.start : 0;
        s.rangeEnd = op.has ? op.end : 0;
        return Done(std::move(old));
    }
    ApplyResult operator()(const OpSetRender& op)
    {
        if (op.render)
        {
            const RenderSettings& r = *op.render;
            if (r.width < 1 || r.height < 1 || r.width > 16384 || r.height > 16384) return Fail("解像度が不正");
            if (!std::isfinite(r.shutter) || r.shutter < 0.0 || r.shutter > 1.0) return Fail("shutter は 0..1");
            if (r.warmup < 0 || r.warmup > 100000) return Fail("warmup が不正");
        }
        OpSetRender old{ s.render };
        s.render = op.render;
        if (s.render && s.render->shutter == 0.0) s.render->shutter = 0.0;
        return Done(std::move(old));
    }
    ApplyResult operator()(const OpSetMeta& op)
    {
        SeqValue m = op.meta;
        if (m.IsNull()) m = SeqValue::MakeObject();
        if (!m.IsObject()) return Fail("meta はオブジェクトでなければならない");
        if (!m.AllFinite()) return Fail("meta に有限でない数値がある");
        OpSetMeta old{ s.meta };
        s.meta = std::move(m);
        return Done(std::move(old));
    }

    // ---- バインディング --------------------------------------------------
    static std::string CheckBindingHeader(const BindingHeader& h)
    {
        if (static_cast<int>(h.kind) > static_cast<int>(BindingKind::Spawnable)) return "バインディング種別が不正";
        if (!h.params.IsNull() && !h.params.IsObject()) return "バインディングの params はオブジェクト";
        for (const auto& m : h.params.obj)
            if (IsReservedBindingParam(m.key)) return "バインディングの params に予約語: " + m.key;
        if (!h.params.AllFinite()) return "バインディングの params に有限でない数値がある";
        return {};
    }
    ApplyResult operator()(const OpAddBinding& op)
    {
        Binding b = op.binding;
        if (std::string e = CheckBindingHeader(b); !e.empty()) return Fail(e);
        if (b.params.IsNull()) b.params = SeqValue::MakeObject();
        std::vector<std::string> ids{ b.id };
        for (auto& t : b.tracks)
        {
            if (std::string e = CanonicalizeTrack(t); !e.empty()) return Fail("トラック " + t.id + ": " + e);
            TrackIds(t, ids);
        }
        if (std::string e = CheckNewIds(s, ids); !e.empty()) return Fail(e);
        int index = op.index;
        if (index == -1) index = static_cast<int>(s.bindings.size());
        if (!InRangeInsert(index, s.bindings.size())) return Fail("バインディングの挿入位置が範囲外");
        s.bindings.insert(s.bindings.begin() + index, std::move(b));
        return Done(OpDeleteBinding{ op.binding.id });
    }
    ApplyResult operator()(const OpDeleteBinding& op)
    {
        const int i = FindBinding(s, op.id);
        if (i < 0) return Fail("バインディングが見つからない: " + op.id);
        for (const auto& c : s.cuts)
            if (c.camera == op.id) return Fail("カット " + c.id + " がこのバインディングを参照している(先にカットを消すこと)");
        OpAddBinding inverse{ std::move(s.bindings[static_cast<std::size_t>(i)]), i };
        s.bindings.erase(s.bindings.begin() + i);
        return Done(std::move(inverse));
    }
    ApplyResult operator()(const OpMoveBinding& op)
    {
        const int i = FindBinding(s, op.id);
        if (i < 0) return Fail("バインディングが見つからない: " + op.id);
        if (!InRangeIndex(op.toIndex, s.bindings.size())) return Fail("移動先の添字が範囲外");
        Binding b = std::move(s.bindings[static_cast<std::size_t>(i)]);
        s.bindings.erase(s.bindings.begin() + i);
        s.bindings.insert(s.bindings.begin() + op.toIndex, std::move(b));
        return Done(OpMoveBinding{ op.id, i });
    }
    ApplyResult operator()(const OpSetBindingHeader& op)
    {
        const int i = FindBinding(s, op.id);
        if (i < 0) return Fail("バインディングが見つからない: " + op.id);
        BindingHeader h = op.header;
        if (std::string e = CheckBindingHeader(h); !e.empty()) return Fail(e);
        if (h.params.IsNull()) h.params = SeqValue::MakeObject();
        Binding& b = s.bindings[static_cast<std::size_t>(i)];
        OpSetBindingHeader old{ op.id, static_cast<const BindingHeader&>(b) };
        static_cast<BindingHeader&>(b) = std::move(h);
        return Done(std::move(old));
    }

    // ---- トラック --------------------------------------------------------
    ApplyResult operator()(const OpAddTrack& op)
    {
        const int bi = FindBinding(s, op.bindingId);
        if (bi < 0) return Fail("バインディングが見つからない: " + op.bindingId);
        Track t = op.track;
        if (std::string e = CanonicalizeTrack(t); !e.empty()) return Fail(e);
        std::vector<std::string> ids;
        TrackIds(t, ids);
        if (std::string e = CheckNewIds(s, ids); !e.empty()) return Fail(e);
        auto& tracks = s.bindings[static_cast<std::size_t>(bi)].tracks;
        int index = op.index;
        if (index == -1) index = static_cast<int>(tracks.size());
        if (!InRangeInsert(index, tracks.size())) return Fail("トラックの挿入位置が範囲外");
        tracks.insert(tracks.begin() + index, std::move(t));
        return Done(OpDeleteTrack{ op.track.id });
    }
    ApplyResult operator()(const OpDeleteTrack& op)
    {
        const TrackLoc l = FindTrack(s, op.trackId);
        if (!l.Valid()) return Fail("トラックが見つからない: " + op.trackId);
        Binding& b = s.bindings[static_cast<std::size_t>(l.binding)];
        OpAddTrack inverse{ b.id, std::move(b.tracks[static_cast<std::size_t>(l.track)]), l.track };
        b.tracks.erase(b.tracks.begin() + l.track);
        return Done(std::move(inverse));
    }
    ApplyResult operator()(const OpMoveTrack& op)
    {
        const TrackLoc l = FindTrack(s, op.trackId);
        if (!l.Valid()) return Fail("トラックが見つからない: " + op.trackId);
        const int dst = FindBinding(s, op.toBindingId);
        if (dst < 0) return Fail("移動先のバインディングが見つからない: " + op.toBindingId);
        Binding& sb = s.bindings[static_cast<std::size_t>(l.binding)];
        Binding& db = s.bindings[static_cast<std::size_t>(dst)];
        const std::size_t afterSize = (dst == l.binding) ? sb.tracks.size() - 1 : db.tracks.size();
        if (!InRangeInsert(op.toIndex, afterSize)) return Fail("移動先の添字が範囲外");
        const std::string fromBinding = sb.id;
        Track t = std::move(sb.tracks[static_cast<std::size_t>(l.track)]);
        sb.tracks.erase(sb.tracks.begin() + l.track);
        db.tracks.insert(db.tracks.begin() + op.toIndex, std::move(t));
        return Done(OpMoveTrack{ op.trackId, fromBinding, l.track });
    }
    ApplyResult operator()(const OpSetTrackHeader& op)
    {
        Track* tr = GetTrack(s, op.trackId);
        if (!tr) return Fail("トラックが見つからない: " + op.trackId);
        Track candidate = *tr;
        static_cast<TrackHeader&>(candidate) = op.header;
        if (std::string e = CanonicalizeTrack(candidate); !e.empty()) return Fail(e);
        OpSetTrackHeader old{ op.trackId, static_cast<const TrackHeader&>(*tr) };
        static_cast<TrackHeader&>(*tr) = static_cast<const TrackHeader&>(candidate);
        return Done(std::move(old));
    }

    // ---- チャンネル ------------------------------------------------------
    ApplyResult operator()(const OpAddChannel& op)
    {
        Track* tr = GetTrack(s, op.trackId);
        if (!tr) return Fail("トラックが見つからない: " + op.trackId);
        if (FamilyOf(tr->type) != TrackFamily::Curve) return Fail("カーブ族のトラックにしかチャンネルを足せない");
        if (op.channel.name.empty()) return Fail("チャンネル名が空");
        if (FindChannel(*tr, op.channel.name) >= 0) return Fail("チャンネル名が重複: " + op.channel.name);
        Channel ch = op.channel;
        for (std::size_t k = 0; k < ch.keys.size(); ++k)
        {
            NormalizeKey(ch.keys[k]);
            if (std::string e = CheckKeyForTrack(*tr, ch.keys[k]); !e.empty()) return Fail(ch.name + ": " + e);
            if (k > 0 && ch.keys[k].t < ch.keys[k - 1].t) return Fail(ch.name + ": キーが時刻昇順でない");
        }
        int index = op.index;
        if (index == -1) index = static_cast<int>(tr->channels.size());
        if (!InRangeInsert(index, tr->channels.size())) return Fail("チャンネルの挿入位置が範囲外");
        tr->channels.insert(tr->channels.begin() + index, std::move(ch));
        return Done(OpDeleteChannel{ op.trackId, op.channel.name });
    }
    ApplyResult operator()(const OpDeleteChannel& op)
    {
        Track* tr = GetTrack(s, op.trackId);
        if (!tr) return Fail("トラックが見つからない: " + op.trackId);
        const int ci = FindChannel(*tr, op.channel);
        if (ci < 0) return Fail("チャンネルが見つからない: " + op.channel);
        OpAddChannel inverse{ op.trackId, std::move(tr->channels[static_cast<std::size_t>(ci)]), ci };
        tr->channels.erase(tr->channels.begin() + ci);
        return Done(std::move(inverse));
    }
    ApplyResult operator()(const OpSetChannelExtrap& op)
    {
        KeyRef r = ResolveChannel(op.trackId, op.channel);
        if (!r.ch) return Fail(r.err);
        if (static_cast<int>(op.pre) > static_cast<int>(Extrap::PingPong) || static_cast<int>(op.post) > static_cast<int>(Extrap::PingPong))
            return Fail("範囲外挙動が不正");
        OpSetChannelExtrap old{ op.trackId, op.channel, r.ch->pre, r.ch->post };
        r.ch->pre = op.pre;
        r.ch->post = op.post;
        return Done(std::move(old));
    }

    // ---- キー ------------------------------------------------------------
    ApplyResult operator()(const OpAddKey& op)
    {
        KeyRef r = ResolveChannel(op.trackId, op.channel);
        if (!r.ch) return Fail(r.err);
        Key k = op.key;
        NormalizeKey(k);
        if (std::string e = CheckKeyForTrack(*r.tr, k); !e.empty()) return Fail(e);
        int index = op.index;
        if (index == -1) index = SortedInsertPos(r.ch->keys, k.t);
        if (!InRangeInsert(index, r.ch->keys.size())) return Fail("キーの挿入位置が範囲外");
        if (!SortedPosOk(r.ch->keys, index, k.t)) return Fail("キーの挿入位置が時刻順を壊す");
        r.ch->keys.insert(r.ch->keys.begin() + index, k);
        return Done(OpDeleteKey{ op.trackId, op.channel, index });
    }
    ApplyResult operator()(const OpDeleteKey& op)
    {
        KeyRef r = ResolveChannel(op.trackId, op.channel);
        if (!r.ch) return Fail(r.err);
        if (!InRangeIndex(op.index, r.ch->keys.size())) return Fail("キーの添字が範囲外");
        OpAddKey inverse{ op.trackId, op.channel, r.ch->keys[static_cast<std::size_t>(op.index)], op.index };
        r.ch->keys.erase(r.ch->keys.begin() + op.index);
        return Done(std::move(inverse));
    }
    ApplyResult operator()(const OpMoveKey& op)
    {
        KeyRef r = ResolveChannel(op.trackId, op.channel);
        if (!r.ch) return Fail(r.err);
        auto& keys = r.ch->keys;
        if (!InRangeIndex(op.index, keys.size())) return Fail("キーの添字が範囲外");
        if (op.newT < -kMaxTick || op.newT > kMaxTick) return Fail("キーの時刻が範囲外");
        Key k = keys[static_cast<std::size_t>(op.index)];
        const Tick oldT = k.t;
        keys.erase(keys.begin() + op.index);
        int newIndex = op.newIndex;
        if (newIndex == -1) newIndex = SortedInsertPos(keys, op.newT);
        if (!InRangeInsert(newIndex, keys.size()) || !SortedPosOk(keys, newIndex, op.newT))
        {
            keys.insert(keys.begin() + op.index, k);   // 元へ戻す
            return Fail("キーの移動先が範囲外か時刻順を壊す");
        }
        k.t = op.newT;
        keys.insert(keys.begin() + newIndex, k);
        return Done(OpMoveKey{ op.trackId, op.channel, newIndex, oldT, op.index });
    }
    ApplyResult operator()(const OpSetKeyValue& op)
    {
        KeyRef r = ResolveChannel(op.trackId, op.channel);
        if (!r.ch) return Fail(r.err);
        if (!InRangeIndex(op.index, r.ch->keys.size())) return Fail("キーの添字が範囲外");
        if (!std::isfinite(op.v)) return Fail("値が有限でない");
        Key& k = r.ch->keys[static_cast<std::size_t>(op.index)];
        OpSetKeyValue old{ op.trackId, op.channel, op.index, k.v };
        k.v = (op.v == 0.0) ? 0.0 : op.v;
        return Done(std::move(old));
    }
    ApplyResult operator()(const OpSetInterp& op)
    {
        KeyRef r = ResolveChannel(op.trackId, op.channel);
        if (!r.ch) return Fail(r.err);
        auto& keys = r.ch->keys;
        if (!InRangeIndex(op.index, keys.size())) return Fail("キーの添字が範囲外");
        Key nk = keys[static_cast<std::size_t>(op.index)];
        const Key oldKey = nk;
        nk.ip = op.ip;
        nk.ease = op.ease;
        if (nk.ip == Interp::Bezier && oldKey.ip != Interp::Bezier)
            DefaultBezierHandles(keys, static_cast<std::size_t>(op.index), nk.inDt, nk.inDv, nk.outDt, nk.outDv);
        NormalizeKey(nk);
        if (std::string e = CheckKeyForTrack(*r.tr, nk); !e.empty()) return Fail(e);
        keys[static_cast<std::size_t>(op.index)] = nk;
        return Done(OpSetKey{ op.trackId, op.channel, op.index, oldKey });
    }
    ApplyResult operator()(const OpSetTangent& op)
    {
        KeyRef r = ResolveChannel(op.trackId, op.channel);
        if (!r.ch) return Fail(r.err);
        if (!InRangeIndex(op.index, r.ch->keys.size())) return Fail("キーの添字が範囲外");
        Key& k = r.ch->keys[static_cast<std::size_t>(op.index)];
        if (k.ip != Interp::Bezier) return Fail("ハンドルを持てるのは Bezier のキーだけ");
        Key nk = k;
        nk.inDt = op.inDt; nk.inDv = op.inDv; nk.outDt = op.outDt; nk.outDv = op.outDv;
        NormalizeKey(nk);
        if (std::string e = CheckKey(nk); !e.empty()) return Fail(e);
        OpSetTangent old{ op.trackId, op.channel, op.index, k.inDt, k.inDv, k.outDt, k.outDv };
        k = nk;
        return Done(std::move(old));
    }
    ApplyResult operator()(const OpSetKey& op)
    {
        KeyRef r = ResolveChannel(op.trackId, op.channel);
        if (!r.ch) return Fail(r.err);
        if (!InRangeIndex(op.index, r.ch->keys.size())) return Fail("キーの添字が範囲外");
        Key& k = r.ch->keys[static_cast<std::size_t>(op.index)];
        Key nk = op.key;
        NormalizeKey(nk);
        if (nk.t != k.t) return Fail("OpSetKey は時刻を変えられない(OpMoveKey を使うこと)");
        if (std::string e = CheckKeyForTrack(*r.tr, nk); !e.empty()) return Fail(e);
        OpSetKey old{ op.trackId, op.channel, op.index, k };
        k = nk;
        return Done(std::move(old));
    }

    // ---- クリップ / イベント ---------------------------------------------
    ApplyResult operator()(const OpAddClip& op)
    {
        Track* tr = GetTrack(s, op.trackId);
        if (!tr) return Fail("トラックが見つからない: " + op.trackId);
        if (FamilyOf(tr->type) != TrackFamily::Clip) return Fail("クリップ族のトラックにしかクリップを足せない");
        Clip c = op.clip;
        if (std::string e = CheckClip(c); !e.empty()) return Fail(e);
        if (std::string e = CheckNewIds(s, { c.id }); !e.empty()) return Fail(e);
        int index = op.index;
        if (index == -1) index = static_cast<int>(tr->clips.size());
        if (!InRangeInsert(index, tr->clips.size())) return Fail("クリップの挿入位置が範囲外");
        tr->clips.insert(tr->clips.begin() + index, std::move(c));
        return Done(OpDeleteClip{ op.trackId, op.clip.id });
    }
    ApplyResult operator()(const OpDeleteClip& op)
    {
        Track* tr = GetTrack(s, op.trackId);
        if (!tr) return Fail("トラックが見つからない: " + op.trackId);
        for (std::size_t i = 0; i < tr->clips.size(); ++i)
            if (tr->clips[i].id == op.clipId)
            {
                OpAddClip inverse{ op.trackId, std::move(tr->clips[i]), static_cast<int>(i) };
                tr->clips.erase(tr->clips.begin() + static_cast<std::ptrdiff_t>(i));
                return Done(std::move(inverse));
            }
        return Fail("クリップが見つからない: " + op.clipId);
    }
    ApplyResult operator()(const OpSetClip& op)
    {
        Track* tr = GetTrack(s, op.trackId);
        if (!tr) return Fail("トラックが見つからない: " + op.trackId);
        Clip c = op.clip;
        if (std::string e = CheckClip(c); !e.empty()) return Fail(e);
        for (auto& cur : tr->clips)
            if (cur.id == c.id)
            {
                OpSetClip old{ op.trackId, cur };
                cur = std::move(c);
                return Done(std::move(old));
            }
        return Fail("クリップが見つからない: " + op.clip.id);
    }
    static std::string CheckClip(Clip& c)
    {
        if (!IsValidId(c.id)) return "クリップ ID が不正: \"" + c.id + "\"";
        if (c.dur < 0) return "クリップの dur は 0 以上";
        if (c.start < -kMaxTick || c.start > kMaxTick || c.dur > kMaxTick) return "クリップの時刻が範囲外";
        if (c.params.IsNull()) c.params = SeqValue::MakeObject();
        if (!c.params.IsObject()) return "クリップの params はオブジェクト";
        for (const auto& m : c.params.obj)
            if (IsReservedClipParam(m.key)) return "クリップの params に予約語: " + m.key;
        if (!c.params.AllFinite()) return "クリップの params に有限でない数値がある";
        return {};
    }
    static std::string CheckEvent(EventItem& e)
    {
        if (!IsValidId(e.id)) return "イベント ID が不正: \"" + e.id + "\"";
        if (e.t < -kMaxTick || e.t > kMaxTick) return "イベントの時刻が範囲外";
        if (e.kind.empty()) return "イベントの kind が空";
        if (e.params.IsNull()) e.params = SeqValue::MakeObject();
        if (!e.params.IsObject()) return "イベントの params はオブジェクト";
        for (const auto& m : e.params.obj)
            if (IsReservedEventParam(m.key)) return "イベントの params に予約語: " + m.key;
        if (!e.params.AllFinite()) return "イベントの params に有限でない数値がある";
        return {};
    }
    ApplyResult operator()(const OpAddEvent& op)
    {
        Track* tr = GetTrack(s, op.trackId);
        if (!tr) return Fail("トラックが見つからない: " + op.trackId);
        if (FamilyOf(tr->type) != TrackFamily::Event) return Fail("イベント族のトラックにしかイベントを足せない");
        EventItem e = op.event;
        if (std::string m = CheckEvent(e); !m.empty()) return Fail(m);
        if (std::string m = CheckNewIds(s, { e.id }); !m.empty()) return Fail(m);
        int index = op.index;
        if (index == -1) index = static_cast<int>(tr->events.size());
        if (!InRangeInsert(index, tr->events.size())) return Fail("イベントの挿入位置が範囲外");
        tr->events.insert(tr->events.begin() + index, std::move(e));
        return Done(OpDeleteEvent{ op.trackId, op.event.id });
    }
    ApplyResult operator()(const OpDeleteEvent& op)
    {
        Track* tr = GetTrack(s, op.trackId);
        if (!tr) return Fail("トラックが見つからない: " + op.trackId);
        for (std::size_t i = 0; i < tr->events.size(); ++i)
            if (tr->events[i].id == op.eventId)
            {
                OpAddEvent inverse{ op.trackId, std::move(tr->events[i]), static_cast<int>(i) };
                tr->events.erase(tr->events.begin() + static_cast<std::ptrdiff_t>(i));
                return Done(std::move(inverse));
            }
        return Fail("イベントが見つからない: " + op.eventId);
    }
    ApplyResult operator()(const OpSetEvent& op)
    {
        Track* tr = GetTrack(s, op.trackId);
        if (!tr) return Fail("トラックが見つからない: " + op.trackId);
        EventItem e = op.event;
        if (std::string m = CheckEvent(e); !m.empty()) return Fail(m);
        for (auto& cur : tr->events)
            if (cur.id == e.id)
            {
                OpSetEvent old{ op.trackId, cur };
                cur = std::move(e);
                return Done(std::move(old));
            }
        return Fail("イベントが見つからない: " + op.event.id);
    }

    // ---- マーカー / カット -----------------------------------------------
    static std::string CheckMarker(Marker& m)
    {
        if (!IsValidId(m.id)) return "マーカー ID が不正: \"" + m.id + "\"";
        if (m.t < -kMaxTick || m.t > kMaxTick) return "マーカーの時刻が範囲外";
        if (m.params.IsNull()) m.params = SeqValue::MakeObject();
        if (!m.params.IsObject()) return "マーカーの params はオブジェクト";
        for (const auto& mm : m.params.obj)
            if (IsReservedMarkerParam(mm.key)) return "マーカーの params に予約語: " + mm.key;
        if (!m.params.AllFinite()) return "マーカーの params に有限でない数値がある";
        return {};
    }
    ApplyResult operator()(const OpAddMarker& op)
    {
        Marker m = op.marker;
        if (std::string e = CheckMarker(m); !e.empty()) return Fail(e);
        if (std::string e = CheckNewIds(s, { m.id }); !e.empty()) return Fail(e);
        int index = op.index;
        if (index == -1) index = static_cast<int>(s.markers.size());
        if (!InRangeInsert(index, s.markers.size())) return Fail("マーカーの挿入位置が範囲外");
        s.markers.insert(s.markers.begin() + index, std::move(m));
        return Done(OpDeleteMarker{ op.marker.id });
    }
    ApplyResult operator()(const OpDeleteMarker& op)
    {
        for (std::size_t i = 0; i < s.markers.size(); ++i)
            if (s.markers[i].id == op.id)
            {
                OpAddMarker inverse{ std::move(s.markers[i]), static_cast<int>(i) };
                s.markers.erase(s.markers.begin() + static_cast<std::ptrdiff_t>(i));
                return Done(std::move(inverse));
            }
        return Fail("マーカーが見つからない: " + op.id);
    }
    ApplyResult operator()(const OpSetMarker& op)
    {
        Marker m = op.marker;
        if (std::string e = CheckMarker(m); !e.empty()) return Fail(e);
        for (auto& cur : s.markers)
            if (cur.id == m.id)
            {
                OpSetMarker old{ cur };
                cur = std::move(m);
                return Done(std::move(old));
            }
        return Fail("マーカーが見つからない: " + op.marker.id);
    }
    std::string CheckCut(const Cut& c) const
    {
        if (!IsValidId(c.id)) return "カット ID が不正: \"" + c.id + "\"";
        if (c.end < c.start) return "カットの end が start より前";
        if (c.start < -kMaxTick || c.end > kMaxTick) return "カットの時刻が範囲外";
        if (c.blend < 0 || c.blend > kMaxTick) return "カットの blend が不正";
        if (FindBinding(s, c.camera) < 0) return "カットのカメラが存在しないバインディング: " + c.camera;
        return {};
    }
    ApplyResult operator()(const OpAddCut& op)
    {
        if (std::string e = CheckCut(op.cut); !e.empty()) return Fail(e);
        if (std::string e = CheckNewIds(s, { op.cut.id }); !e.empty()) return Fail(e);
        int index = op.index;
        if (index == -1) index = static_cast<int>(s.cuts.size());
        if (!InRangeInsert(index, s.cuts.size())) return Fail("カットの挿入位置が範囲外");
        s.cuts.insert(s.cuts.begin() + index, op.cut);
        return Done(OpDeleteCut{ op.cut.id });
    }
    ApplyResult operator()(const OpDeleteCut& op)
    {
        for (std::size_t i = 0; i < s.cuts.size(); ++i)
            if (s.cuts[i].id == op.id)
            {
                OpAddCut inverse{ s.cuts[i], static_cast<int>(i) };
                s.cuts.erase(s.cuts.begin() + static_cast<std::ptrdiff_t>(i));
                return Done(std::move(inverse));
            }
        return Fail("カットが見つからない: " + op.id);
    }
    ApplyResult operator()(const OpSetCut& op)
    {
        if (std::string e = CheckCut(op.cut); !e.empty()) return Fail(e);
        for (auto& cur : s.cuts)
            if (cur.id == op.cut.id)
            {
                OpSetCut old{ cur };
                cur = op.cut;
                return Done(std::move(old));
            }
        return Fail("カットが見つからない: " + op.cut.id);
    }
};

} // namespace

const char* OpName(const SeqOp& op)
{
    static const char* const names[] = {
        "SetName", "SetFrameRate", "SetRange", "SetRender", "SetMeta",
        "AddBinding", "DeleteBinding", "MoveBinding", "SetBindingHeader",
        "AddTrack", "DeleteTrack", "MoveTrack", "SetTrackHeader",
        "AddChannel", "DeleteChannel", "SetChannelExtrap",
        "AddKey", "DeleteKey", "MoveKey", "SetKeyValue", "SetInterp", "SetTangent", "SetKey",
        "AddClip", "DeleteClip", "SetClip", "AddEvent", "DeleteEvent", "SetEvent",
        "AddMarker", "DeleteMarker", "SetMarker", "AddCut", "DeleteCut", "SetCut",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == std::variant_size_v<SeqOp>, "OpName の表を op の数に揃えること");
    return names[op.index()];
}

ApplyResult ApplyOp(Sequence& seq, const SeqOp& op, SeqOp* inverse)
{
    Applier a{ seq, inverse };
    return std::visit(a, op);
}

ApplyResult ApplyTxn(Sequence& seq, const SeqTxn& txn, SeqTxn* inverse)
{
    std::vector<SeqOp> invs;
    invs.reserve(txn.ops.size());
    for (std::size_t i = 0; i < txn.ops.size(); ++i)
    {
        SeqOp inv;
        ApplyResult r = ApplyOp(seq, txn.ops[i], &inv);
        if (!r)
        {
            for (std::size_t j = invs.size(); j-- > 0;) ApplyOp(seq, invs[j]);   // 巻き戻し(必ず成功する)
            return Fail("op[" + std::to_string(i) + "] " + OpName(txn.ops[i]) + ": " + r.error);
        }
        invs.push_back(std::move(inv));
    }
    if (inverse)
    {
        inverse->label = txn.label;
        inverse->ops.assign(std::make_move_iterator(invs.rbegin()), std::make_move_iterator(invs.rend()));
    }
    return {};
}

ApplyResult SeqHistory::Execute(Sequence& seq, SeqTxn txn)
{
    SeqTxn inv;
    ApplyResult r = ApplyTxn(seq, txn, &inv);
    if (!r) return r;
    m_redo.clear();
    m_undo.push_back({ std::move(txn), std::move(inv) });
    if (m_max > 0 && m_undo.size() > m_max) m_undo.erase(m_undo.begin());
    return r;
}

void SeqHistory::Record(SeqTxn forward, SeqTxn inverse)
{
    m_redo.clear();
    m_undo.push_back({ std::move(forward), std::move(inverse) });
    if (m_max > 0 && m_undo.size() > m_max) m_undo.erase(m_undo.begin());
}

ApplyResult SeqHistory::Undo(Sequence& seq)
{
    if (m_undo.empty()) return Fail("Undo できる操作がない");
    Entry e = std::move(m_undo.back());
    m_undo.pop_back();
    SeqTxn redoInv;
    ApplyResult r = ApplyTxn(seq, e.inverse, &redoInv);
    if (!r)
    {
        Clear();   // 履歴の外で書き換えられた等。整合が取れないので履歴を捨てる
        return Fail("Undo に失敗(履歴を破棄): " + r.error);
    }
    e.forward.ops = std::move(redoInv.ops);   // 逆の逆 = 元の操作(ID 等が確定した形)
    m_redo.push_back(std::move(e));
    return {};
}

ApplyResult SeqHistory::Redo(Sequence& seq)
{
    if (m_redo.empty()) return Fail("Redo できる操作がない");
    Entry e = std::move(m_redo.back());
    m_redo.pop_back();
    SeqTxn inv;
    ApplyResult r = ApplyTxn(seq, e.forward, &inv);
    if (!r)
    {
        Clear();
        return Fail("Redo に失敗(履歴を破棄): " + r.error);
    }
    e.inverse = std::move(inv);
    m_undo.push_back(std::move(e));
    return {};
}

} // namespace dx12e::seq
