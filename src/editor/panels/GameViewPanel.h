#pragma once

#include "core/Types.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

namespace dx12e
{

// シーンに置かれた CameraComponent(isActive=true) の視点を表示する ImGui パネル。
// 中身はオフスクリーン RT を ImGui::Image() で描画するだけ。
class GameViewPanel
{
public:
    // gameViewTextureId は ImGui DX12 バックエンド用の SRV GPU ハンドル値。
    // 未準備時は 0 を渡せば暗いプレースホルダーが表示される。
    void Render(bool isPlaying, u64 gameViewTextureId);

    // 既に Begin 済みの ImGui ウィンドウ内に「16:9 レターボックス + 中央 Image」だけ描画する。
    // EditorLayer の Maximize on Play から呼ばれて、通常 Render() と全く同じ見た目を再現する。
    void RenderLetterboxedImage(u64 gameViewTextureId, ImVec2 avail);

    // 前フレームのコンテンツ領域サイズ。Application が GameView RT のリサイズ判定に使う。
    ImVec2 GetContentSize() const { return m_contentSize; }

    // ホバー状態。Play 中の入力ゲートに使う (Editor 中は未使用)。
    bool IsHovered() const { return m_isHovered; }

private:
    ImVec2 m_contentSize{1, 1};
    bool   m_isHovered = false;
};

} // namespace dx12e
