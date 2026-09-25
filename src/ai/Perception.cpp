#include "ai/Perception.h"

#include <algorithm>
#include <cmath>

namespace dx12e
{
namespace ai
{

ViewCheck CheckView(const f32 eye[3], f32 yawDeg, const f32 target[3], f32 range, f32 fovDeg, f32 nearRange)
{
    ViewCheck v;
    const f32 dx = target[0] - eye[0], dz = target[2] - eye[2];
    v.dist = std::sqrt(dx * dx + dz * dz);
    v.inRange = v.dist <= range;
    v.isNear = v.dist <= nearRange;
    const f32 yaw = yawDeg * 0.017453292519943295f;
    const f32 fx = std::sin(yaw), fz = std::cos(yaw);
    if (v.dist > 1e-4f)
    {
        const f32 c = std::clamp((fx * dx + fz * dz) / v.dist, -1.0f, 1.0f);
        v.angleDeg = std::acos(c) * 57.29577951308232f;
    }
    v.inFov = v.angleDeg <= fovDeg * 0.5f;
    return v;
}

f32 HearingReach(f32 soundRadius, f32 hearingScale, bool occluded, f32 occlusionFactor)
{
    f32 r = soundRadius * hearingScale;
    if (occluded) r *= occlusionFactor;
    return (std::max)(0.0f, r);
}

bool Hears(f32 dist, f32 reach, f32* loudness)
{
    if (reach <= 0.0f || dist > reach)
    {
        if (loudness) *loudness = 0.0f;
        return false;
    }
    if (loudness) *loudness = std::clamp(1.0f - dist / reach, 0.0f, 1.0f);
    return true;
}

f32 StepAwareness(f32 awareness, bool visible, f32 dt, f32 confirmTime, f32 decayPerSec)
{
    if (visible)
        awareness += (confirmTime > 0.0f) ? dt / confirmTime : 1.0f;
    else
        awareness -= decayPerSec * dt;
    return std::clamp(awareness, 0.0f, 1.0f);
}

void RememberSeen(TargetMemory& m, const f32 pos[3], f64 now)
{
    m.known = true;
    for (int k = 0; k < 3; ++k) { m.knownPos[k] = pos[k]; m.lastSeenPos[k] = pos[k]; }
    m.lastSeen = now;
    m.lastStimulus = now;
}

void RememberHeard(TargetMemory& m, const f32 pos[3], f64 now)
{
    // 見つけている最中は、目の方が正確なので上書きしない
    if (!m.seen)
    {
        m.known = true;
        for (int k = 0; k < 3; ++k) m.knownPos[k] = pos[k];
    }
    m.lastHeard = now;
    m.lastStimulus = (std::max)(m.lastStimulus, now);
}

bool ForgetIfStale(TargetMemory& m, f64 now, f32 memoryTime)
{
    if (!m.known || m.seen) return false;
    if (now - m.lastStimulus > static_cast<f64>(memoryTime))
    {
        m.known = false;
        return true;
    }
    return false;
}

} // namespace ai
} // namespace dx12e
