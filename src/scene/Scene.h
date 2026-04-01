#pragma once

#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <entt/entt.hpp>
#include <DirectXMath.h>
#include "core/Types.h"
#include "ecs/Components.h"
#include "scene/Entity.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Mesh;
class ResourceManager;
class GraphicsDevice;
class DescriptorHeap;

class Scene
{
public:
    void Initialize(ResourceManager* resourceManager,
                    GraphicsDevice* device,
                    DescriptorHeap* srvHeap,
                    ID3D12GraphicsCommandList* cmdList);

    Entity Spawn(const std::string& name,
                 const std::string& modelPath,
                 DirectX::XMFLOAT3 position,
                 DirectX::XMFLOAT3 rotation = {0, 0, 0},
                 DirectX::XMFLOAT3 scale = {1, 1, 1});

    Entity SpawnPlane(const std::string& name,
                      DirectX::XMFLOAT3 position,
                      f32 size = 50.0f,
                      bool gridShader = false);

    Entity SpawnBox(const std::string& name,
                    DirectX::XMFLOAT3 position,
                    DirectX::XMFLOAT3 rotation = {0, 0, 0},
                    DirectX::XMFLOAT3 scale = {1, 1, 1});

    Entity SpawnSphere(const std::string& name,
                       DirectX::XMFLOAT3 position,
                       f32 radius = 0.5f);

    void Remove(Entity entity);
    void Clear();
    void Update(f32 dt);

    template<typename Fn>
    void ForEachEntity(Fn&& fn) const
    {
        auto view = m_registry.view<const NameTag, const Transform>();
        for (auto [entity, name, transform] : view.each())
        {
            (void)name;
            (void)transform;
            fn(m_registry, entity);
        }
    }

    Entity FindEntity(const std::string& name);
    size_t GetEntityCount() const;

    entt::registry&       GetRegistry()       { return m_registry; }
    const entt::registry& GetRegistry() const { return m_registry; }

private:
    Entity CreateEntityWithTransform(const std::string& name,
                                     DirectX::XMFLOAT3 position,
                                     DirectX::XMFLOAT3 rotation,
                                     DirectX::XMFLOAT3 scale);

    entt::registry m_registry;
    std::vector<std::unique_ptr<Mesh>> m_ownedMeshes;

    ResourceManager*  m_resourceManager = nullptr;
    GraphicsDevice*   m_device          = nullptr;
    DescriptorHeap*   m_srvHeap         = nullptr;
    ID3D12GraphicsCommandList* m_cmdList = nullptr;
};

} // namespace dx12e
