#include "sequencer/SeqEval.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dx12e::seq
{

// ===========================================================================
// Evaluate
// ===========================================================================
namespace
{
constexpr const char* kQuatNames[4] = { "rotation.x", "rotation.y", "rotation.z", "rotation.w" };

inline bool ColorSrgbChannel(const Track& tr, const Channel& ch)
{
    if (tr.colorSpace != ColorSpace::Srgb) return false;
    if (tr.type == TrackType::Property) { if (tr.valueType != ValueType::Color) return false; }
    else if (tr.type != TrackType::Light) return false;
    return !ChannelIsAlpha(ch.name);
}
} // namespace

void Evaluate(const Sequence& seq, Tick t, EvalResult& out, const EvalFilter* filter)
{
    out.Clear();
    out.t = t;
    for (std::size_t b = 0; b < seq.bindings.size(); ++b)
    {
        if (filter && b < filter->bindingEnabled.size() && filter->bindingEnabled[b] == 0) continue;
        const Binding& bd = seq.bindings[b];
        for (std::size_t ti = 0; ti < bd.tracks.size(); ++ti)
        {
            const Track& tr = bd.tracks[ti];
            if (tr.mute) continue;
            const std::int32_t bi = static_cast<std::int32_t>(b), tri = static_cast<std::int32_t>(ti);
            switch (FamilyOf(tr.type))
            {
            case TrackFamily::Curve:
            {
                // クォータニオン回転: 4 チャンネルが揃っていれば 1 個の QuatSample にまとめる
                int q[4] = { -1, -1, -1, -1 };
                bool quat = false;
                if (tr.type == TrackType::Transform && tr.rotation == RotationMode::Quat)
                {
                    quat = true;
                    for (int i = 0; i < 4; ++i)
                    {
                        q[i] = FindChannel(tr, kQuatNames[i]);
                        if (q[i] < 0 || tr.channels[static_cast<std::size_t>(q[i])].keys.empty()) quat = false;
                    }
                    // キー数が食い違う(=編集途中や不正データ)ときは、通常のスカラーとして評価する(範囲外アクセスを避ける)
                    if (quat)
                        for (int i = 1; i < 4; ++i)
                            if (tr.channels[static_cast<std::size_t>(q[i])].keys.size() != tr.channels[static_cast<std::size_t>(q[0])].keys.size())
                                quat = false;
                }
                for (std::size_t ci = 0; ci < tr.channels.size(); ++ci)
                {
                    const Channel& ch = tr.channels[ci];
                    if (ch.keys.empty()) continue;
                    if (quat && (static_cast<int>(ci) == q[0] || static_cast<int>(ci) == q[1]
                                 || static_cast<int>(ci) == q[2] || static_cast<int>(ci) == q[3]))
                        continue;
                    ChannelSample s;
                    s.binding = bi; s.track = tri; s.channel = static_cast<std::int32_t>(ci);
                    s.value = EvalChannel(ch, t, ColorSrgbChannel(tr, ch));
                    out.channels.push_back(s);
                }
                if (quat)
                {
                    QuatSample qs;
                    qs.binding = bi; qs.track = tri;
                    qs.q = EvalQuatChannels(tr.channels[static_cast<std::size_t>(q[0])], tr.channels[static_cast<std::size_t>(q[1])],
                                            tr.channels[static_cast<std::size_t>(q[2])], tr.channels[static_cast<std::size_t>(q[3])], t);
                    out.quats.push_back(qs);
                }
                break;
            }
            case TrackFamily::Clip:
                for (std::size_t ci = 0; ci < tr.clips.size(); ++ci)
                {
                    const Clip& c = tr.clips[ci];
                    const bool active = (c.dur > 0) ? (t >= c.start && t < c.start + c.dur) : (t == c.start);
                    if (!active) continue;
                    ClipSample s;
                    s.binding = bi; s.track = tri; s.clip = static_cast<std::int32_t>(ci);
                    s.localTick = t - c.start;
                    double w = 1.0;
                    const double bin = c.params.GetNumber("blendIn", 0.0);
                    const double bout = c.params.GetNumber("blendOut", 0.0);
                    if (bin > 0.0 && static_cast<double>(s.localTick) < bin) w *= static_cast<double>(s.localTick) / bin;
                    if (bout > 0.0 && c.dur > 0)
                    {
                        const double remain = static_cast<double>(c.dur - s.localTick);
                        if (remain < bout) w *= remain / bout;
                    }
                    s.weight = std::clamp(w, 0.0, 1.0);
                    out.clips.push_back(s);
                }
                break;
            default: break;   // Event / Constraint は Evaluate の対象外(イベントは CollectEvents)
            }
        }
    }
    out.cutIndex = SelectCut(seq, t);
    if (out.cutIndex >= 0) out.cutBinding = FindBinding(seq, seq.cuts[static_cast<std::size_t>(out.cutIndex)].camera);
}

EvalResult Evaluate(const Sequence& seq, Tick t, const EvalFilter* filter)
{
    EvalResult r;
    Evaluate(seq, t, r, filter);
    return r;
}

namespace
{
inline bool SameBits(double a, double b)
{
    std::uint64_t x, y;
    std::memcpy(&x, &a, sizeof(x));
    std::memcpy(&y, &b, sizeof(y));
    return x == y;
}
} // namespace

bool BitEqual(const EvalResult& a, const EvalResult& b)
{
    if (a.t != b.t || a.cutIndex != b.cutIndex || a.cutBinding != b.cutBinding) return false;
    if (a.channels.size() != b.channels.size() || a.quats.size() != b.quats.size() || a.clips.size() != b.clips.size()) return false;
    for (std::size_t i = 0; i < a.channels.size(); ++i)
    {
        const auto &x = a.channels[i], &y = b.channels[i];
        if (x.binding != y.binding || x.track != y.track || x.channel != y.channel || !SameBits(x.value, y.value)) return false;
    }
    for (std::size_t i = 0; i < a.quats.size(); ++i)
    {
        const auto &x = a.quats[i], &y = b.quats[i];
        if (x.binding != y.binding || x.track != y.track) return false;
        if (!SameBits(x.q.x, y.q.x) || !SameBits(x.q.y, y.q.y) || !SameBits(x.q.z, y.q.z) || !SameBits(x.q.w, y.q.w)) return false;
    }
    for (std::size_t i = 0; i < a.clips.size(); ++i)
    {
        const auto &x = a.clips[i], &y = b.clips[i];
        if (x.binding != y.binding || x.track != y.track || x.clip != y.clip || x.localTick != y.localTick
            || !SameBits(x.weight, y.weight)) return false;
    }
    return true;
}

// ===========================================================================
// カット / マーカー
// ===========================================================================
int SelectCut(const Sequence& seq, Tick t)
{
    int best = -1;
    for (std::size_t i = 0; i < seq.cuts.size(); ++i)
        if (t >= seq.cuts[i].start && t < seq.cuts[i].end) best = static_cast<int>(i);   // 後ろが勝つ
    if (best >= 0) return best;

    // 範囲末尾ちょうどのフレームは、そこを終端に持つ最後のカットに含める(最終フレームでカメラが戻らない)
    int endHit = -1;
    for (std::size_t i = 0; i < seq.cuts.size(); ++i)
        if (seq.cuts[i].end == t && seq.cuts[i].end >= seq.cuts[i].start) endHit = static_cast<int>(i);
    if (endHit < 0) return -1;
    Tick rs, re;
    GetRange(seq, rs, re);
    return (t == re) ? endHit : -1;
}

int NextMarker(const Sequence& seq, Tick t)
{
    int best = -1;
    for (std::size_t i = 0; i < seq.markers.size(); ++i)
        if (seq.markers[i].t > t && (best < 0 || seq.markers[i].t < seq.markers[static_cast<std::size_t>(best)].t))
            best = static_cast<int>(i);
    return best;
}

int PrevMarker(const Sequence& seq, Tick t)
{
    int best = -1;
    for (std::size_t i = 0; i < seq.markers.size(); ++i)
        if (seq.markers[i].t < t && (best < 0 || seq.markers[i].t > seq.markers[static_cast<std::size_t>(best)].t))
            best = static_cast<int>(i);
    return best;
}

// ===========================================================================
// イベントの発火
// ===========================================================================
namespace
{
struct Cand
{
    Tick u;
    std::int32_t b, t, e;
};

inline Tick CeilDiv(Tick a, Tick b) { return -FloorDiv(-a, b); }
} // namespace

void CollectEvents(const Sequence& seq, Tick u0, Tick u1, const EventCollectOptions& opt, std::vector<EventFire>& out)
{
    if (u0 == u1) return;
    const bool fwd = u1 > u0;
    if (!fwd && !opt.fireBackward) return;

    Tick lo = fwd ? u0 : u1;
    Tick hi = fwd ? u1 : u0;
    bool loInc = fwd ? opt.startInclusive : true;
    bool hiInc = fwd ? true : opt.startInclusive;

    const Tick P = opt.rangeEnd - opt.rangeStart;
    LoopMode mode = opt.mode;
    if (P <= 0) mode = LoopMode::Once;
    const Tick period = (mode == LoopMode::PingPong) ? 2 * P : P;
    if (mode != LoopMode::Once)
    {
        constexpr Tick kMaxLaps = 64;
        const Tick maxSpan = kMaxLaps * period;
        if (hi - lo > maxSpan)
        {
            if (fwd) { lo = hi - maxSpan; loInc = false; }
            else     { hi = lo + maxSpan; hiInc = false; }
        }
    }
    auto inside = [&](Tick u) {
        return (u > lo || (loInc && u == lo)) && (u < hi || (hiInc && u == hi));
    };

    std::vector<Cand> found;
    auto scan = [&](Tick base, std::int32_t b, std::int32_t t, std::int32_t e) {
        const Tick kmin = CeilDiv(lo - base, period);
        const Tick kmax = FloorDiv(hi - base, period);
        for (Tick k = kmin; k <= kmax; ++k)
        {
            const Tick u = base + k * period;
            if (inside(u)) found.push_back({ u, b, t, e });
        }
    };

    for (std::size_t b = 0; b < seq.bindings.size(); ++b)
        for (std::size_t ti = 0; ti < seq.bindings[b].tracks.size(); ++ti)
        {
            const Track& tr = seq.bindings[b].tracks[ti];
            if (tr.mute || tr.events.empty()) continue;
            for (std::size_t ei = 0; ei < tr.events.size(); ++ei)
            {
                const EventItem& ev = tr.events[ei];
                const std::int32_t bi = static_cast<std::int32_t>(b), tri = static_cast<std::int32_t>(ti),
                                   evi = static_cast<std::int32_t>(ei);
                if (mode == LoopMode::Once)
                {
                    if (inside(ev.t)) found.push_back({ ev.t, bi, tri, evi });
                    continue;
                }
                // 範囲外のイベントは周回では鳴らない。Loop は [start, end) のみ(end ちょうどは次周の start と同じ瞬間なので鳴らさない)
                if (ev.t < opt.rangeStart || ev.t > opt.rangeEnd || (mode == LoopMode::Loop && ev.t == opt.rangeEnd)) continue;
                const Tick r = ev.t - opt.rangeStart;
                scan(opt.rangeStart + r, bi, tri, evi);                        // 前進側の通過
                if (mode == LoopMode::PingPong && r != 0 && r != P)
                    scan(opt.rangeStart + 2 * P - r, bi, tri, evi);            // 折り返し後(後退側)の通過
            }
        }

    std::sort(found.begin(), found.end(), [](const Cand& a, const Cand& b) {
        if (a.u != b.u) return a.u < b.u;
        if (a.b != b.b) return a.b < b.b;
        if (a.t != b.t) return a.t < b.t;
        return a.e < b.e;
    });
    if (!fwd) std::reverse(found.begin(), found.end());
    for (const Cand& c : found) out.push_back({ c.b, c.t, c.e, c.u });
}

// ===========================================================================
// SeqPlayback
// ===========================================================================
void SeqPlayback::Reset(const Sequence& seq, LoopMode mode)
{
    GetRange(seq, m_start, m_end);
    m_mode = (m_end > m_start) ? mode : LoopMode::Once;
    m_unwrapped = m_start;
    m_frac = 0.0;
    m_playing = false;
    m_armStart = false;
    m_finished = false;
}

Tick SeqPlayback::Position() const
{
    const Tick P = m_end - m_start;
    switch (m_mode)
    {
    case LoopMode::Loop:
        return P > 0 ? m_start + FloorMod(m_unwrapped - m_start, P) : m_start;
    case LoopMode::PingPong:
    {
        if (P <= 0) return m_start;
        const Tick m = FloorMod(m_unwrapped - m_start, 2 * P);
        return m_start + (m <= P ? m : 2 * P - m);
    }
    default:
        return std::clamp(m_unwrapped, m_start, std::max(m_start, m_end));
    }
}

void SeqPlayback::Seek(Tick pos)
{
    m_unwrapped = std::clamp(pos, m_start, std::max(m_start, m_end));
    m_frac = 0.0;
    m_armStart = false;
    m_finished = false;
}

void SeqPlayback::Play()
{
    m_playing = true;
    const Tick pos = Position();
    m_armStart = (m_rate >= 0.0) ? (pos == m_start) : (pos == m_end);
}

void SeqPlayback::Advance(const Sequence& seq, double dtSeconds, std::vector<EventFire>& out)
{
    if (!m_playing) return;
    double total = m_frac + dtSeconds * m_rate * static_cast<double>(seq.ticksPerSecond);
    const double rounded = std::round(total);
    if (std::fabs(total - rounded) < 1e-9) total = rounded;      // 浮動小数の誤差で 1 tick 遅れないように
    // 巨大な dt / rate でも int64 に収まるよう頭打ちする(kMaxTick 相当の移動 = 実質「端まで」)
    if (!std::isfinite(total)) total = 0.0;
    total = std::clamp(total, -static_cast<double>(kMaxTick), static_cast<double>(kMaxTick));
    const double whole = std::floor(total);
    m_frac = total - whole;

    const Tick u0 = m_unwrapped;
    Tick u1 = u0 + static_cast<Tick>(whole);
    if (m_mode == LoopMode::Once) u1 = std::clamp(u1, m_start, std::max(m_start, m_end));

    EventCollectOptions o;
    o.rangeStart = m_start;
    o.rangeEnd = m_end;
    o.mode = m_mode;
    o.startInclusive = m_armStart;
    o.fireBackward = m_fireBackward;
    CollectEvents(seq, u0, u1, o, out);
    if (u1 != u0) m_armStart = false;
    m_unwrapped = u1;

    if (m_mode == LoopMode::Once)
    {
        const bool atEnd = (m_rate > 0.0 && u1 >= m_end) || (m_rate < 0.0 && u1 <= m_start);
        if (atEnd) { m_finished = true; m_playing = false; m_frac = 0.0; }
    }
}

} // namespace dx12e::seq
