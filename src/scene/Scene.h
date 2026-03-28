#pragma once

#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <DirectXMath.h>
#include "core/Types.h"
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

    // Entity生成（モデル読み込み + シーンに追加）
    Entity* Spawn(const std::string& name,
                  const std::string& modelPath,
                  DirectX::XMFLOAT3 position,
                  DirectX::XMFLOAT3 rotation = {0, 0, 0},
                  DirectX::XMFLOAT3 scale = {1, 1, 1});

    // プリミティブEntity生成
    Entity* SpawnPlane(const std::string& name,
                       DirectX::XMFLOAT3 position,
                       f32 size = 50.0f,
                       bool gridShader = false);

    Entity* SpawnBox(const std::string& name,
                     DirectX::XMFLOAT3 position,
                     DirectX::XMFLOAT3 rotation = {0, 0, 0},
                     DirectX::XMFLOAT3 scale = {1, 1, 1});

    Entity* SpawnSphere(const std::string& name,
                        DirectX::XMFLOAT3 position,
                        f32 radius = 0.5f);

    void Remove(Entity* entity);
    void Clear();

    // 全EntityのAnimator更新
    void Update(f32 dt);

    // 描画用イテレーション
    template<typename Fn>
    void ForEachEntity(Fn&& fn) const
    {
        for (const auto& entity : m_entities)
        {
            fn(*entity);
        }
    }

    size_t GetEntityCount() const { return m_entities.size(); }
    const std::vector<std::unique_ptr<Entity>>& GetEntities() const { return m_entities; }

private:
    std::vector<std::unique_ptr<Entity>> m_entities;
    std::vector<std::unique_ptr<Mesh>>  m_ownedMeshes;  // プリミティブメッシュの所有権
    ResourceManager*  m_resourceManager = nullptr;
    GraphicsDevice*   m_device          = nullptr;
    DescriptorHeap*   m_srvHeap         = nullptr;
    ID3D12GraphicsCommandList* m_cmdList = nullptr;
};

} // namespace dx12e
