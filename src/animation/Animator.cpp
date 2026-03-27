#include "animation/Animator.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace dx12e
{

// ---------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------
void Animator::Initialize(const Skeleton* skeleton, const AnimationClip* clip)
{
    m_skeleton    = skeleton;
    m_clip        = clip;
    m_currentTime = 0.0f;

    u32 boneCount = skeleton->GetBoneCount();
    m_skinningMatrices.resize(boneCount);

    // 単位行列で初期化
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    for (u32 i = 0; i < boneCount; ++i)
    {
        m_skinningMatrices[i] = identity;
    }
}

// ---------------------------------------------------------------
// Update
// ---------------------------------------------------------------
void Animator::Update(float deltaTime)
{
    if (!m_skeleton || !m_clip)
    {
        return;
    }

    float duration = m_clip->GetDuration();
    if (duration <= 0.0f)
    {
        return;
    }

    // 1. 時間を進める（秒 → ticks に変換）
    m_currentTime += deltaTime * m_clip->GetTicksPerSecond();
    if (m_looping)
    {
        m_currentTime = std::fmod(m_currentTime, duration);
        if (m_currentTime < 0.0f)
        {
            m_currentTime += duration;
        }
    }
    else
    {
        m_currentTime = std::clamp(m_currentTime, 0.0f, duration);
    }

    u32 boneCount = m_skeleton->GetBoneCount();
    std::vector<XMMATRIX> globalMatrices(boneCount);

    // 2-4. 各ボーンのローカル変換を計算してグローバル行列を組み立てる
    for (u32 i = 0; i < boneCount; ++i)
    {
        const BoneNode& bone = m_skeleton->GetBone(i);
        const BoneTrack* track = m_clip->FindTrackForBone(i);

        XMMATRIX localMatrix;

        if (track)
        {
            // キーフレーム補間でローカルTRSを計算
            XMFLOAT3 pos   = InterpolatePosition(track->positionKeys, m_currentTime);
            XMFLOAT4 rot   = InterpolateRotation(track->rotationKeys, m_currentTime);
            XMFLOAT3 scale = InterpolateScale(track->scaleKeys, m_currentTime);

            XMMATRIX S = XMMatrixScalingFromVector(XMLoadFloat3(&scale));
            XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&rot));
            XMMATRIX T = XMMatrixTranslationFromVector(XMLoadFloat3(&pos));

            // S * R * T
            localMatrix = S * R * T;
        }
        else
        {
            // トラックが無いボーンはlocalBindPoseを使う
            localMatrix = XMLoadFloat4x4(&bone.localBindPose);
        }

        // グローバル行列: local * parent_global
        if (bone.parentIndex >= 0)
        {
            globalMatrices[i] = localMatrix * globalMatrices[static_cast<u32>(bone.parentIndex)];
        }
        else
        {
            globalMatrices[i] = localMatrix;
        }
    }

    // 5-6. スキニング行列を計算して格納
    for (u32 i = 0; i < boneCount; ++i)
    {
        const BoneNode& bone = m_skeleton->GetBone(i);
        XMMATRIX invBind = XMLoadFloat4x4(&bone.inverseBindPose);

        // skinning = inverseBindPose * globalPose
        XMMATRIX skinning = invBind * globalMatrices[i];

        // HLSL column-major 用に転置して格納
        XMStoreFloat4x4(&m_skinningMatrices[i], XMMatrixTranspose(skinning));
    }
}

// ---------------------------------------------------------------
// InterpolatePosition (lerp)
// ---------------------------------------------------------------
XMFLOAT3 Animator::InterpolatePosition(
    const std::vector<Keyframe<XMFLOAT3>>& keys, float time) const
{
    if (keys.empty())
    {
        return XMFLOAT3(0.0f, 0.0f, 0.0f);
    }
    if (keys.size() == 1 || time <= keys.front().time)
    {
        return keys.front().value;
    }
    if (time >= keys.back().time)
    {
        return keys.back().value;
    }

    // バイナリサーチで前後キーフレームを特定
    auto it = std::upper_bound(keys.begin(), keys.end(), time,
        [](float t, const Keyframe<XMFLOAT3>& kf) { return t < kf.time; });

    auto& next = *it;
    auto& prev = *(it - 1);

    float segmentLen = next.time - prev.time;
    float factor = (segmentLen > 0.0f) ? (time - prev.time) / segmentLen : 0.0f;

    XMVECTOR v0 = XMLoadFloat3(&prev.value);
    XMVECTOR v1 = XMLoadFloat3(&next.value);
    XMVECTOR result = XMVectorLerp(v0, v1, factor);

    XMFLOAT3 out;
    XMStoreFloat3(&out, result);
    return out;
}

// ---------------------------------------------------------------
// InterpolateRotation (slerp)
// ---------------------------------------------------------------
XMFLOAT4 Animator::InterpolateRotation(
    const std::vector<Keyframe<XMFLOAT4>>& keys, float time) const
{
    if (keys.empty())
    {
        return XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f); // identity quaternion
    }
    if (keys.size() == 1 || time <= keys.front().time)
    {
        return keys.front().value;
    }
    if (time >= keys.back().time)
    {
        return keys.back().value;
    }

    // バイナリサーチで前後キーフレームを特定
    auto it = std::upper_bound(keys.begin(), keys.end(), time,
        [](float t, const Keyframe<XMFLOAT4>& kf) { return t < kf.time; });

    auto& next = *it;
    auto& prev = *(it - 1);

    float segmentLen = next.time - prev.time;
    float factor = (segmentLen > 0.0f) ? (time - prev.time) / segmentLen : 0.0f;

    XMVECTOR q0 = XMLoadFloat4(&prev.value);
    XMVECTOR q1 = XMLoadFloat4(&next.value);
    XMVECTOR result = XMQuaternionSlerp(q0, q1, factor);

    XMFLOAT4 out;
    XMStoreFloat4(&out, result);
    return out;
}

// ---------------------------------------------------------------
// InterpolateScale (lerp)
// ---------------------------------------------------------------
XMFLOAT3 Animator::InterpolateScale(
    const std::vector<Keyframe<XMFLOAT3>>& keys, float time) const
{
    if (keys.empty())
    {
        return XMFLOAT3(1.0f, 1.0f, 1.0f);
    }
    if (keys.size() == 1 || time <= keys.front().time)
    {
        return keys.front().value;
    }
    if (time >= keys.back().time)
    {
        return keys.back().value;
    }

    // バイナリサーチで前後キーフレームを特定
    auto it = std::upper_bound(keys.begin(), keys.end(), time,
        [](float t, const Keyframe<XMFLOAT3>& kf) { return t < kf.time; });

    auto& next = *it;
    auto& prev = *(it - 1);

    float segmentLen = next.time - prev.time;
    float factor = (segmentLen > 0.0f) ? (time - prev.time) / segmentLen : 0.0f;

    XMVECTOR v0 = XMLoadFloat3(&prev.value);
    XMVECTOR v1 = XMLoadFloat3(&next.value);
    XMVECTOR result = XMVectorLerp(v0, v1, factor);

    XMFLOAT3 out;
    XMStoreFloat3(&out, result);
    return out;
}

} // namespace dx12e
