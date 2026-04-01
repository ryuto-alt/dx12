#pragma once
#include <string>
#include <vector>
#include <DirectXMath.h>
#include "core/Types.h"
#include "animation/AnimationClip.h"  // Keyframe<T> の再利用

namespace dx12e
{

struct NodeTrack
{
    u32 nodeIndex = 0;  // NodeGraph 内のインデックス
    std::vector<Keyframe<DirectX::XMFLOAT3>> positionKeys;
    std::vector<Keyframe<DirectX::XMFLOAT4>> rotationKeys;  // quaternion XYZW
    std::vector<Keyframe<DirectX::XMFLOAT3>> scaleKeys;
};

class NodeAnimationClip
{
public:
    void  AddTrack(NodeTrack track);
    float GetDuration() const        { return m_duration; }
    void  SetDuration(float d)       { m_duration = d; }
    float GetTicksPerSecond() const  { return m_ticksPerSecond; }
    void  SetTicksPerSecond(float tps) { m_ticksPerSecond = tps; }
    u32   GetTrackCount() const      { return static_cast<u32>(m_tracks.size()); }

    const NodeTrack& GetTrack(u32 i) const          { return m_tracks[i]; }
    const NodeTrack* FindTrackForNode(u32 nodeIndex) const;

    void SetName(const std::string& name) { m_name = name; }
    const std::string& GetName() const    { return m_name; }

private:
    std::string m_name;
    std::vector<NodeTrack> m_tracks;
    float m_duration       = 0.0f;
    float m_ticksPerSecond = 25.0f;
};

} // namespace dx12e
