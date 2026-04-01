#pragma once

#include <string>
#include <vector>
#include <memory>
#include "scene/Transform.h"

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

struct Entity
{
    std::string name;
    Transform transform;

    // 描画データ（ResourceManager/CachedModel所有、借用ポインタ）
    std::vector<Mesh*> meshes;
    std::vector<Material*> materials;
    bool hasSkeleton = false;
    bool hasNodeAnimation = false;
    bool useGridShader = false;  // グリッド床用フラグ

    // スケルタルアニメーション（Entity固有）
    std::unique_ptr<Skeleton> skeleton;
    std::vector<std::unique_ptr<AnimationClip>> animClips;
    std::unique_ptr<Animator> animator;
    std::unique_ptr<SkinningBuffer> skinningBuffer;

    // ノードアニメーション（Entity固有、Kenney系モデル等）
    std::unique_ptr<NodeGraph> nodeGraph;
    std::vector<std::unique_ptr<NodeAnimationClip>> nodeAnimClips;
    std::unique_ptr<NodeAnimator> nodeAnimator;
    std::vector<DirectX::XMFLOAT4X4> meshNodeTransforms;  // メッシュごとのワールド行列

    // unique_ptr メンバのため .cpp でデストラクタ定義
    Entity();
    ~Entity();
    Entity(Entity&&) noexcept;
    Entity& operator=(Entity&&) noexcept;
};

} // namespace dx12e
