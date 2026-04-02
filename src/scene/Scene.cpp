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
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"

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

Entity Scene::CreateEntityWithTransform(const std::string& name,
                                        DirectX::XMFLOAT3 position,
                                        DirectX::XMFLOAT3 rotation,
                                        DirectX::XMFLOAT3 scale)
{
    auto handle = m_registry.create();
    Entity entity(handle, &m_registry);

    entity.AddComponent<NameTag>(NameTag{name});

    Transform t;
    t.position = position;
    t.rotation = rotation;
    t.scale    = scale;
    entity.AddComponent<Transform>(t);

    return entity;
}

Entity Scene::Spawn(const std::string& name,
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
        return Entity();
    }

    Entity entity = CreateEntityWithTransform(name, position, rotation, scale);

    // MeshRenderer コンポーネント
    MeshRenderer& renderer = entity.AddComponent<MeshRenderer>();
    renderer.modelPath = modelPath;
    for (const auto& mesh : cached->meshes)
    {
        renderer.meshes.push_back(mesh.get());
    }
    for (const auto& mat : cached->materials)
    {
        renderer.materials.push_back(mat.get());
    }

    // スケルタルメッシュの場合
    if (cached->skeleton)
    {
        SkeletalAnimation& skelAnim = entity.AddComponent<SkeletalAnimation>();

        // Skeletonをコピー
        skelAnim.skeleton = std::make_unique<Skeleton>(*cached->skeleton);

        // AnimationClipをコピー
        for (const auto& clip : cached->animClips)
        {
            skelAnim.clips.push_back(std::make_unique<AnimationClip>(*clip));
        }

        // Animator作成
        skelAnim.animator = std::make_unique<Animator>();
        if (!skelAnim.clips.empty())
        {
            skelAnim.animator->Initialize(skelAnim.skeleton.get(),
                                          skelAnim.clips[0].get());
        }

        // SkinningBuffer作成
        skelAnim.skinningBuffer = std::make_unique<SkinningBuffer>();
        skelAnim.skinningBuffer->Initialize(*m_device, *m_srvHeap,
                                            Skeleton::kMaxBones,
                                            FrameResources::kFrameCount);
    }
    // ノードアニメーションの場合（ボーンなし＋アニメあり）
    else if (cached->nodeGraph && !cached->nodeAnimClips.empty())
    {
        NodeAnimationComp& nodeAnim = entity.AddComponent<NodeAnimationComp>();

        // NodeGraphをコピー
        nodeAnim.nodeGraph = std::make_unique<NodeGraph>(*cached->nodeGraph);

        // NodeAnimationClipをコピー
        for (const auto& clip : cached->nodeAnimClips)
        {
            nodeAnim.clips.push_back(std::make_unique<NodeAnimationClip>(*clip));
        }

        // NodeAnimator作成
        nodeAnim.nodeAnimator = std::make_unique<NodeAnimator>();
        {
            // 再生クリップ: idle > walk > 先頭
            const NodeAnimationClip* playClip = nodeAnim.clips[0].get();
            for (const auto& clip : nodeAnim.clips)
            {
                if (clip->GetName() == "idle" || clip->GetName() == "walk")
                {
                    playClip = clip.get();
                    break;
                }
            }

            // レストポーズクリップ: static > 先頭（inverseRest計算用）
            const NodeAnimationClip* restClip = nodeAnim.clips[0].get();
            for (const auto& clip : nodeAnim.clips)
            {
                if (clip->GetName() == "static")
                {
                    restClip = clip.get();
                    break;
                }
            }

            nodeAnim.nodeAnimator->Initialize(nodeAnim.nodeGraph.get(), playClip, restClip);
        }

        // meshNodeTransformsを単位行列で初期化
        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        renderer.meshNodeTransforms.assign(renderer.meshes.size(), identity);
    }

    Logger::Info("Spawned entity '{}' at ({:.1f}, {:.1f}, {:.1f})",
                 name, position.x, position.y, position.z);
    return entity;
}

