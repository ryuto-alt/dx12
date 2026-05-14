#pragma once

#include <entt/entt.hpp>
#include "core/Types.h"
#include "ecs/Components.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

namespace dx12e
{

class EditorContext;
class Camera;
class Scene;
class Window;

// シーン (エディタ視点) 用 ImGui タブパネル。
// 中身は Application が描画したオフスクリーン RT (m_sceneViewRT) の SRV を ImGui::Image で表示するだけ。
// GameViewPanel と完全に対称な実装 (Unity の Scene/Game タブ並びに準拠)。
class SceneViewPanel
{
public:
    // Render は ImGui パネルとして "シーン" タブを描画し、16:9 レターボックスで RT を中央に表示する。
    void Render(bool isPlaying, u64 sceneViewTextureId);

    // 既に Begin 済みの ImGui ウィンドウ内に「16:9 レターボックス + 中央 Image」だけ描画する。
    void RenderLetterboxedImage(u64 sceneViewTextureId, ImVec2 avail);

    // Application が SceneView RT のリサイズ判定に使う、前フレームのコンテンツ領域サイズ。
    ImVec2 GetContentSize() const { return m_contentSize; }
    bool   IsHovered()      const { return m_isHovered; }

    // ピッキング/ギズモが扱う「実際の 3D 描画矩形」のスクリーン座標 (16:9 レターボックス内側)
    ImVec2 GetImageMin()    const { return m_imageMin; }
    ImVec2 GetImageSize()   const { return m_imageSize; }

    void RenderGizmo(entt::registry& reg,
                     EditorContext& ctx,
                     Camera* camera,
                     f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    void HandlePicking(entt::registry& reg,
                       EditorContext& ctx,
                       Camera* camera,
                       f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    void HandleDeleteKey(entt::registry& reg,
                         EditorContext& ctx,
                         Scene* scene,
                         f32 vpX, f32 vpY, f32 vpW, f32 vpH);

private:
    // ギズモ操作中の開始時 Transform（Undo用）
    bool      m_gizmoWasUsing = false;
    Transform m_gizmoStartTransform{};

    // パネル表示状態
    ImVec2 m_contentSize{1, 1};   // タブの content area サイズ (RT リサイズ用)
    ImVec2 m_imageMin{0, 0};       // 16:9 Image の左上スクリーン座標 (ピッキング/ギズモ用)
    ImVec2 m_imageSize{1, 1};      // 16:9 Image のサイズ
    bool   m_isHovered = false;

    // SceneView パネル自身の DrawList。ImGuizmo はこの DrawList に描いてもらう
    // (Background だと Image の下に隠れる、Foreground だと他パネルの上に出てしまう)
    ImDrawList* m_drawList = nullptr;
};

} // namespace dx12e
