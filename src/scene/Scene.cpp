#include "scene/Scene.h"

#include "core/Logger.h"
#include "resource/ResourceManager.h"
#include "resource/ModelLoader.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/FrameResources.h"
#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/SkinningBuffer.h"

namespace dx12e
{

void Scene::Initialize(ResourceManager* resourceManager,
                       GraphicsDevice* device,
                       DescriptorHeap* srvHeap,
                       ID3D12GraphicsCommandList* cmdList)
{
    m_resourceManager = resourceManager;
    m_device          = device;
    m_srvHeap         = srvHeap;
    m_cmdList         = cmdList;
}

Entity* Scene::Spawn(const std::string& name,
                     const std::string& modelPath,
                     DirectX::XMFLOAT3 position,
                     DirectX::XMFLOAT3 rotation,
                     DirectX::XMFLOAT3 scale)
{
    // モデル読み込み（キャッシュ付き）
    const CachedModel* cached = m_resourceManager->GetOrLoadModel(modelPath, m_cmdList);
    if (!cached)
    {
        Logger::Warn("Failed to load model: {}", modelPath);
        return nullptr;
    }

    auto entity = std::make_unique<Entity>();
    entity->name = name;
    entity->transform.position = position;
    entity->transform.rotation = rotation;
    entity->transform.scale    = scale;

    // メッシュ/マテリアル参照（CachedModel所有、借用）
    for (const auto& mesh : cached->meshes)
    {
        entity->meshes.push_back(mesh.get());
    }
    for (const auto& mat : cached->materials)
    {
        entity->materials.push_back(mat.get());
    }

    // スケルタルメッシュの場合: Skeleton/AnimClip/Animator/SkinningBufferをEntity固有で作成
    if (cached->skeleton)
    {
        entity->hasSkeleton = true;

        // Skeletonをコピー
        entity->skeleton = std::make_unique<Skeleton>(*cached->skeleton);

        // AnimationClipをコピー
        for (const auto& clip : cached->animClips)
        {
            entity->animClips.push_back(std::make_unique<AnimationClip>(*clip));
        }

        // Animator作成
        entity->animator = std::make_unique<Animator>();
        if (!entity->animClips.empty())
        {
            entity->animator->Initialize(entity->skeleton.get(),
                                         entity->animClips[0].get());
        }

        // SkinningBuffer作成
        entity->skinningBuffer = std::make_unique<SkinningBuffer>();
        entity->skinningBuffer->Initialize(*m_device, *m_srvHeap,
                                           Skeleton::kMaxBones,
                                           FrameResources::kFrameCount);
    }

    Entity* rawPtr = entity.get();
    m_entities.push_back(std::move(entity));

    Logger::Info("Spawned entity '{}' at ({:.1f}, {:.1f}, {:.1f})",
                 name, position.x, position.y, position.z);
    return rawPtr;
}

Entity* Scene::SpawnPlane(const std::string& name,
                          DirectX::XMFLOAT3 position,
                          f32 size,
                          bool gridShader)
{
    auto entity = std::make_unique<Entity>();
    entity->name = name;
    entity->transform.position = position;
    entity->useGridShader = gridShader;

    // プリミティブメッシュはEntity自身が所有する（m_ownedMeshesに保持）
    auto planeMesh = std::make_unique<Mesh>();
    planeMesh->InitializeAsPlane(*m_device, size);
    entity->meshes.push_back(planeMesh.get());
    m_ownedMeshes.push_back(std::move(planeMesh));

    Entity* rawPtr = entity.get();
    m_entities.push_back(std::move(entity));

    Logger::Info("Spawned plane '{}' (size={:.0f})", name, size);
    return rawPtr;
}

Entity* Scene::SpawnBox(const std::string& name,
                        DirectX::XMFLOAT3 position,
                        DirectX::XMFLOAT3 rotation,
                        DirectX::XMFLOAT3 scale)
{
    auto entity = std::make_unique<Entity>();
    entity->name = name;
    entity->transform.position = position;
    entity->transform.rotation = rotation;
    entity->transform.scale = scale;

    auto boxMesh = std::make_unique<Mesh>();
    boxMesh->InitializeAsBox(*m_device);
    entity->meshes.push_back(boxMesh.get());
    m_ownedMeshes.push_back(std::move(boxMesh));

    Entity* rawPtr = entity.get();
    m_entities.push_back(std::move(entity));

    Logger::Info("Spawned box '{}'", name);
    return rawPtr;
}

Entity* Scene::SpawnSphere(const std::string& name,
                           DirectX::XMFLOAT3 position,
                           f32 radius)
{
    auto entity = std::make_unique<Entity>();
    entity->name = name;
    entity->transform.position = position;

    auto sphereMesh = std::make_unique<Mesh>();
    sphereMesh->InitializeAsSphere(*m_device, radius);
    entity->meshes.push_back(sphereMesh.get());
    m_ownedMeshes.push_back(std::move(sphereMesh));

    Entity* rawPtr = entity.get();
    m_entities.push_back(std::move(entity));

    Logger::Info("Spawned sphere '{}'", name);
    return rawPtr;
}

void Scene::Remove(Entity* entity)
{
    auto it = std::find_if(m_entities.begin(), m_entities.end(),
        [entity](const std::unique_ptr<Entity>& e) { return e.get() == entity; });

    if (it != m_entities.end())
    {
        Logger::Info("Removed entity '{}'", (*it)->name);
        m_entities.erase(it);
    }
}

void Scene::Clear()
{
    m_entities.clear();
    m_ownedMeshes.clear();
}

void Scene::Update(f32 dt)
{
    for (auto& entity : m_entities)
    {
        if (entity->animator)
        {
            entity->animator->Update(dt);
        }
    }
}

} // namespace dx12e
