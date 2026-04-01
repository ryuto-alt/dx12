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

} // namespace dx12e
