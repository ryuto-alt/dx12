#pragma once

#include <vector>
#include <memory>
#include <filesystem>

#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"

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
    std::unique_ptr<AnimationClip>         animClip;   // null = no animation
};

class ModelLoader
{
public:
    static ModelData LoadFromFile(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const std::filesystem::path& filePath,
        ResourceManager& resourceManager);
};

} // namespace dx12e
