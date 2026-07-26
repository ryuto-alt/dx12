#pragma once

#include <vector>
#include <memory>
#include <filesystem>

#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class GraphicsDevice;
class ResourceManager;

struct ModelData
{
    std::vector<std::unique_ptr<Mesh>>     meshes;
    std::vector<std::unique_ptr<Material>> materials;
    std::unique_ptr<Skeleton>              skeleton;   // null = static mesh
    std::vector<std::unique_ptr<AnimationClip>> animClips;  // empty = no animation

    // Node animation（skeleton が null でアニメーションがある場合）
    std::unique_ptr<NodeGraph>                         nodeGraph;
    std::vector<std::unique_ptr<NodeAnimationClip>>    nodeAnimClips;
};

// MCP asset_info 用: GPU を使わずモデルのメタ情報だけ読む(Assimp のみ)。
struct ModelProbeInfo
{
    bool        ok = false;
    std::string error;
    uint32_t meshCount = 0, materialCount = 0, boneCount = 0;
    uint64_t totalVertices = 0, totalFaces = 0;
    bool     hasSkeleton = false;
    struct Anim { std::string name; double durationSec = 0.0; };
    std::vector<Anim> animations;
    // ノード変換込みのワールド空間 AABB(= スケール1で置いた時の実サイズ)
    float aabbMin[3] = {0, 0, 0}, aabbMax[3] = {0, 0, 0};
};

class ModelLoader
{
public:
    static ModelProbeInfo Probe(const std::filesystem::path& filePath);

    static ModelData LoadFromFile(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const std::filesystem::path& filePath,
        ResourceManager& resourceManager);

    static std::vector<std::unique_ptr<AnimationClip>> LoadAnimationsFromFile(
        const std::filesystem::path& filePath,
        const Skeleton& skeleton);
};

} // namespace dx12e
