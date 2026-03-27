#pragma once
#include <string>
#include <vector>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

template<typename T>
struct Keyframe
{
    float time;
    T     value;
};

struct BoneTrack
{
    u32 boneIndex = 0;
    std::vector<Keyframe<DirectX::XMFLOAT3>> positionKeys;
    std::vector<Keyframe<DirectX::XMFLOAT4>> rotationKeys;  // quaternion XYZW
    std::vector<Keyframe<DirectX::XMFLOAT3>> scaleKeys;
};

class AnimationClip
{
public:
    void  AddTrack(BoneTrack track);
    float GetDuration() const        { return m_duration; }
    void  SetDuration(float d)       { m_duration = d; }
    float GetTicksPerSecond() const  { return m_ticksPerSecond; }
    void  SetTicksPerSecond(float tps) { m_ticksPerSecond = tps; }
    u32   GetTrackCount() const      { return static_cast<u32>(m_tracks.size()); }

    const BoneTrack& GetTrack(u32 i) const          { return m_tracks[i]; }
    const BoneTrack* FindTrackForBone(u32 boneIndex) const;

    void SetName(const std::string& name) { m_name = name; }
    const std::string& GetName() const    { return m_name; }

private:
    std::string m_name;
    std::vector<BoneTrack> m_tracks;
    float m_duration       = 0.0f;
    float m_ticksPerSecond = 25.0f;
};

} // namespace dx12e
