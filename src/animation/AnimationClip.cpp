#include "animation/AnimationClip.h"

#include <algorithm>

namespace dx12e
{

void AnimationClip::AddTrack(BoneTrack track)
{
    const u32 bone = track.boneIndex;
    const i32 slot = static_cast<i32>(m_tracks.size());
    m_tracks.push_back(std::move(track));

    if (bone >= m_boneToTrack.size())
        m_boneToTrack.resize(static_cast<size_t>(bone) + 1, -1);
    // 同じボーンに複数トラックが来たら「最初のもの」を残す（旧・線形探索と同じ勝ち方）。
    if (m_boneToTrack[bone] < 0)
        m_boneToTrack[bone] = slot;
}

const BoneTrack* AnimationClip::FindTrackForBone(u32 boneIndex) const
{
    if (boneIndex >= m_boneToTrack.size()) return nullptr;
    const i32 slot = m_boneToTrack[boneIndex];
    return (slot >= 0) ? &m_tracks[static_cast<size_t>(slot)] : nullptr;
}

void AnimationClip::AddEvent(AnimEvent ev)
{
    // time 昇順を保って挿入する（線形探索で十分な件数）。
    auto it = std::upper_bound(m_events.begin(), m_events.end(), ev.time,
        [](float t, const AnimEvent& e) { return t < e.time; });
    m_events.insert(it, std::move(ev));
}

void AnimationClip::NormalizeToSeconds()
{
    const float tps = (m_ticksPerSecond > 0.0f) ? m_ticksPerSecond : 25.0f;
    const float inv = 1.0f / tps;

    m_duration *= inv;
    for (auto& track : m_tracks)
    {
        for (auto& k : track.positionKeys) k.time *= inv;
        for (auto& k : track.rotationKeys) k.time *= inv;
        for (auto& k : track.scaleKeys)    k.time *= inv;
    }
    // イベント時刻も一緒に正規化する（忘れると足音だけ ticks 単位のまま残る）。
    // 通常イベントは .animfsm から「正規化後」に流し込まれるので空だが、
    // 将来ローダ側が埋めるようになったときのために揃えておく。
    for (auto& ev : m_events) ev.time *= inv;

    m_ticksPerSecond = 1.0f;
}

} // namespace dx12e
