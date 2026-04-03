#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

class Mesh;
struct Material;
class Skeleton;
class AnimationClip;
class Animator;
class SkinningBuffer;
class NodeGraph;
class NodeAnimationClip;
class NodeAnimator;

struct NameTag
{
    std::string name;
};

struct Transform
{
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 rotation = {0.0f, 0.0f, 0.0f};  // Euler degrees（エディタ表示用）
    DirectX::XMFLOAT3 scale    = {1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT4 quaternion = {0.0f, 0.0f, 0.0f, 1.0f}; // (x,y,z,w) 物理同期用
    bool useQuaternion = false; // true なら quaternion から行列を生成

    DirectX::XMMATRIX GetWorldMatrix() const;
};

struct MeshRenderer
{
    std::string modelPath; // アセット相対パス（シーン保存/読み込み用）
    std::vector<Mesh*>     meshes;
    std::vector<Material*> materials;
    std::vector<DirectX::XMFLOAT4X4> meshNodeTransforms;
};

struct SkeletalAnimation
{
    std::unique_ptr<Skeleton>       skeleton;
    std::unique_ptr<Animator>       animator;
    std::unique_ptr<SkinningBuffer> skinningBuffer;
    std::vector<std::unique_ptr<AnimationClip>> clips;

    SkeletalAnimation() = default;
    ~SkeletalAnimation();
    SkeletalAnimation(SkeletalAnimation&&) noexcept;
    SkeletalAnimation& operator=(SkeletalAnimation&&) noexcept;
};

struct NodeAnimationComp
{
    std::unique_ptr<NodeGraph>    nodeGraph;
    std::unique_ptr<NodeAnimator> nodeAnimator;
    std::vector<std::unique_ptr<NodeAnimationClip>> clips;

    NodeAnimationComp() = default;
    ~NodeAnimationComp();
    NodeAnimationComp(NodeAnimationComp&&) noexcept;
    NodeAnimationComp& operator=(NodeAnimationComp&&) noexcept;
};

struct GridPlane
{
    bool enabled = true;
};

struct PointLight
{
    DirectX::XMFLOAT3 color = {1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    f32 range     = 10.0f;
};

struct DirectionalLight
{
    DirectX::XMFLOAT3 direction = {0.0f, -1.0f, 0.0f};
    DirectX::XMFLOAT3 color     = {1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
};

struct CameraComponent
{
    f32  fovDegrees = 60.0f;
    f32  nearClip   = 0.1f;
    f32  farClip    = 1000.0f;
    bool isActive   = false;
};

// --- Physics Components ---

static constexpr uint32_t kInvalidBodyId = 0xFFFFFFFF;

enum class MotionType : uint8_t
{
    Static    = 0,
    Kinematic = 1,
    Dynamic   = 2,
};

struct RigidBody
{
    MotionType motionType    = MotionType::Dynamic;
    f32        mass          = 1.0f;
    f32        restitution   = 0.4f;   // 適度に弾む
    f32        friction      = 0.3f;   // 低め → 滑りやすく不安定に
    f32        linearDamping  = 0.02f;  // 移動減衰を弱く
    f32        angularDamping = 0.01f;  // 回転減衰を弱く → 倒れやすい
    bool       useGravity    = true;

    // PhysicsSystem が管理（ユーザーは触らない）
    uint32_t   bodyId = kInvalidBodyId;
};

struct BoxCollider
{
    DirectX::XMFLOAT3 halfExtents = {0.5f, 0.5f, 0.5f};
    DirectX::XMFLOAT3 offset      = {0.0f, 0.0f, 0.0f};
};

struct SphereCollider
{
    f32               radius = 0.5f;
    DirectX::XMFLOAT3 offset = {0.0f, 0.0f, 0.0f};
};

struct CapsuleCollider
{
    f32               radius     = 0.5f;
    f32               halfHeight = 1.0f;
    DirectX::XMFLOAT3 offset     = {0.0f, 0.0f, 0.0f};
};

struct ConvexHullCollider
{
    std::vector<DirectX::XMFLOAT3> points; // スケール適用済みワールド頂点
    DirectX::XMFLOAT3 offset = {0.0f, 0.0f, 0.0f};
};

} // namespace dx12e
