#pragma once

// ===== フローティング窓をメインウィンドウ内へ収める =====
// ImGui は保存位置の無い新規窓を (60,60) に開くので、ツールバーに被って開いていた。
// また imgui.ini に残った ViewportPos で、フローティング窓が「メインウィンドウの外（= 実画面）」へ
// 別 OS 窓として出る事故があった（マテリアルエディタ）。
//   ・領域（ツールバーの下〜ステータスバーの上）は EditorLayer が毎フレーム SetArea で渡す
//   ・ImGuiManager::EndFrame が【全パネルの描画が終わった後】に Apply を呼ぶ
//     （Application 直属のツール窓も含めて拾うため、EditorLayer::Render の中では呼ばない）
//   ・always=true（仮想入力モード）は毎フレーム、false（通常）は出現直後の数フレームだけクランプ
// ドック窓・ポップアップ・ツールチップ・NoMove 窓は対象外。人が意図して別 OS 窓へ出した窓（通常時）も触らない。

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

namespace dx12e::floatguard
{
void SetArea(ImVec2 areaMin, ImVec2 areaMax);   // スクリーン座標
void Apply(bool always);
} // namespace dx12e::floatguard
