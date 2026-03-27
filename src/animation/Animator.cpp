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
    m_nextClip    = nullptr;
    m_blending    = false;

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
// SetClip - 即座にクリップを切り替え
// ---------------------------------------------------------------
void Animator::SetClip(const AnimationClip* clip)
{
    m_clip        = clip;
    m_currentTime = 0.0f;
    m_blending    = false;
    m_nextClip    = nullptr;
}

// ---------------------------------------------------------------
// CrossFadeTo - ブレンドで切り替え
// ---------------------------------------------------------------
void Animator::CrossFadeTo(const AnimationClip* nextClip, float blendDuration)
{
    if (!nextClip || nextClip == m_clip)
    {
        return;
    }

    m_nextClip      = nextClip;
    m_nextTime      = 0.0f;
    m_blendFactor   = 0.0f;
    m_blendDuration = blendDuration;
    m_blending      = true;
}

// ---------------------------------------------------------------
// ComputeBoneMatrices - 指定クリップ・時間でスキニング行列を計算
// ---------------------------------------------------------------
void Animator::ComputeBoneMatrices(
    const AnimationClip* clip, float time,
    std::vector<XMFLOAT4X4>& outMatrices) const
{
    u32 boneCount = m_skeleton->GetBoneCount();
    outMatrices.resize(boneCount);

    std::vector<XMMATRIX> globalMatrices(boneCount);

    for (u32 i = 0; i < boneCount; ++i)
    {
        const BoneNode& bone = m_skeleton->GetBone(i);
        const BoneTrack* track = clip->FindTrackForBone(i);

        XMMATRIX localMatrix;

        if (track)
        {
            XMFLOAT3 pos   = InterpolatePosition(track->positionKeys, time);
            XMFLOAT4 rot   = InterpolateRotation(track->rotationKeys, time);
            XMFLOAT3 scale = InterpolateScale(track->scaleKeys, time);

            XMMATRIX S = XMMatrixScalingFromVector(XMLoadFloat3(&scale));
            XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&rot));
            XMMATRIX T = XMMatrixTranslationFromVector(XMLoadFloat3(&pos));

            localMatrix = S * R * T;
        }
        else
        {
            localMatrix = XMLoadFloat4x4(&bone.localBindPose);
        }

        if (bone.parentIndex >= 0)
        {
            globalMatrices[i] = localMatrix * globalMatrices[static_cast<u32>(bone.parentIndex)];
        }
        else
        {
            globalMatrices[i] = localMatrix;
        }
    }

    for (u32 i = 0; i < boneCount; ++i)
    {
        const BoneNode& bone = m_skeleton->GetBone(i);
        XMMATRIX invBind = XMLoadFloat4x4(&bone.inverseBindPose);
        XMMATRIX skinning = invBind * globalMatrices[i];
        XMStoreFloat4x4(&outMatrices[i], XMMatrixTranspose(skinning));
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

    // 現在のクリップの時間を進める
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

    if (m_blending && m_nextClip)
    {
        // ブレンド先の時間を進める
        m_nextTime += deltaTime * m_nextClip->GetTicksPerSecond();
        float nextDuration = m_nextClip->GetDuration();
        if (nextDuration > 0.0f && m_looping)
        {
            m_nextTime = std::fmod(m_nextTime, nextDuration);
            if (m_nextTime < 0.0f)
            {
                m_nextTime += nextDuration;
            }
        }

        // ブレンドファクタを進める
        m_blendFactor += deltaTime / m_blendDuration;

        if (m_blendFactor >= 1.0f)
        {
            // ブレンド完了
            m_clip        = m_nextClip;
            m_currentTime = m_nextTime;
            m_nextClip    = nullptr;
            m_blending    = false;
            m_blendFactor = 0.0f;

            // 完了したクリップだけで行列計算
            ComputeBoneMatrices(m_clip, m_currentTime, m_skinningMatrices);
        }
        else
        {
            // 両クリップの行列を計算してlerp
            std::vector<XMFLOAT4X4> currentMatrices;
            std::vector<XMFLOAT4X4> nextMatrices;

            ComputeBoneMatrices(m_clip, m_currentTime, currentMatrices);
            ComputeBoneMatrices(m_nextClip, m_nextTime, nextMatrices);

            u32 boneCount = m_skeleton->GetBoneCount();
            m_skinningMatrices.resize(boneCount);

            float t = m_blendFactor;
            for (u32 i = 0; i < boneCount; ++i)
            {
                XMMATRIX matA = XMLoadFloat4x4(&currentMatrices[i]);
                XMMATRIX matB = XMLoadFloat4x4(&nextMatrices[i]);

                // 簡易lerp: 各要素ごとの線形補間
                XMMATRIX blended;
                blended.r[0] = XMVectorLerp(matA.r[0], matB.r[0], t);
                blended.r[1] = XMVectorLerp(matA.r[1], matB.r[1], t);
                blended.r[2] = XMVectorLerp(matA.r[2], matB.r[2], t);
                blended.r[3] = XMVectorLerp(matA.r[3], matB.r[3], t);

                XMStoreFloat4x4(&m_skinningMatrices[i], blended);
            }
        }
    }
    else
    {
        // ブレンドなし: 通常の行列計算
        ComputeBoneMatrices(m_clip, m_currentTime, m_skinningMatrices);
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
