#include "ecs/Components.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/SkinningBuffer.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"

using namespace DirectX;

namespace dx12e
{

XMMATRIX Transform::GetWorldMatrix() const
{
    XMMATRIX s = XMMatrixScaling(scale.x, scale.y, scale.z);

    // Euler degrees -> radians, YXZ order (Yaw -> Pitch -> Roll)
    XMMATRIX r = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(rotation.x),   // pitch
        XMConvertToRadians(rotation.y),    // yaw
        XMConvertToRadians(rotation.z));   // roll

    XMMATRIX t = XMMatrixTranslation(position.x, position.y, position.z);

    return s * r * t;
}

SkeletalAnimation::~SkeletalAnimation() = default;
SkeletalAnimation::SkeletalAnimation(SkeletalAnimation&&) noexcept = default;
SkeletalAnimation& SkeletalAnimation::operator=(SkeletalAnimation&&) noexcept = default;

NodeAnimationComp::~NodeAnimationComp() = default;
NodeAnimationComp::NodeAnimationComp(NodeAnimationComp&&) noexcept = default;
NodeAnimationComp& NodeAnimationComp::operator=(NodeAnimationComp&&) noexcept = default;

} // namespace dx12e
