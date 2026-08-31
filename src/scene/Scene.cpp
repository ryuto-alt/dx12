#include "scene/Scene.h"

#include <algorithm>
#include <unordered_set>

#include <Windows.h>
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
#include "animation/AnimGraphAsset.h"
#include "animation/AnimGraphRuntime.h"
#include "core/vfs/Vfs.h"
#include "core/PathResolver.h"
#include "terrain/HeightField.h"
#include "terrain/TerrainIO.h"
#include "terrain/TerrainMeshBuilder.h"
#include "terrain/SculptMesh.h"
#include "terrain/SculptIO.h"

#include <cmath>     // BuildSculptVertices の std::fabs
#include <memory>    // std::make_shared / std::make_unique
#include <vector>

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
        Logger::Warn("モデルの読み込みに失敗しました: {}", modelPath);
        OutputDebugStringA(("[Spawn FAILED] " + modelPath + "\n").c_str());
        return Entity();
    }
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "[Spawn OK] %s -> %s (meshes=%zu, mats=%zu)\n",
            name.c_str(), modelPath.c_str(),
            cached->meshes.size(), cached->materials.size());
        OutputDebugStringA(buf);
        for (size_t i = 0; i < cached->meshes.size(); ++i)
        {
            auto& m = cached->meshes[i];
            auto mn = m->GetAABBMin();
            auto mx = m->GetAABBMax();
            snprintf(buf, sizeof(buf), "  mesh[%zu] indices=%u aabb=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n",
                i, m->GetIndexCount(), mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
            OutputDebugStringA(buf);
        }
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

            // レストポーズクリップ（inverseRest 計算用）。★選択規則は ModelLoader が
            // 頂点を焼くときと同一でなければならないので PickRestClip に集約してある。
            const NodeAnimationClip* restClip = PickRestClip(nodeAnim.clips);

            nodeAnim.nodeAnimator->Initialize(nodeAnim.nodeGraph.get(), playClip, restClip);
        }

        // meshNodeTransformsを単位行列で初期化
        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        renderer.meshNodeTransforms.assign(renderer.meshes.size(), identity);
    }

    // Spawn直後に初期ポーズを計算（ノードアニメーション付きモデルのmeshNodeTransforms初期化）
    if (entity.HasComponent<SkeletalAnimation>())
    {
        auto& skelAnim = entity.GetComponent<SkeletalAnimation>();
        if (skelAnim.animator)
            skelAnim.animator->Update(0.0f);
    }
    if (entity.HasComponent<NodeAnimationComp>())
    {
        auto& nodeAnim = entity.GetComponent<NodeAnimationComp>();
        auto& meshRend = entity.GetComponent<MeshRenderer>();
        if (nodeAnim.nodeAnimator)
        {
            nodeAnim.nodeAnimator->Update(0.0f);

            const auto& globalMats = nodeAnim.nodeAnimator->GetNodeGlobalMatrices();
            const NodeGraph* graph = nodeAnim.nodeGraph.get();
            for (u32 ni = 0; ni < graph->GetNodeCount(); ++ni)
            {
                const SceneNode& node = graph->GetNode(ni);
                for (u32 meshIdx : node.meshIndices)
                {
                    if (meshIdx < static_cast<u32>(meshRend.meshNodeTransforms.size()))
                        meshRend.meshNodeTransforms[meshIdx] = globalMats[ni];
                }
            }
        }
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
    renderer.modelPath = "__primitive_plane__";
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
    renderer.modelPath = "__primitive_box__";
    // 発光弾(Pfx*)は共有 unit box を使い instancing 対象に（色は instanceColor へ）。
    if (name.rfind("Pfx", 0) == 0)
    {
        renderer.meshes.push_back(GetSharedGlowMesh(false, 0.0f));
        renderer.instanced = true;
    }
    else
    {
        auto boxMesh = std::make_unique<Mesh>();
        boxMesh->InitializeAsBox(*m_device);
        renderer.meshes.push_back(boxMesh.get());
        m_ownedMeshes.push_back(std::move(boxMesh));
    }

    Logger::Info("Spawned box '{}'", name);
    return entity;
}

