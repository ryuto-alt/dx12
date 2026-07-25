#include "ecs/Components.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/SkinningBuffer.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"
#include "animation/AnimGraphRuntime.h"

using namespace DirectX;

namespace dx12e
{

XMMATRIX Transform::GetWorldMatrix() const
{
    XMMATRIX s = XMMatrixScaling(scale.x, scale.y, scale.z);

    XMMATRIX r;
    if (useQuaternion)
    {
        XMVECTOR q = XMLoadFloat4(&quaternion);
        r = XMMatrixRotationQuaternion(q);
    }
    else
    {
        // Euler degrees -> radians, YXZ order (Yaw -> Pitch -> Roll)
        r = XMMatrixRotationRollPitchYaw(
            XMConvertToRadians(rotation.x),   // pitch
            XMConvertToRadians(rotation.y),    // yaw
            XMConvertToRadians(rotation.z));   // roll
    }

    XMMATRIX t = XMMatrixTranslation(position.x, position.y, position.z);

    return s * r * t;
}

XMMATRIX ComputeWorldMatrix(const entt::registry& reg, entt::entity e)
{
    XMMATRIX world = XMMatrixIdentity();
    entt::entity cur = e;
    int depth = 0;  // 循環ガード
    while (cur != entt::null && reg.valid(cur) && reg.all_of<Transform>(cur)
           && depth++ < 64)
    {
        const auto& t = reg.get<Transform>(cur);
        world = world * t.GetWorldMatrix();
        cur = t.parent;
    }
    return world;
}

SkeletalAnimation::~SkeletalAnimation() = default;
SkeletalAnimation::SkeletalAnimation(SkeletalAnimation&&) noexcept = default;
SkeletalAnimation& SkeletalAnimation::operator=(SkeletalAnimation&&) noexcept = default;

// AnimGraphRuntimeState は Components.h では前方宣言なので、
// 特殊メンバは全部ここ（完全型が見える TU）で定義する。
AnimatorController::AnimatorController() = default;
AnimatorController::~AnimatorController() = default;
AnimatorController::AnimatorController(AnimatorController&&) noexcept = default;
AnimatorController& AnimatorController::operator=(AnimatorController&&) noexcept = default;

// コピーは「永続フィールドだけ」。_state / _loaded は既定のままにして、
// コピー先が次の Update で .animfsm を読み直すようにする。
AnimatorController::AnimatorController(const AnimatorController& other)
    : graphPath(other.graphPath)
    , playOnStart(other.playOnStart)
    , speed(other.speed)
    , applyRootMotion(other.applyRootMotion)
    , eventChannel(other.eventChannel)
{
}

AnimatorController& AnimatorController::operator=(const AnimatorController& other)
{
    if (this != &other)
    {
        graphPath       = other.graphPath;
        playOnStart     = other.playOnStart;
        speed           = other.speed;
        applyRootMotion = other.applyRootMotion;
        eventChannel    = other.eventChannel;
        _state.reset();
        _loaded = false;
        _failed = false;
        _loadedPath.clear();
    }
    return *this;
}

NodeAnimationComp::~NodeAnimationComp() = default;
NodeAnimationComp::NodeAnimationComp(NodeAnimationComp&&) noexcept = default;
NodeAnimationComp& NodeAnimationComp::operator=(NodeAnimationComp&&) noexcept = default;

} // namespace dx12e
