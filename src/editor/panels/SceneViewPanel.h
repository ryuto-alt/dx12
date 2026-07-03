#pragma once

#include <entt/entt.hpp>
#include <vector>
#include <utility>
#include <DirectXMath.h>
#include "core/Types.h"
#include "ecs/Components.h"

namespace dx12e
{

class EditorContext;
class Camera;
class Scene;
class Window;

// メッシュ単位(サブメッシュ)のピッキング結果。テクスチャD&Dでどのメッシュに
// マテリアルを割り当てるか特定するために使う（HandlePicking のエンティティ全体
// AABBと違い、MeshRenderer::meshes[] の各サブメッシュAABBを個別にレイテストする）。
struct SubmeshPickResult
{
    entt::entity entity = entt::null;
    u32 submeshIndex = 0;
};

class SceneViewPanel
{
public:
    void RenderGizmo(entt::registry& reg,
                     EditorContext& ctx,
                     Camera* camera,
                     f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    void HandlePicking(entt::registry& reg,
                       EditorContext& ctx,
                       Camera* camera,
                       f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    // 現在のマウス位置でレイキャストし、ヒットしたエンティティ+サブメッシュ番号を返す
    // (選択状態は変更しない、副作用なしの問い合わせ)。ヒット無しは entity=entt::null。
    SubmeshPickResult PickEntityAndSubmesh(entt::registry& reg,
                                           Camera* camera,
                                           f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    // ビューポートカメラ操作（Unity 風）:
    //   スクロール    = ドリーズーム（右クリック中はフライ移動速度の増減）
    //   中ボタンドラッグ = パン
    //   Alt + 左ドラッグ = 選択物（無ければ前方点）を中心にオービット
    // ※ 右クリック長押し + WASD のフライは Application::Update 側で処理。
    void HandleCameraNavigation(entt::registry& reg,
                                EditorContext& ctx,
                                Camera* camera,
                                f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    void HandleDeleteKey(entt::registry& reg,
                         EditorContext& ctx,
                         Scene* scene,
                         f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    // 右クリック(ドラッグでないただのクリック)でメッシュの上に乗っていたら「テクスチャを外す」
    // コンテキストメニューを出す。右クリック長押し+ドラッグはフライカメラ操作
    // (Application::Update が GetAsyncKeyState で独自に処理)なので、それと衝突しないよう
    // 「押下位置からの移動量が小さいクリックだけ」をメニュー対象にする。
    void HandleTextureContextMenu(entt::registry& reg,
                                  EditorContext& ctx,
                                  Camera* camera,
                                  f32 vpX, f32 vpY, f32 vpW, f32 vpH);

private:
    // ギズモ操作中の開始時 Transform（Undo用）
    bool      m_gizmoWasUsing = false;
    Transform m_gizmoStartTransform{};
    // マルチ選択時: 全選択エンティティの開始時 Transform
    std::vector<std::pair<entt::entity, Transform>> m_gizmoStartGroup;

    // オービット/ドリーの基準点と距離（ドラッグ開始時に確定）
    DirectX::XMFLOAT3 m_orbitPivot    = {0.0f, 0.0f, 0.0f};
    f32               m_orbitDistance = 10.0f;

    // 右クリックのコンテキストメニュー対象(押下時にピッキングして確定、Popup描画まで保持)
    SubmeshPickResult m_textureCtxTarget{};
};

} // namespace dx12e