Entity Scene::SpawnSphere(const std::string& name,
                          DirectX::XMFLOAT3 position,
                          f32 radius)
{
    Entity entity = CreateEntityWithTransform(name, position, {0, 0, 0}, {1, 1, 1});

    MeshRenderer& renderer = entity.AddComponent<MeshRenderer>();
    renderer.modelPath = "__primitive_sphere__";
    // 発光弾(Pfx*)は同半径で共有メッシュ → instancing 対象（色は instanceColor へ）。
    // scale は {1,1,1} のまま＝半径はメッシュ側に焼く＝game1 の挙動を一切変えない。
    if (name.rfind("Pfx", 0) == 0)
    {
        renderer.meshes.push_back(GetSharedGlowMesh(true, radius));
        renderer.instanced = true;
    }
    else
    {
        auto sphereMesh = std::make_unique<Mesh>();
        sphereMesh->InitializeAsSphere(*m_device, radius);
        renderer.meshes.push_back(sphereMesh.get());
        m_ownedMeshes.push_back(std::move(sphereMesh));
    }

    Logger::Info("Spawned sphere '{}'", name);
    return entity;
}

Entity Scene::SpawnTerrain(const std::string& name,
                           DirectX::XMFLOAT3 position,
                           const Terrain& params,
                           ID3D12GraphicsCommandList* cmd)
{
    Entity entity = CreateEntityWithTransform(name, position, {0, 0, 0}, {1, 1, 1});

    Terrain t = params;
    t.resolution = HeightField::NormalizeResolution(t.resolution);
    if (!(t.worldSize > 1.0f)) t.worldSize = 1.0f;   // NaN も弾く
    if (!(t.maxHeight > 0.0f)) t.maxHeight = 200.0f;
    t._hf = std::make_shared<HeightField>();
    t._hf->Create(t.resolution, t.worldSize, 0.0f);

    if (!t.heightmapPath.empty())
    {
        if (terrain::LoadHeightFieldAsset(t.heightmapPath, *t._hf))
        {
            // .hf 側の解像度/サイズを正とする（保存後にコンポーネントだけ書き換わっても壊れない）
            t.resolution = t._hf->Resolution();
            t.worldSize  = t._hf->WorldSize();
        }
        else
        {
            // 読めなかった場合は平坦のまま続行（シーンは開ける＝作業内容を失わない）
            t._hf->Create(t.resolution, t.worldSize, 0.0f);
        }
    }

    // スプラット重み（レイヤーセットを使うときだけ）。ファイルが無いのは正常（未ペイント）で、
    // その場合は傾斜と標高から自動生成して即座に見た目が出るようにする。
    if (!t.layerSetPath.empty())
    {
        t._splat = std::make_shared<TerrainSplatMap>();
        if (t.splatPath.empty() || !terrain::LoadSplatMapAsset(t.splatPath, *t._splat))
        {
            t._splat->Create(t.splatResolution);
            t._splat->AutoPaintFromHeightField(*t._hf, TerrainAutoPaintParams{});
            t._splatNeedsSave = true;   // TerrainPanel が .splat を書き出す
        }
        t.splatResolution = t._splat->Size();
    }

    MeshRenderer& renderer = entity.AddComponent<MeshRenderer>();
    renderer.modelPath = kTerrainMeshMarker;   // ファイルではない内部マーカー
    {
        std::vector<Vertex> vertices;
        std::vector<u32>    indices;
        TerrainMeshBuilder::Build(*t._hf, t.uvScale, t.color, vertices, indices);

        auto mesh = std::make_unique<Mesh>();
        mesh->InitializeDynamic(*m_device, vertices, indices, cmd ? cmd : m_cmdList);
        renderer.meshes.push_back(mesh.get());
        m_ownedMeshes.push_back(std::move(mesh));
    }

    // 静的コライダー。実際の形状は PhysicsSystem::RegisterBody が Terrain を見て
    // Jolt の HeightFieldShape として作る（高さ配列を共有＝彫れば当たり判定も追従する）。
    RigidBody& rb = entity.AddComponent<RigidBody>();
    rb.motionType  = MotionType::Static;
    rb.friction    = 0.7f;
    rb.restitution = 0.0f;

    entity.AddComponent<Terrain>(std::move(t));

    Logger::Info("Spawned terrain '{}' (resolution={}, worldSize={:.1f})",
                 name, entity.GetComponent<Terrain>().resolution,
                 entity.GetComponent<Terrain>().worldSize);
    return entity;
}

