#include "animation/NodeAnimator.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace dx12e
{

// ---------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------
void NodeAnimator::Initialize(const NodeGraph* graph,
                              const NodeAnimationClip* playClip,
                              const NodeAnimationClip* restClip)
{
    m_graph       = graph;
    m_clip        = playClip;
    m_currentTime = 0.0f;
    m_nextClip    = nullptr;
    m_blending    = false;

    u32 nodeCount = graph->GetNodeCount();
    m_nodeGlobalMatrices.resize(nodeCount);

    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    for (u32 i = 0; i < nodeCount; ++i)
    {
        m_nodeGlobalMatrices[i] = identity;
    }

    // restClip の time=0 で各ノードの「ローカル行列」と「ピボット位置」を計算
    // ローカル差分 + ピボット方式：親ノードのスケール/回転の影響を受けない
    m_restLocalMatrices.resize(nodeCount);
    m_inverseRestLocalMatrices.resize(nodeCount);
    m_pivotPositions.resize(nodeCount);

    // まず rest のグローバル行列を計算してピボット位置を抽出
    std::vector<XMMATRIX> restGlobals(nodeCount, XMMatrixIdentity());

    for (u32 i = 0; i < nodeCount; ++i)
    {
        const SceneNode& node = graph->GetNode(i);
        const NodeTrack* track = restClip->FindTrackForNode(i);

        XMMATRIX localMatrix;
        if (track)
        {
            XMFLOAT3 pos = !track->positionKeys.empty() ? track->positionKeys.front().value : XMFLOAT3(0, 0, 0);
            XMFLOAT4 rot = !track->rotationKeys.empty() ? track->rotationKeys.front().value : XMFLOAT4(0, 0, 0, 1);
            XMFLOAT3 scl = !track->scaleKeys.empty()    ? track->scaleKeys.front().value    : XMFLOAT3(1, 1, 1);

            XMMATRIX S = XMMatrixScalingFromVector(XMLoadFloat3(&scl));
            XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&rot));
            XMMATRIX T = XMMatrixTranslationFromVector(XMLoadFloat3(&pos));
            localMatrix = S * R * T;
        }
        else
        {
            localMatrix = XMLoadFloat4x4(&node.localDefault);
        }

        // ローカル行列と逆行列を保存
        XMStoreFloat4x4(&m_restLocalMatrices[i], localMatrix);
        XMVECTOR det;
        XMMATRIX invLocal = XMMatrixInverse(&det, localMatrix);
        XMStoreFloat4x4(&m_inverseRestLocalMatrices[i], invLocal);

        // グローバル行列を計算（ピボット位置抽出用）
        if (node.parentIndex >= 0)
        {
            restGlobals[i] = localMatrix * restGlobals[static_cast<u32>(node.parentIndex)];
        }
        else
        {
            restGlobals[i] = localMatrix;
        }

        // ピボット = rest global の位置成分（row 3 の xyz）
        XMFLOAT4X4 globalF;
        XMStoreFloat4x4(&globalF, restGlobals[i]);
        m_pivotPositions[i] = XMFLOAT3(globalF._41, globalF._42, globalF._43);
    }
}

// ---------------------------------------------------------------
// SetClip
// ---------------------------------------------------------------
void NodeAnimator::SetClip(const NodeAnimationClip* clip)
{
    m_clip        = clip;
    m_currentTime = 0.0f;
    m_blending    = false;
    m_nextClip    = nullptr;
}

// ---------------------------------------------------------------
// CrossFadeTo
// ---------------------------------------------------------------
void NodeAnimator::CrossFadeTo(const NodeAnimationClip* nextClip, float blendDuration)
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
// ComputeRawNodeMatrices - 素のグローバル行列を計算
// ---------------------------------------------------------------
void NodeAnimator::ComputeRawNodeMatrices(
    const NodeAnimationClip* clip, float time,
    std::vector<XMFLOAT4X4>& outMatrices) const
{
    u32 nodeCount = m_graph->GetNodeCount();
    outMatrices.resize(nodeCount);

    std::vector<XMMATRIX> globalMatrices(nodeCount, XMMatrixIdentity());

    for (u32 i = 0; i < nodeCount; ++i)
    {
        const SceneNode& node = m_graph->GetNode(i);
        const NodeTrack* track = clip->FindTrackForNode(i);

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
            localMatrix = XMLoadFloat4x4(&node.localDefault);
        }

        if (node.parentIndex >= 0)
        {
            globalMatrices[i] = localMatrix * globalMatrices[static_cast<u32>(node.parentIndex)];
        }
        else
        {
            globalMatrices[i] = localMatrix;
        }
    }

    // 常に素のグローバル行列を返す（inverseRest の適用は呼び出し側で行う）
    for (u32 i = 0; i < nodeCount; ++i)
    {
        XMStoreFloat4x4(&outMatrices[i], globalMatrices[i]);
    }
}

