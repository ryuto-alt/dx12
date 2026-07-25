#pragma once

#include <entt/entt.hpp>
#include <vector>
#include <utility>
#include <DirectXMath.h>
#include "core/Types.h"
#include "ecs/Components.h"

struct ID3D12GraphicsCommandList;   // UI プレビューのテクスチャ遅延ロード用

namespace dx12e
{

class EditorContext;
class Camera;
class Scene;
class Window;
class ResourceManager;
class DescriptorHeap;

// メッシュ単位(サブメッシュ)のピッキング結果。テクスチャD&Dでどのメッシュに
// マテリアルを割り当てるか特定するために使う（MeshRenderer::meshes[] のどの要素に
// 当たったか。実体は editor/ScenePick.h の RaycastScene が返す先頭ヒット）。
struct SubmeshPickResult
{
    entt::entity entity = entt::null;
    u32 submeshIndex = 0;
};

class SceneViewPanel
{
public:
    // 非 Play 時のゲーム内 UI プレビュー（ctx.uiEditMode ON のとき）。
    // UISystem::RenderPreview をギズモと同じ背景 DrawList へ描く（3D の上・全パネルの下）。
    // resources/srvHeap/cmdList は UIImage テクスチャの遅延ロード用（##GameUI 経路と同じ）。
    void RenderUiPreview(entt::registry& reg,
                         EditorContext& ctx,
                         f32 vpX, f32 vpY, f32 vpW, f32 vpH,
                         ResourceManager* resources,
                         DescriptorHeap* srvHeap,
                         ID3D12GraphicsCommandList* cmdList);

    void RenderGizmo(entt::registry& reg,
                     EditorContext& ctx,
                     Camera* camera,
                     f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    // UI 編集モード（ctx.uiEditMode）の SceneView 上での UI 要素編集。
    //   - 左クリック: 解決済み UI 矩形を手前優先でヒットテストして選択
    //     （ヒット無しなら HandlePicking の 3D ピッキングへフォールスルー）
    //   - 選択中の UIRect: アウトライン + 8 リサイズハンドルを描画し、
    //     ハンドルドラッグでリサイズ / 矩形内ドラッグで移動（Undo 対応）
    // RenderGizmo の後（ImGuizmo の当たり判定確定後）・HandlePicking の前に呼ぶこと。
    void HandleUiEditing(entt::registry& reg,
                         EditorContext& ctx,
                         f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    // ビューポートツール（地形ブラシ / ライトのハンドル操作など）へマウスを先に回す受け口。
    // EditorContext::viewportToolHandlers を先頭から順に呼び、true を返したものがあれば
    // 今フレームの UI 編集・3D ピッキングを行わない。EditorLayer が RenderGizmo の後・
    // HandleUiEditing の前に呼ぶこと。戻り値 = 消費されたか。
    bool RunViewportTools(EditorContext& ctx,
                          Camera* camera,
                          f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    void HandlePicking(entt::registry& reg,
                       EditorContext& ctx,
                       Camera* camera,
                       f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    // 現在のマウス位置でレイキャストし、ヒットしたエンティティ+サブメッシュ番号を返す
    // (選択状態は変更しない、副作用なしの問い合わせ)。ヒット無しは entity=entt::null。
    // 中身は HandlePicking と同じ RaycastScene（メッシュ限定モード）。
    SubmeshPickResult PickEntityAndSubmesh(entt::registry& reg,
                                           EditorContext& ctx,
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
    // ギズモ操作中か（前フレームの ImGuizmo::IsUsing()）
    bool      m_gizmoWasUsing = false;
    // 全選択エンティティの開始時 Transform（Undo の before。左ボタン押下フレームで採取）
    std::vector<std::pair<entt::entity, Transform>> m_gizmoStartGroup;
    // マルチ選択時の仮想ピボット（選択群の中心。Manipulate が毎フレーム書き換える）。
    // 前フレームとの差分行列を各選択のワールドへ右から掛けることで、Translate だけでなく
    // Rotate/Scale も群全体に効かせる。
    DirectX::XMFLOAT4X4 m_gizmoPivot{};

    // ---- 重なりの循環選択（同じ場所を連続クリックで手前→奥へ）----
    DirectX::XMFLOAT2         m_cyclePos{-1e9f, -1e9f};  // 前回クリックのスクリーン座標
    std::vector<entt::entity> m_cycleChain;              // 前回の重なり列（手前→奥）
    int                       m_cycleIndex = 0;          // 現在選んでいる段
    double                    m_cycleTime  = -1.0;       // 前回クリック時刻（ImGui::GetTime）

    // ビューポートツール（EditorContext::viewportToolHandlers）が今フレームの
    // マウス操作を消費したか。UI 編集・3D ピッキングがこれを見てスキップする。
    bool m_toolConsumed = false;

    // オービット/ドリーの基準点と距離（ドラッグ開始時に確定）
    DirectX::XMFLOAT3 m_orbitPivot    = {0.0f, 0.0f, 0.0f};
    f32               m_orbitDistance = 10.0f;

    // 右クリックのコンテキストメニュー対象(押下時にピッキングして確定、Popup描画まで保持)
    SubmeshPickResult m_textureCtxTarget{};

    // ---- UI 編集モード（ctx.uiEditMode）のドラッグ状態 ----
    // m_uiDragEdges: -1=非ドラッグ, 0=移動, 正=リサイズ中エッジの bit 組み合わせ
    //                (1=左 2=右 4=上 8=下。四隅は 2bit。定数は .cpp の kUiEdge* 参照)
    int               m_uiDragEdges  = -1;
    entt::entity      m_uiDragEntity = entt::null;
    UIRect            m_uiDragStartRect{};             // ドラッグ開始時の UIRect（Undo の before）
    DirectX::XMFLOAT2 m_uiDragStartMouse{0.0f, 0.0f};  // 開始時のマウス（スクリーン座標）
    DirectX::XMFLOAT2 m_uiDragStartMin{0.0f, 0.0f};    // 開始時の解決済みスクリーン矩形
    DirectX::XMFLOAT2 m_uiDragStartMax{0.0f, 0.0f};
    f32               m_uiDragCanvasScale = 1.0f;      // スクリーンΔ → キャンバス px の換算係数
    // 今フレームの左クリックを UI 編集（選択/ドラッグ開始）が消費したか。
    // HandlePicking がこれを見て 3D ピッキングをスキップする（毎フレーム再計算）。
    bool              m_uiClickConsumed = false;
};

} // namespace dx12e