void Scene::RebuildTerrainMesh(entt::entity e, ID3D12GraphicsCommandList* cmd)
{
    if (!m_device || !m_registry.valid(e)) return;
    auto* t  = m_registry.try_get<Terrain>(e);
    auto* mr = m_registry.try_get<MeshRenderer>(e);
    if (!t || !t->_hf || !t->_hf->IsValid() || !mr || mr->meshes.empty() || !mr->meshes[0]) return;

    Mesh* mesh = mr->meshes[0];
    ID3D12GraphicsCommandList* useCmd = cmd ? cmd : m_cmdList;

    const size_t expected = static_cast<size_t>(t->_hf->Resolution())
                          * static_cast<size_t>(t->_hf->Resolution());
    if (mesh->MutableVertices().size() != expected)
    {
        // 解像度が変わった（読み込み直し / 作り直し）→ トポロジごと再構築
        std::vector<Vertex> vertices;
        std::vector<u32>    indices;
        TerrainMeshBuilder::Build(*t->_hf, t->uvScale, t->color, vertices, indices);
        mesh->InitializeDynamic(*m_device, vertices, indices, useCmd);
    }
    else
    {
        HeightField::Rect dirty;
        dirty.x0 = t->_dirtyX0; dirty.z0 = t->_dirtyZ0;
        dirty.x1 = t->_dirtyX1; dirty.z1 = t->_dirtyZ1;
        if (!dirty.Valid()) dirty = t->_hf->FullRect();
        TerrainMeshBuilder::UpdateVertices(*t->_hf, t->uvScale, t->color,
                                           dirty, mesh->MutableVertices());
        mesh->UploadVertexCache(*m_device, useCmd);
    }

    t->_meshDirty = false;
    t->ClearDirtyRect();
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

Mesh* Scene::GetSharedGlowMesh(bool sphere, f32 radius)
{
    // sphere は半径(0.1mm刻み)別、box は単一キー。値の Mesh は m_ownedMeshes が所有。
    uint64_t key = sphere ? (0x5000000000000000ull | static_cast<uint64_t>(radius * 10000.0f + 0.5f))
                          : 0x6000000000000000ull;
    auto it = m_glowMeshCache.find(key);
    if (it != m_glowMeshCache.end()) return it->second;

    auto mesh = std::make_unique<Mesh>();
    if (sphere) mesh->InitializeAsSphere(*m_device, radius);
    else        mesh->InitializeAsBox(*m_device);
    Mesh* ptr = mesh.get();
    m_ownedMeshes.push_back(std::move(mesh));
    m_glowMeshCache[key] = ptr;
    return ptr;
}

void Scene::CollectUnusedMeshes(std::vector<const Mesh*>& outFreed)
{
    if (m_ownedMeshes.empty()) return;

    // 根: 生きている MeshRenderer が指すメッシュ + 共有グローメッシュ。
    // （弾が 1 発も出ていない瞬間でも共有メッシュは残す。次の弾で作り直すと
    //   インスタンシングのキャッシュが毎回無効になるだけで得が無い）
    std::unordered_set<const Mesh*> roots;
    for (auto [e, mr] : m_registry.view<MeshRenderer>().each())
        for (const Mesh* m : mr.meshes)
            if (m) roots.insert(m);
    for (const auto& [key, m] : m_glowMeshCache)
        if (m) roots.insert(m);

    const size_t before = m_ownedMeshes.size();
    auto it = std::remove_if(m_ownedMeshes.begin(), m_ownedMeshes.end(),
        [&](const std::unique_ptr<Mesh>& m)
        {
            if (!m || roots.count(m.get())) return false;
            outFreed.push_back(m.get());   // 破棄前にアドレスを控える（BLAS キャッシュの掃除用）
            return true;
        });
    if (it == m_ownedMeshes.end()) return;

    m_ownedMeshes.erase(it, m_ownedMeshes.end());   // ここで ~Mesh → DeferredRelease
    Logger::Info("未参照メッシュを {} 個回収しました（{} → {}）",
                 before - m_ownedMeshes.size(), before, m_ownedMeshes.size());
}

void Scene::Clear()
{
    m_registry.clear();
    m_glowMeshCache.clear();   // m_ownedMeshes の前にクリア（dangling 回避）
    m_ownedMeshes.clear();
    // ★ここは長らく post と skybox の 2 つしか戻していなかった。
    //   シーンの**読み込み**は ApplySceneJson が全ブロックを既定から入れ直すので無事だが、
    //   「新規シーン」は Clear → Initialize → そのまま Save なので、直前に開いていた
    //   シーンの DXR / DDGI / フォグ / SSR / SSGI / TAA / PCSS / コンタクトシャドウ /
    //   SSAO / デカールアトラス / 影 ON-OFF がそのまま新ファイルへ書き込まれていた。
    //   Scene.h にメンバを足したらここにも足すこと（足し忘れると同じ形で漏れる）。
    m_postSettings   = PostProcessSettings{};
    m_skybox         = SkyboxSettings{};
    m_ssao           = SSAOSettings{};
    m_contactShadow  = ContactShadowSettings{};
    m_normalFilter   = NormalFilterSettings{};
    m_shadowPcss     = ShadowPcssSettings{};
    m_ssr            = SsrSettings{};
    m_ssgi           = SsgiSettings{};
    m_rt             = RtSettings{};
    m_ddgi           = DdgiSettings{};
    m_taa            = TaaSettings{};
    m_volFog         = VolumetricFogSettings{};
    m_decalAtlasPath.clear();
    m_shadowsEnabled = true;
    m_navConfig      = nav::NavBuildConfig{};
    m_navMesh.Clear();   // 前のシーンのナビメッシュを持ち越さない（.nav が無いシーンで残る）
    m_pendingAnimEvents.clear();
}

// ---------------------------------------------------------------------------
// AnimatorController（.animfsm）を進める。Scene::Update の先頭で呼ばれる。
// ---------------------------------------------------------------------------
void Scene::UpdateAnimGraphs(f32 dt)
{
    auto view = m_registry.view<AnimatorController, SkeletalAnimation>();
    if (view.begin() == view.end()) return;

    std::vector<AnimFiredEvent> fired;

    for (auto [entity, ctrl, skelAnim] : view.each())
    {
        if (!skelAnim.animator || !skelAnim.skeleton) continue;

        // --- 遅延ロード（毎フレームの I/O を避ける。パスを変えたら読み直す）---
        if (!ctrl._loaded || ctrl._loadedPath != ctrl.graphPath)
        {
            ctrl._loaded     = true;
            ctrl._failed     = false;
            ctrl._loadedPath = ctrl.graphPath;
            ctrl._state.reset();

            if (!ctrl.graphPath.empty())
            {
                const std::vector<uint8_t> bytes = vfs::ReadAsset(ctrl.graphPath);
                AnimGraphAsset asset;
                std::string err;
                if (bytes.empty())
                {
                    Logger::Warn("AnimGraph: .animfsm を読めない: {}", ctrl.graphPath);
                    ctrl._failed = true;
                }
                else if (!ParseAnimGraphAsset(bytes, asset, err))
                {
                    Logger::Warn("AnimGraph: .animfsm の JSON が不正: {} ({})", ctrl.graphPath, err);
                    ctrl._failed = true;
                }
                else
                {
                    // extraClips: グラフが要求する追加クリップを別ファイルから読む
                    // （Application にあった sneakWalk.gltf のハードコードの置き換え）。
                    for (const AnimGraphExtraClip& ec : asset.extraClips)
                    {
                        const std::string abs = PathResolver::AssetsDir() + ec.path;
                        auto extra = ModelLoader::LoadAnimationsFromFile(abs, *skelAnim.skeleton);
                        if (extra.empty())
                        {
                            Logger::Warn("AnimGraph: extraClips を読めない: {}", ec.path);
                            continue;
                        }
                        for (auto& a : extra)
                        {
                            if (!ec.name.empty()) a->SetName(ec.name);
                            if (anim_graph::FindClipIndex(skelAnim.clips, a->GetName()) >= 0) continue;
                            skelAnim.clips.push_back(std::move(a));
                        }
                    }

                    std::vector<std::string> missing;
                    anim_graph::ResolveAsset(asset, skelAnim.clips, missing);
                    if (!missing.empty())
                    {
                        std::string list;
                        for (const auto& m : missing) { if (!list.empty()) list += ", "; list += m; }
                        Logger::Warn("AnimGraph: 解決できない参照 [{}] ({})", list, ctrl.graphPath);
                    }
                    anim_graph::ApplyClipEvents(asset, skelAnim.clips);

                    ctrl._state = std::make_unique<AnimGraphRuntimeState>();
                    ctrl._state->asset = std::move(asset);
                    anim_graph::InitRuntime(*ctrl._state, *skelAnim.skeleton);

                    if (!ctrl._state->missingMaskBones.empty())
                    {
                        std::string list;
                        for (const auto& m : ctrl._state->missingMaskBones)
                        { if (!list.empty()) list += ", "; list += m; }
                        Logger::Warn("AnimGraph: マスクのボーンが見つからない [{}] ({})",
                                     list, ctrl.graphPath);
                    }
                }
            }
        }

        if (!ctrl._state || !ctrl._state->valid) continue;
        if (!ctrl.playOnStart) continue;

        fired.clear();
        anim_graph::Update(*ctrl._state, skelAnim.clips, *skelAnim.skeleton,
                           *skelAnim.animator, dt * ctrl.speed, fired);

        for (const auto& fe : fired)
        {
            SceneAnimEvent se;
            se.entity      = entity;
            se.name        = ctrl.eventChannel.empty() ? fe.name : (ctrl.eventChannel + fe.name);
            se.stringParam = fe.stringParam;
            se.floatParam  = fe.floatParam;
            se.clip        = fe.clip;
            se.layer       = fe.layer;
            se.time        = fe.time;
            m_pendingAnimEvents.push_back(std::move(se));
        }
    }
}

void Scene::Update(f32 dt)
{
    // アニメーションステートマシン（.animfsm）。
    // ⚠️ 必ず SkeletalAnimation の更新より**前**に回すこと。
    //    ここで Animator::SetPoseOverride を呼び、直後の animator->Update(dt) が
    //    「注入されたフレーム」としてサンプリングをスキップする、という並びになっている。
    UpdateAnimGraphs(dt);

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

std::vector<entt::entity> Scene::QueryByTag(const std::string& tag) const
{
    std::vector<entt::entity> result;
    auto view = m_registry.view<const Tag>();
    for (auto [handle, t] : view.each())
    {
        for (const auto& s : t.tags)
        {
            if (s == tag) { result.push_back(handle); break; }
        }
    }
    return result;
}

std::vector<entt::entity> Scene::QueryInBox(float minX, float minZ, float maxX, float maxZ,
                                            const std::string& tag) const
{
    std::vector<entt::entity> result;
    auto view = m_registry.view<const Transform>();
    for (auto [handle, tf] : view.each())
    {
        const auto& p = tf.position;
        if (p.x < minX || p.x > maxX || p.z < minZ || p.z > maxZ) continue;
        if (!tag.empty())
        {
            if (!m_registry.all_of<Tag>(handle)) continue;
            const auto& t = m_registry.get<Tag>(handle);
            bool found = false;
            for (const auto& s : t.tags)
                if (s == tag) { found = true; break; }
            if (!found) continue;
        }
        result.push_back(handle);
    }
    return result;
}

// ==========================================================================
//  頂点スカルプトメッシュ（洞窟・アーチ・岩などの異形）
// ==========================================================================
namespace
{

// SculptMeshData → 描画用の頂点配列。並びは SculptMeshData と 1:1 に保つこと
//（Mesh::InitializeDynamic は meshoptimizer の並べ替えを通さないので、この並びのまま
//  GPU と m_verticesCache に乗る＝以後 UploadVertexCache で送り直せる）。
void BuildSculptVertices(const SculptMeshData& data, f32 uvScale,
                         const DirectX::XMFLOAT4& color, std::vector<Vertex>& outVertices)
{
    using namespace DirectX;

    const std::vector<XMFLOAT3>& pos = data.Positions();
    const std::vector<XMFLOAT3>& nrm = data.Normals();
    const std::vector<XMFLOAT2>& uvs = data.UVs();

    outVertices.resize(pos.size());
    for (size_t i = 0; i < pos.size(); ++i)
    {
        Vertex& v = outVertices[i];
        v.position = pos[i];
        v.normal   = nrm[i];
        v.color    = color;
        v.texCoord = XMFLOAT2{ uvs[i].x * uvScale, uvs[i].y * uvScale };

        // 接線は法線と直交する適当な軸から作る（法線マップ付きの .dxmat を貼っても破綻しない）。
        const XMVECTOR n  = XMLoadFloat3(&nrm[i]);
        const XMVECTOR up = (std::fabs(nrm[i].y) > 0.99f) ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
                                                          : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMVECTOR t = XMVector3Cross(up, n);
        if (XMVectorGetX(XMVector3LengthSq(t)) < 1e-12f) t = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        t = XMVector3Normalize(t);
        XMFLOAT3 t3;
        XMStoreFloat3(&t3, t);
        v.tangent     = XMFLOAT4{ t3.x, t3.y, t3.z, 1.0f };
        v.boneIndices = XMUINT4{ 0, 0, 0, 0 };
        v.boneWeights = XMFLOAT4{ 0.0f, 0.0f, 0.0f, 0.0f };
    }
}

} // anonymous namespace

Entity Scene::SpawnSculpt(const std::string& name,
                          DirectX::XMFLOAT3 position,
                          const SculptMesh& params,
                          SculptPrimitive fallbackPrimitive,
                          u32 fallbackSubdivisions,
                          f32 fallbackSize,
                          ID3D12GraphicsCommandList* cmd)
{
    Entity entity = CreateEntityWithTransform(name, position, {0, 0, 0}, {1, 1, 1});

    SculptMesh sc = params;
    if (!(sc.uvScale > 0.0f)) sc.uvScale = 1.0f;   // NaN も弾く
    sc._data = std::make_shared<SculptMeshData>();

    bool loaded = false;
    if (!sc.meshPath.empty())
        loaded = sculpt::LoadSculptMeshAsset(sc.meshPath, *sc._data);
    if (!loaded)
    {
        // 読めなかった場合も素体で続行（シーンは開ける＝他の作業内容を失わない）
        sc._data->BuildPrimitive(fallbackPrimitive, fallbackSubdivisions, fallbackSize);
    }

    MeshRenderer& renderer = entity.AddComponent<MeshRenderer>();
    renderer.modelPath = kSculptMeshMarker;   // ファイルではない内部マーカー
    if (m_device)
    {
        std::vector<Vertex> vertices;
        BuildSculptVertices(*sc._data, sc.uvScale, sc.color, vertices);

        auto mesh = std::make_unique<Mesh>();
        mesh->InitializeDynamic(*m_device, vertices, sc._data->Indices(), cmd ? cmd : m_cmdList);
        renderer.meshes.push_back(mesh.get());
        m_ownedMeshes.push_back(std::move(mesh));
    }

    // 静的コライダー。実際の形状は PhysicsSystem::RegisterBody が SculptMesh を見て
    // Jolt の MeshShape として作る（頂点配列を共有＝彫れば当たり判定も追従する）。
    RigidBody& rb = entity.AddComponent<RigidBody>();
    rb.motionType  = MotionType::Static;
    rb.friction    = 0.7f;
    rb.restitution = 0.0f;

    const size_t vcount = sc._data->VertexCount();
    const size_t tcount = sc._data->TriangleCount();
    entity.AddComponent<SculptMesh>(std::move(sc));

    Logger::Info("Spawned sculpt mesh '{}' ({} verts / {} tris)", name, vcount, tcount);
    return entity;
}

void Scene::RebuildSculptMesh(entt::entity e, ID3D12GraphicsCommandList* cmd)
{
    if (!m_device || !m_registry.valid(e)) return;
    auto* sc = m_registry.try_get<SculptMesh>(e);
    auto* mr = m_registry.try_get<MeshRenderer>(e);
    if (!sc || !sc->_data || !sc->_data->IsValid()) return;
    if (!mr || mr->meshes.empty() || !mr->meshes[0]) return;

    Mesh* mesh = mr->meshes[0];
    ID3D12GraphicsCommandList* useCmd = cmd ? cmd : m_cmdList;

    if (mesh->MutableVertices().size() != sc->_data->VertexCount())
    {
        // 素体を作り直した（＝トポロジごと変わった）→ 丸ごと再初期化
        std::vector<Vertex> vertices;
        BuildSculptVertices(*sc->_data, sc->uvScale, sc->color, vertices);
        mesh->InitializeDynamic(*m_device, vertices, sc->_data->Indices(), useCmd);
    }
    else
    {
        // トポロジは変わらない（ブラシは位置しか動かさない）ので頂点だけ送り直す。
        // 頂点配列の作り直し自体は全頂点ぶん走るが、UploadVertexCache が
        // どのみち VB 全体を上げ直すので、部分更新にしても得は小さい。
        BuildSculptVertices(*sc->_data, sc->uvScale, sc->color, mesh->MutableVertices());
        mesh->UploadVertexCache(*m_device, useCmd);
    }

    sc->_meshDirty = false;
}

} // namespace dx12e
