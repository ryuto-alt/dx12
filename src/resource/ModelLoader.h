#pragma once

#include <vector>
#include <memory>
#include <filesystem>

#include "renderer/Mesh.h"
#include "renderer/Material.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class GraphicsDevice;
class ResourceManager;

struct MeshData
{
    std::unique_ptr<Mesh>     mesh;
    std::unique_ptr<Material> material;
};

class ModelLoader
{
public:
    static std::vector<MeshData> LoadFromFile(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const std::filesystem::path& filePath,
        ResourceManager& resourceManager);
};

} // namespace dx12e
