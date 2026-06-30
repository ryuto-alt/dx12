#pragma once

#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <entt/entt.hpp>
#include <DirectXMath.h>
#include "core/Types.h"
#include "ecs/Components.h"
#include "scene/Entity.h"
#include "renderer/PostProcessSettings.h"
#include "renderer/SSAOSettings.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Mesh;
class ResourceManager;
class GraphicsDevice;
class DescriptorHeap;

// シーン単位のスカイボックス / IBL 設定（per-entity component ではなく scene-level）。
struct SkyboxSettings
{
    std::string envMapPath;            // assets 相対 .dds（空=IBL/skybox 無効）
    float       iblIntensity   = 1.0f;
    float       skyboxIntensity = 1.0f;
    bool        drawSkybox     = true; // false=IBL のみ（背景は塗らない）
};

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

    // 指定タグを持つ全エンティティのハンドルを返す（filter汎用化・RTS群管理・対象クエリ）。
    std::vector<entt::entity> QueryByTag(const std::string& tag) const;

    // XZ矩形 [minX,maxX]×[minZ,maxZ] 内のエンティティを返す。tag 非空ならそのタグ持ちに絞る。
    // RTS の矩形選択・範囲判定をゲーム側からエンジン無改造で使うための汎用クエリ。
    std::vector<entt::entity> QueryInBox(float minX, float minZ, float maxX, float maxZ,
                                         const std::string& tag = "") const;

    GraphicsDevice* GetDevice() const { return m_device; }
    entt::registry&       GetRegistry()       { return m_registry; }
    const entt::registry& GetRegistry() const { return m_registry; }

    PostProcessSettings&       GetPostSettings()       { return m_postSettings; }
    const PostProcessSettings& GetPostSettings() const { return m_postSettings; }

    SkyboxSettings&       GetSkyboxSettings()       { return m_skybox; }
    const SkyboxSettings& GetSkyboxSettings() const { return m_skybox; }

    SSAOSettings&       GetSSAOSettings()       { return m_ssao; }
    const SSAOSettings& GetSSAOSettings() const { return m_ssao; }

private:
    Entity CreateEntityWithTransform(const std::string& name,
                                     DirectX::XMFLOAT3 position,
                                     DirectX::XMFLOAT3 rotation,
                                     DirectX::XMFLOAT3 scale);

    // 発光弾(Pfx)など「同一形状を大量に出すプリミティブ」をサイズ別に共有して
    // インスタンシング可能にするキャッシュ。値は m_ownedMeshes が所有する Mesh*。
    Mesh* GetSharedGlowMesh(bool sphere, f32 radius);

    entt::registry m_registry;
    std::vector<std::unique_ptr<Mesh>> m_ownedMeshes;
    std::unordered_map<uint64_t, Mesh*> m_glowMeshCache;
    PostProcessSettings m_postSettings;
    SkyboxSettings      m_skybox;
    SSAOSettings        m_ssao;

    ResourceManager*  m_resourceManager = nullptr;
    GraphicsDevice*   m_device          = nullptr;
    DescriptorHeap*   m_srvHeap         = nullptr;
    ID3D12GraphicsCommandList* m_cmdList = nullptr;
};

} // namespace dx12e
