#include "animation/AnimPose.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace dx12e
{

// ---------------------------------------------------------------
// キーフレーム補間
// ---------------------------------------------------------------
XMFLOAT3 SampleVec3Track(const std::vector<Keyframe<XMFLOAT3>>& keys, float time, XMFLOAT3 fallback)
{
    if (keys.empty())                                return fallback;
    if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time)                    return keys.back().value;

    auto it = std::upper_bound(keys.begin(), keys.end(), time,
        [](float t, const Keyframe<XMFLOAT3>& kf) { return t < kf.time; });

    const Keyframe<XMFLOAT3>& next = *it;
    const Keyframe<XMFLOAT3>& prev = *(it - 1);

    const float segmentLen = next.time - prev.time;
    const float factor = (segmentLen > 0.0f) ? (time - prev.time) / segmentLen : 0.0f;

    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVectorLerp(XMLoadFloat3(&prev.value), XMLoadFloat3(&next.value), factor));
    return out;
}

XMFLOAT4 SampleQuatTrack(const std::vector<Keyframe<XMFLOAT4>>& keys, float time)
{
    if (keys.empty())                                return XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time)                    return keys.back().value;

    auto it = std::upper_bound(keys.begin(), keys.end(), time,
        [](float t, const Keyframe<XMFLOAT4>& kf) { return t < kf.time; });

    const Keyframe<XMFLOAT4>& next = *it;
    const Keyframe<XMFLOAT4>& prev = *(it - 1);

    const float segmentLen = next.time - prev.time;
    const float factor = (segmentLen > 0.0f) ? (time - prev.time) / segmentLen : 0.0f;

    XMFLOAT4 out;
    XMStoreFloat4(&out, XMQuaternionSlerp(XMLoadFloat4(&prev.value), XMLoadFloat4(&next.value), factor));
    return out;
}

// ---------------------------------------------------------------
// ポーズ生成
// ---------------------------------------------------------------
void MakeBindPose(const Skeleton& skeleton, AnimPose& out)
{
    const u32 boneCount = skeleton.GetBoneCount();
    out.resize(boneCount);
    for (u32 i = 0; i < boneCount; ++i)
    {
        const BoneNode& b = skeleton.GetBone(i);
        out[i].t = b.bindT;
        out[i].r = b.bindR;
        out[i].s = b.bindS;
    }
}

void SamplePose(const AnimationClip& clip, float time, const Skeleton& skeleton, AnimPose& out)
{
    const u32 boneCount = skeleton.GetBoneCount();
    out.resize(boneCount);

    for (u32 i = 0; i < boneCount; ++i)
    {
        const BoneNode&  bone  = skeleton.GetBone(i);
        const BoneTrack* track = clip.FindTrackForBone(i);

        if (track)
        {
            // 旧 Animator::ComputeBoneMatrices と同じ既定値
            // （トラックはあるがキー列が空、というケースの互換）。
            out[i].t = SampleVec3Track(track->positionKeys, time, XMFLOAT3(0.0f, 0.0f, 0.0f));
            out[i].r = SampleQuatTrack(track->rotationKeys, time);
            out[i].s = SampleVec3Track(track->scaleKeys,    time, XMFLOAT3(1.0f, 1.0f, 1.0f));
        }
        else
        {
            out[i].t = bone.bindT;
            out[i].r = bone.bindR;
            out[i].s = bone.bindS;
        }
    }
}

// ---------------------------------------------------------------
// ブレンド
// ---------------------------------------------------------------
void BlendPose(const AnimPose& a, const AnimPose& b, float t, AnimPose& out)
{
    const size_t n = a.size();
    out.resize(n);

    const float tc = std::clamp(t, 0.0f, 1.0f);
    const size_t common = (std::min)(n, b.size());

    for (size_t i = 0; i < common; ++i)
    {
        XMStoreFloat3(&out[i].t, XMVectorLerp(XMLoadFloat3(&a[i].t), XMLoadFloat3(&b[i].t), tc));
        XMStoreFloat3(&out[i].s, XMVectorLerp(XMLoadFloat3(&a[i].s), XMLoadFloat3(&b[i].s), tc));
        // XMQuaternionSlerp は内部で近傍（内積が負なら反転）を処理する。
        XMStoreFloat4(&out[i].r, XMQuaternionSlerp(XMLoadFloat4(&a[i].r), XMLoadFloat4(&b[i].r), tc));
    }
    for (size_t i = common; i < n; ++i)
        out[i] = a[i];
}

void BlendPoseAccum(AnimPose& dst, const AnimPose& src, float w)
{
    if (w <= 0.0f) return;
    const float wc = std::clamp(w, 0.0f, 1.0f);
    const size_t n = (std::min)(dst.size(), src.size());

    for (size_t i = 0; i < n; ++i)
    {
        XMStoreFloat3(&dst[i].t, XMVectorLerp(XMLoadFloat3(&dst[i].t), XMLoadFloat3(&src[i].t), wc));
        XMStoreFloat3(&dst[i].s, XMVectorLerp(XMLoadFloat3(&dst[i].s), XMLoadFloat3(&src[i].s), wc));

        // nlerp 相当の逐次累積。四元数は「内積が負なら反転」してから混ぜる
        // （XMQuaternionSlerp と違い、生の lerp は近傍を自分で処理する必要がある）。
        XMVECTOR q0 = XMLoadFloat4(&dst[i].r);
        XMVECTOR q1 = XMLoadFloat4(&src[i].r);
        if (XMVectorGetX(XMVector4Dot(q0, q1)) < 0.0f)
            q1 = XMVectorNegate(q1);
        XMVECTOR q = XMVectorLerp(q0, q1, wc);
        if (XMVectorGetX(XMVector4LengthSq(q)) < 1e-12f)
            q = q0;   // 真逆の四元数を 0.5 で混ぜた退化ケース
        XMStoreFloat4(&dst[i].r, XMQuaternionNormalize(q));
    }
}

