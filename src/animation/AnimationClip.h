#pragma once
#include <string>
#include <vector>
#include <DirectXMath.h>
#include "core/Types.h"
#include "animation/AnimEvent.h"

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

    // duration と全キー時刻を ticks → 秒へ正規化し、m_ticksPerSecond を 1 にする。
    // Assimp のキー時刻は ticks 単位（glTF は通常 1000 ticks/s = ミリ秒、FBX は 30 等）。
    // ロード直後に一度だけ呼ぶことで、以降 Animator は「秒」で統一的に扱える
    // （Update の deltaTime[秒] * tps(=1) がそのままキー時刻と比較可能になる）。
    // m_ticksPerSecond <= 0 の場合は 25 ticks/s（Assimp 既定）とみなす。
    void NormalizeToSeconds();

    void SetName(const std::string& name) { m_name = name; }
    const std::string& GetName() const    { return m_name; }

    // --- イベント（足音等。実体は .animfsm の "clipEvents" から流し込む）---
    // 時刻昇順を保って挿入する（CollectAnimEvents が返す添字順＝時刻順になる）。
    void AddEvent(AnimEvent ev);
    void ClearEvents() { m_events.clear(); }
    const std::vector<AnimEvent>& GetEvents() const { return m_events; }

private:
    std::string m_name;
    std::vector<BoneTrack> m_tracks;
    // boneIndex → m_tracks の添字（-1 = トラック無し）。FindTrackForBone を O(1) にする。
    // 以前は全トラックの線形探索だったので、ボーン数 × トラック数 の総当たりが
    // クロスフェード中はフレームあたり 2 回走っていた。
    std::vector<i32> m_boneToTrack;
    std::vector<AnimEvent> m_events;   // time 昇順（AddEvent が保証する）
    float m_duration       = 0.0f;
    float m_ticksPerSecond = 25.0f;
};

} // namespace dx12e
