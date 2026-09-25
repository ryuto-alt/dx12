#pragma once

// ============================================================================
// 選択中の Brain のデバッグ表示（シーンビューに重ねる）
// ============================================================================
//   視界の扇形（sightRange × sightFov）と気配の円、最後の視線判定の線（緑=通った / 赤=遮られた）、
//   聞いた音（輪。古いほど薄い）、最後に見た位置、群衆の通路の角、今の行動と選んだ理由。
// ★ImGui の背景ドローリストへ描くだけ（描画パイプラインには触らない＝スクショの絵は変わらない）。
//   Play 中（一時停止中も含む）にエディタで Brain を選ぶと出る。Brain.debugDraw で消せる。
// ============================================================================

#include <entt/entt.hpp>

namespace dx12e
{
class Camera;
class EditorContext;

void AiDebugOverlayFrame(entt::registry& reg, EditorContext& ctx, Camera* camera);

} // namespace dx12e