void AdditiveBlendPose(AnimPose& dst, const AnimPose& add, const AnimPose& ref, float w, const float* mask)
{
    const size_t n = (std::min)(dst.size(), (std::min)(add.size(), ref.size()));
    for (size_t i = 0; i < n; ++i)
    {
        float wi = w;
        if (mask) wi *= mask[i];
        wi = std::clamp(wi, 0.0f, 1.0f);
        if (wi <= 0.0f) continue;

        // 位置 / スケールは単純な差分の加算
        XMVECTOR dt = XMVectorSubtract(XMLoadFloat3(&add[i].t), XMLoadFloat3(&ref[i].t));
        XMVECTOR ds = XMVectorSubtract(XMLoadFloat3(&add[i].s), XMLoadFloat3(&ref[i].s));
        XMStoreFloat3(&dst[i].t, XMVectorMultiplyAdd(dt, XMVectorReplicate(wi), XMLoadFloat3(&dst[i].t)));
        XMStoreFloat3(&dst[i].s, XMVectorMultiplyAdd(ds, XMVectorReplicate(wi), XMLoadFloat3(&dst[i].s)));

        // 回転は「参照ポーズからの相対回転」を重みぶんだけ適用
        XMVECTOR qAdd = XMLoadFloat4(&add[i].r);
        XMVECTOR qRef = XMLoadFloat4(&ref[i].r);
        XMVECTOR qDelta = XMQuaternionMultiply(XMQuaternionInverse(qRef), qAdd);
        XMVECTOR qScaled = XMQuaternionSlerp(XMQuaternionIdentity(), qDelta, wi);
        XMVECTOR qOut = XMQuaternionMultiply(qScaled, XMLoadFloat4(&dst[i].r));
        XMStoreFloat4(&dst[i].r, XMQuaternionNormalize(qOut));
    }
}

void OverrideBlendPose(AnimPose& dst, const AnimPose& src, float w, const float* mask)
{
    const size_t n = (std::min)(dst.size(), src.size());
    for (size_t i = 0; i < n; ++i)
    {
        float wi = w;
        if (mask) wi *= mask[i];
        wi = std::clamp(wi, 0.0f, 1.0f);
        if (wi <= 0.0f) continue;
        if (wi >= 1.0f) { dst[i] = src[i]; continue; }

        XMStoreFloat3(&dst[i].t, XMVectorLerp(XMLoadFloat3(&dst[i].t), XMLoadFloat3(&src[i].t), wi));
        XMStoreFloat3(&dst[i].s, XMVectorLerp(XMLoadFloat3(&dst[i].s), XMLoadFloat3(&src[i].s), wi));
        XMStoreFloat4(&dst[i].r, XMQuaternionSlerp(XMLoadFloat4(&dst[i].r), XMLoadFloat4(&src[i].r), wi));
    }
}

// ---------------------------------------------------------------
// FK / スキニング
// ---------------------------------------------------------------
XMMATRIX BoneTRSToMatrix(const BoneTRS& b)
{
    // 行ベクトル規約: S * R * T（旧 Animator.cpp と同一）
    const XMMATRIX S = XMMatrixScalingFromVector(XMLoadFloat3(&b.s));
    const XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&b.r));
    const XMMATRIX T = XMMatrixTranslationFromVector(XMLoadFloat3(&b.t));
    return S * R * T;
}

void ComputeGlobalMatrices(const Skeleton& skeleton, const AnimPose& pose,
                           std::vector<XMFLOAT4X4>& out)
{
    const u32 boneCount = skeleton.GetBoneCount();
    out.resize(boneCount);

    for (u32 i = 0; i < boneCount; ++i)
    {
        const BoneNode& bone = skeleton.GetBone(i);

        XMMATRIX local = (i < pose.size())
                       ? BoneTRSToMatrix(pose[i])
                       : XMLoadFloat4x4(&bone.localBindPose);

        if (bone.parentIndex >= 0)
        {
            // 親が子より前に並んでいる前提（Skeleton::AreBonesCorrectlyOrdered）。
            const XMMATRIX parent = XMLoadFloat4x4(&out[static_cast<u32>(bone.parentIndex)]);
            local = local * parent;
        }
        XMStoreFloat4x4(&out[i], local);
    }
}

void GlobalToSkinning(const Skeleton& skeleton, const std::vector<XMFLOAT4X4>& globals,
                      std::vector<XMFLOAT4X4>& out)
{
    const u32 boneCount = (std::min)(skeleton.GetBoneCount(), static_cast<u32>(globals.size()));
    out.resize(skeleton.GetBoneCount());

    for (u32 i = 0; i < boneCount; ++i)
    {
        const XMMATRIX invBind  = XMLoadFloat4x4(&skeleton.GetBone(i).inverseBindPose);
        const XMMATRIX skinning = invBind * XMLoadFloat4x4(&globals[i]);
        XMStoreFloat4x4(&out[i], XMMatrixTranspose(skinning));
    }
}

} // namespace dx12e
