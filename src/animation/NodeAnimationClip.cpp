#include "animation/NodeAnimationClip.h"

namespace dx12e
{

void NodeAnimationClip::AddTrack(NodeTrack track)
{
    m_tracks.push_back(std::move(track));
}

const NodeTrack* NodeAnimationClip::FindTrackForNode(u32 nodeIndex) const
{
    for (const auto& track : m_tracks)
    {
        if (track.nodeIndex == nodeIndex)
        {
            return &track;
        }
    }
    return nullptr;
}

void NodeAnimationClip::NormalizeToSeconds()
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
    m_ticksPerSecond = 1.0f;
}

} // namespace dx12e
