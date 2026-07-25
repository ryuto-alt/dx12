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
#include "renderer/ContactShadowSettings.h"
#include "renderer/ShadowPcssSettings.h"
#include "renderer/SsrSettings.h"
#include "renderer/SsgiSettings.h"
#include "renderer/TaaSettings.h"
#include "renderer/VolumetricFogSettings.h"
// SpawnSculpt の既定引数で SculptPrimitive の列挙子を使うため（前方宣言では足りない）。
#include "terrain/SculptMesh.h"

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

// .animfsm のクリップイベント（足音等）が発火したときの通知。
// Scene::Update が積み、Application が EventBus へ流してから Clear する
// （Scene は EventBus を知らないので、UI アニメと同じく「積んで渡す」形にしている）。
struct SceneAnimEvent
{
    entt::entity entity = entt::null;
    std::string  name;          // EventBus のイベント名（eventChannel が付いた後の最終名）
    std::string  stringParam;
    f32          floatParam = 0.0f;
    std::string  clip;
    i32          layer = 0;
    f32          time = 0.0f;
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

    // --- ハイトフィールド地形 ---
    // params の resolution / worldSize / heightmapPath 等を元に HeightField を用意し、
    // CPU 生成メッシュを載せた「普通のエンティティ」を作る（+ 静的コライダー用の RigidBody）。
    // heightmapPath が空でなければ vfs 経由で .hf を読む（読めなければ平坦のまま警告）。
    // cmd = 記録中のコマンドリスト（null なら Initialize 時のものを使う。シーンロード中は
    // そのままで良いが、フレーム途中で呼ぶときは必ずその時点の cmdList を渡すこと）。
    Entity SpawnTerrain(const std::string& name,
                        DirectX::XMFLOAT3 position,
                        const Terrain& params,
                        ID3D12GraphicsCommandList* cmd = nullptr);

    // 高さ配列の変更をメッシュへ反映する（Terrain::_dirtyX0.. の矩形だけ差し替える）。
    // 頂点数が合わない（解像度が変わった）ときは丸ごと作り直す。
    // 呼んだあと Terrain::_meshDirty は落ちる（_colliderDirty は物理側が消費する）。
    void RebuildTerrainMesh(entt::entity e, ID3D12GraphicsCommandList* cmd = nullptr);

    // --- 頂点スカルプトメッシュ（洞窟・アーチ・岩などの異形）---
    // params.meshPath が空でなければ vfs 経由で .smsh を読む。空（または読めない）なら
    // fallbackPrimitive / fallbackSubdivisions / fallbackSize から素体を作る。
    // 地形と同じく「CPU 生成メッシュを載せた普通のエンティティ」+ 静的コライダー用 RigidBody。
    Entity SpawnSculpt(const std::string& name,
                       DirectX::XMFLOAT3 position,
                       const SculptMesh& params,
                       SculptPrimitive fallbackPrimitive = SculptPrimitive::Sphere,
                       u32 fallbackSubdivisions = 16,
                       f32 fallbackSize = 2.0f,
                       ID3D12GraphicsCommandList* cmd = nullptr);

    // 頂点の変更を GPU のメッシュへ反映する（トポロジは変わらないので頂点だけ送り直す）。
    // 頂点数が合わない（素体を作り直した）ときは丸ごと再初期化する。
    // 呼んだあと SculptMesh::_meshDirty は落ちる（_colliderDirty は物理側が消費する）。
    void RebuildSculptMesh(entt::entity e, ID3D12GraphicsCommandList* cmd = nullptr);

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

    ContactShadowSettings&       GetContactShadowSettings()       { return m_contactShadow; }
    const ContactShadowSettings& GetContactShadowSettings() const { return m_contactShadow; }

    // PCSS（ソフトシャドウ）。既定 OFF ＝ 従来の 3x3 PCF とビット一致。
    ShadowPcssSettings&       GetShadowPcssSettings()       { return m_shadowPcss; }
    const ShadowPcssSettings& GetShadowPcssSettings() const { return m_shadowPcss; }