Entity Scene::SpawnPlane(const std::string& name,
                         DirectX::XMFLOAT3 position,
                         f32 size,
                         bool gridShader)
{
    Entity entity = CreateEntityWithTransform(name, position, {0, 0, 0}, {1, 1, 1});

    // MeshRenderer
    MeshRenderer& renderer = entity.AddComponent<MeshRenderer>();
    auto planeMesh = std::make_unique<Mesh>();
    planeMesh->InitializeAsPlane(*m_device, size);
    renderer.meshes.push_back(planeMesh.get());
    m_ownedMeshes.push_back(std::move(planeMesh));

    if (gridShader)
    {
        entity.AddComponent<GridPlane>();
    }

    Logger::Info("Spawned plane '{}' (size={:.0f})", name, size);
    return entity;
}

Entity Scene::SpawnBox(const std::string& name,
                       DirectX::XMFLOAT3 position,
                       DirectX::XMFLOAT3 rotation,
                       DirectX::XMFLOAT3 scale)
{
    Entity entity = CreateEntityWithTransform(name, position, rotation, scale);

    MeshRenderer& renderer = entity.AddComponent<MeshRenderer>();
    auto boxMesh = std::make_unique<Mesh>();
    boxMesh->InitializeAsBox(*m_device);
    renderer.meshes.push_back(boxMesh.get());
    m_ownedMeshes.push_back(std::move(boxMesh));

    Logger::Info("Spawned box '{}'", name);
    return entity;
}

Entity Scene::SpawnSphere(const std::string& name,
                          DirectX::XMFLOAT3 position,
                          f32 radius)
{
    Entity entity = CreateEntityWithTransform(name, position, {0, 0, 0}, {1, 1, 1});

    MeshRenderer& renderer = entity.AddComponent<MeshRenderer>();
    auto sphereMesh = std::make_unique<Mesh>();
    sphereMesh->InitializeAsSphere(*m_device, radius);
    renderer.meshes.push_back(sphereMesh.get());
    m_ownedMeshes.push_back(std::move(sphereMesh));

    Logger::Info("Spawned sphere '{}'", name);
    return entity;
}

void Scene::Remove(Entity entity)
{
    if (entity.IsValid())
    {
        if (entity.HasComponent<NameTag>())
        {
            Logger::Info("Removed entity '{}'", entity.GetComponent<NameTag>().name);
        }
        m_registry.destroy(entity.GetHandle());
    }
}

void Scene::Clear()
{
    m_registry.clear();
    m_ownedMeshes.clear();
}

void Scene::Update(f32 dt)
{
    // スケルタルアニメーション更新
    {
        auto view = m_registry.view<SkeletalAnimation>();
        for (auto [entity, skelAnim] : view.each())
        {
            if (skelAnim.animator)
            {
                skelAnim.animator->Update(dt);
            }
        }
    }

    // ノードアニメーション更新
    {
        auto view = m_registry.view<NodeAnimationComp, MeshRenderer>();
        for (auto [entity, nodeAnim, renderer] : view.each())
        {
            if (nodeAnim.nodeAnimator)
            {
                nodeAnim.nodeAnimator->Update(dt);

                // NodeAnimatorの結果をmeshNodeTransformsにマッピング
                const auto& globalMats = nodeAnim.nodeAnimator->GetNodeGlobalMatrices();
                const NodeGraph* graph = nodeAnim.nodeGraph.get();

                for (u32 ni = 0; ni < graph->GetNodeCount(); ++ni)
                {
                    const SceneNode& node = graph->GetNode(ni);
                    for (u32 meshIdx : node.meshIndices)
                    {
                        if (meshIdx < static_cast<u32>(renderer.meshNodeTransforms.size()))
                        {
                            renderer.meshNodeTransforms[meshIdx] = globalMats[ni];
                        }
                    }
                }
            }
        }
    }
}

Entity Scene::FindEntity(const std::string& name)
{
    auto view = m_registry.view<const NameTag>();
    for (auto [handle, tag] : view.each())
    {
        if (tag.name == name)
        {
            return Entity(handle, &m_registry);
        }
    }
    return Entity();
}

size_t Scene::GetEntityCount() const
{
    auto view = m_registry.view<const NameTag>();
    size_t count = 0;
    for (auto entity : view)
    {
        (void)entity;
        ++count;
    }
    return count;
}

} // namespace dx12e
