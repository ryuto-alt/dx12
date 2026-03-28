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

struct Entity
{
    std::string name;
    Transform transform;

    // 描画データ（ResourceManager/CachedModel所有、借用ポインタ）
    std::vector<Mesh*> meshes;
    std::vector<Material*> materials;
    bool hasSkeleton = false;
    bool useGridShader = false;  // グリッド床用フラグ

    // アニメーション（Entity固有、スケルタルメッシュのみ）
    std::unique_ptr<Skeleton> skeleton;
    std::vector<std::unique_ptr<AnimationClip>> animClips;
    std::unique_ptr<Animator> animator;
    std::unique_ptr<SkinningBuffer> skinningBuffer;

    // unique_ptr メンバのため .cpp でデストラクタ定義
    Entity();
    ~Entity();
    Entity(Entity&&) noexcept;
    Entity& operator=(Entity&&) noexcept;
};

} // namespace dx12e