// ---------------------------------------------------------------
// Update
// ---------------------------------------------------------------
void NodeAnimator::Update(float deltaTime)
{
    if (!m_graph || !m_clip)
    {
        return;
    }

    float duration = m_clip->GetDuration();
    if (duration <= 0.0f)
    {
        return;
    }

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

        m_blendFactor += deltaTime / m_blendDuration;

        if (m_blendFactor >= 1.0f)
        {
            m_clip        = m_nextClip;
            m_currentTime = m_nextTime;
            m_nextClip    = nullptr;
            m_blending    = false;
            m_blendFactor = 0.0f;

            ComputeRawNodeMatrices(m_clip, m_currentTime, m_nodeGlobalMatrices);
        }
        else
        {
            std::vector<XMFLOAT4X4> currentMatrices;
            std::vector<XMFLOAT4X4> nextMatrices;

            ComputeRawNodeMatrices(m_clip, m_currentTime, currentMatrices);
            ComputeRawNodeMatrices(m_nextClip, m_nextTime, nextMatrices);

            u32 nodeCount = m_graph->GetNodeCount();
            m_nodeGlobalMatrices.resize(nodeCount);

            float t = m_blendFactor;
            for (u32 i = 0; i < nodeCount; ++i)
            {
                XMMATRIX matA = XMLoadFloat4x4(&currentMatrices[i]);
                XMMATRIX matB = XMLoadFloat4x4(&nextMatrices[i]);

                XMMATRIX blended;
                blended.r[0] = XMVectorLerp(matA.r[0], matB.r[0], t);
                blended.r[1] = XMVectorLerp(matA.r[1], matB.r[1], t);
                blended.r[2] = XMVectorLerp(matA.r[2], matB.r[2], t);
                blended.r[3] = XMVectorLerp(matA.r[3], matB.r[3], t);

                XMStoreFloat4x4(&m_nodeGlobalMatrices[i], blended);
            }
        }
    }
    else
    {
        ComputeRawNodeMatrices(m_clip, m_currentTime, m_nodeGlobalMatrices);
    }

    // SRT分解で回転差分を安全に計算（逆行列を使わない）
    if (!m_restLocalMatrices.empty())
    {
        u32 nodeCount = m_graph->GetNodeCount();
        for (u32 i = 0; i < nodeCount; ++i)
        {
            const NodeTrack* track = m_clip ? m_clip->FindTrackForNode(i) : nullptr;

            if (!track)
            {
                // アニメーションチャンネルがないノードは差分なし
                XMStoreFloat4x4(&m_nodeGlobalMatrices[i], XMMatrixIdentity());
                continue;
            }

            // current の SRT
            XMFLOAT3 curPos   = InterpolatePosition(track->positionKeys, m_currentTime);
            XMFLOAT4 curRot   = InterpolateRotation(track->rotationKeys, m_currentTime);
            XMFLOAT3 curScale = InterpolateScale(track->scaleKeys, m_currentTime);

            // rest の SRT を分解
            XMMATRIX restLocal = XMLoadFloat4x4(&m_restLocalMatrices[i]);
            XMVECTOR restS, restR, restT;
            XMMatrixDecompose(&restS, &restR, &restT, restLocal);

            // 回転差分 = inv(rest_R) * current_R
            XMVECTOR curR = XMLoadFloat4(&curRot);
            XMVECTOR diffR = XMQuaternionMultiply(XMQuaternionInverse(restR), curR);

            // 原点基準の回転差分（ピボットは将来対応）
            XMMATRIX result = XMMatrixRotationQuaternion(diffR);

            XMStoreFloat4x4(&m_nodeGlobalMatrices[i], result);
        }
    }
}

// ---------------------------------------------------------------
// InterpolatePosition (lerp)
// ---------------------------------------------------------------
XMFLOAT3 NodeAnimator::InterpolatePosition(
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
XMFLOAT4 NodeAnimator::InterpolateRotation(
    const std::vector<Keyframe<XMFLOAT4>>& keys, float time) const
{
    if (keys.empty())
    {
        return XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    if (keys.size() == 1 || time <= keys.front().time)
    {
        return keys.front().value;
    }
    if (time >= keys.back().time)
    {
        return keys.back().value;
    }

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
XMFLOAT3 NodeAnimator::InterpolateScale(
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
