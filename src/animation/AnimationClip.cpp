#include "animation/AnimationClip.h"

namespace dx12e
{

void AnimationClip::AddTrack(BoneTrack track)
{
    m_tracks.push_back(std::move(track));
}

const BoneTrack* AnimationClip::FindTrackForBone(u32 boneIndex) const
{
    for (const auto& track : m_tracks)
    {
        if (track.boneIndex == boneIndex)
        {
            return &track;
        }
    }
    return nullptr;
}

} // namespace dx12e