    // スクリーン空間反射 / スクリーン空間GI（どちらも既定 OFF。深度プリパスの G-Buffer を使う）
    SsrSettings&        GetSsrSettings()        { return m_ssr; }
    const SsrSettings&  GetSsrSettings() const  { return m_ssr; }
    SsgiSettings&       GetSsgiSettings()       { return m_ssgi; }
    const SsgiSettings& GetSsgiSettings() const { return m_ssgi; }

    TaaSettings&       GetTaaSettings()       { return m_taa; }
    const TaaSettings& GetTaaSettings() const { return m_taa; }

    // froxel ボリュメトリックフォグ（既定 OFF。有効時のみ 3D テクスチャ 28MB を確保する）
    VolumetricFogSettings&       GetVolumetricFogSettings()       { return m_volFog; }
    const VolumetricFogSettings& GetVolumetricFogSettings() const { return m_volFog; }

    // デカールアトラス（assets 相対）。DecalComponent の atlasUV はこの 1 枚の中の矩形を指す。
    // 空なら 1x1 の透明ダミーが貼られる＝デカールは描かれない。
    const std::string& GetDecalAtlasPath() const { return m_decalAtlasPath; }
    void SetDecalAtlasPath(const std::string& p) { m_decalAtlasPath = p; }

    // リアルタイム影(CSM)をこのシーンで描くか。false なら影パスを丸ごとスキップ
    // （トップダウン等で影が要らないシーンの FPS 向上用。シェーダは無影センチネルで全面ライト）。
    bool GetShadowsEnabled() const { return m_shadowsEnabled; }
    void SetShadowsEnabled(bool v) { m_shadowsEnabled = v; }

    // 今フレームの Update で発火したアニメイベント。Application が EventBus へ流したら
    // clear すること（Scene 自身は毎フレーム先頭で clear しない＝取りこぼしを見えるようにする）。
    std::vector<SceneAnimEvent>&       GetPendingAnimEvents()       { return m_pendingAnimEvents; }
    const std::vector<SceneAnimEvent>& GetPendingAnimEvents() const { return m_pendingAnimEvents; }

private:
    Entity CreateEntityWithTransform(const std::string& name,
                                     DirectX::XMFLOAT3 position,
                                     DirectX::XMFLOAT3 rotation,
                                     DirectX::XMFLOAT3 scale);

    // AnimatorController を持つエンティティの .animfsm を進める（Update の先頭で呼ぶ）。
    // 未ロードならここで vfs からアセットを読み、クリップ名の解決とイベントの流し込みを行う。
    void UpdateAnimGraphs(f32 dt);

    // 発光弾(Pfx)など「同一形状を大量に出すプリミティブ」をサイズ別に共有して
    // インスタンシング可能にするキャッシュ。値は m_ownedMeshes が所有する Mesh*。
    Mesh* GetSharedGlowMesh(bool sphere, f32 radius);

    entt::registry m_registry;
    std::vector<std::unique_ptr<Mesh>> m_ownedMeshes;
    std::unordered_map<uint64_t, Mesh*> m_glowMeshCache;
    PostProcessSettings m_postSettings;
    SkyboxSettings      m_skybox;
    SSAOSettings        m_ssao;
    ContactShadowSettings m_contactShadow;
    ShadowPcssSettings    m_shadowPcss;
    SsrSettings         m_ssr;
    SsgiSettings        m_ssgi;
    TaaSettings         m_taa;
    VolumetricFogSettings m_volFog;
    std::string         m_decalAtlasPath;         // assets 相対。空 = デカール無効
    bool                m_shadowsEnabled = true;  // 既定 ON（エディタ/従来シーン互換）
    std::vector<SceneAnimEvent> m_pendingAnimEvents;

    ResourceManager*  m_resourceManager = nullptr;
    GraphicsDevice*   m_device          = nullptr;
    DescriptorHeap*   m_srvHeap         = nullptr;
    ID3D12GraphicsCommandList* m_cmdList = nullptr;
};

} // namespace dx12e
