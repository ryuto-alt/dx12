#pragma once

#include <entt/entt.hpp>
#include <string>
#include <vector>
#include "core/Types.h"
#include "ecs/Components.h"
#include "scene/SceneSerializer.h"   // PrefabOverride（プレハブ差分表示のキャッシュ用）

namespace dx12e
{

class EditorContext;
class Camera;
class AudioSystem;
class PhysicsDebugRenderer;
class GameClock;
class Scene;
class ScriptEngine;
class AssetBrowserPanel;

class InspectorPanel
{
public:
    // 選択エンティティのコンポーネントを描く（グローバル設定は RenderEngineSettings へ分離）。
    void Render(entt::registry& reg,
                EditorContext& ctx,
                Scene* scene);

    // グローバルなエンジン設定（カメラ速度/シャドウ/オーディオ/VSync/ビルド）を
    // 独立した「エンジン設定」ウィンドウに描く。Inspector からは分離し、下ドックに置く。
    void RenderEngineSettings(EditorContext& ctx,
                              Camera* camera,
                              AudioSystem* audioSystem,
                              PhysicsDebugRenderer* physicsDebugRenderer,
                              bool& physicsDebugDraw,
                              bool& useVsync,
                              i32& shadowQualityIndex,
                              u32& shadowMapSize,
                              bool& shadowMapDirty,
                              f32& cascadeSplitLambda,
                              f32& cascadeBlendBand,
                              bool& showCascadeDebug,
                              GameClock* clock);

    void SetScriptEngine(ScriptEngine* e) { m_scriptEngine = e; }
    void SetAssetsDir(const std::string& d) { m_assetsDir = d; }
    // マテリアルテクスチャスロットのサムネイルプレビュー/選択ダイアログに使う
    void SetAssetBrowser(AssetBrowserPanel* p) { m_assetBrowser = p; }

    // Undo 用: コンポーネント編集の追跡状態
    template<typename T>
    struct EditState
    {
        bool editing = false;
        T    snapshot{};
    };

private:
    // Undo 用: ウィジェット操作開始時のスナップショット
    bool      m_transformEditing = false;
    Transform m_transformSnapshot{};

    bool  m_pbrEditing = false;
    float m_pbrMetallicSnapshot  = -1.0f;
    float m_pbrRoughnessSnapshot = -1.0f;

    EditState<PointLight>       m_plEdit;
    EditState<DirectionalLight> m_dlEdit;
    EditState<SpotLight>        m_slEdit;
    EditState<AudioSource>      m_audioEdit;
    EditState<CameraComponent>  m_camEdit;
    EditState<Gimmick>          m_gimmickEdit;
    EditState<ParticleEmitter>  m_emitterEdit;
    EditState<Sprite2D>        m_spriteEdit;
    EditState<TrailRenderer>    m_trailEdit;
    EditState<RigidBody>        m_rbEdit;
    EditState<BoxCollider>      m_boxColEdit;
    EditState<SphereCollider>   m_sphereColEdit;
    EditState<CapsuleCollider>  m_capsuleColEdit;
    EditState<CharacterController> m_ccEdit;
    EditState<NetworkIdentity>  m_netIdEdit;
    EditState<NetworkTransform> m_netTfEdit;
    EditState<UICanvas>         m_uiCanvasEdit;
    EditState<UIRect>           m_uiRectEdit;
    EditState<UIImage>          m_uiImageEdit;
    EditState<UIText>           m_uiTextEdit;
    EditState<UIButton>         m_uiButtonEdit;
    EditState<UISlider>         m_uiSliderEdit;
    EditState<UIToggle>         m_uiToggleEdit;
    EditState<UIScrollView>     m_uiScrollEdit;
    EditState<UILayout>         m_uiLayoutEdit;
    EditState<UIAnimator>       m_uiAnimatorEdit;
    EditState<UIAnimPlayer>     m_uiAnimPlayerEdit;
    EditState<SpriteAnimator>   m_spriteAnimatorEdit;

    // プレハブ差分のキャッシュ。差分計算はサブツリー全体を JSON 化するので、
    // 毎フレームやると要素の多い UI プレハブで無駄が大きい。選択が変わった時と
    // 一定間隔でだけ取り直す（表示専用の情報なので少し遅れても困らない）。
    entt::entity m_prefabDiffEntity = entt::null;
    f32          m_prefabDiffTimer  = 0.0f;
    bool         m_prefabDiffOk     = false;
    std::vector<SceneSerializer::PrefabOverride> m_prefabDiff;

    // 種別専用インスペクター（ライト/オーディオは専用UIを最前面に出す。共通部品はこの下に並ぶ）。
    void RenderLightHero(entt::registry& reg, EditorContext& ctx, entt::entity e);
    // プレハブインスタンスのヘッダー（リンク先 / 変更点 / 適用・元に戻す）。
    // Revert はエンティティを作り直すので、実行はフレーム境界（ctx.pending*）へ回す。
    void RenderPrefabHeader(entt::registry& reg, EditorContext& ctx, Scene& scene, entt::entity e);
    void RenderAudioHero(entt::registry& reg, EditorContext& ctx, entt::entity e);

    ScriptEngine* m_scriptEngine = nullptr;
    std::string   m_assetsDir;
    AssetBrowserPanel* m_assetBrowser = nullptr;
};

} // namespace dx12e
