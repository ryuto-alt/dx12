#pragma once
// ===========================================================================
// 品質プリセットの設定 / 取得（W1-C7 で本体）。
//   Application の private に触れる処理は AppAccess（lane-C ブロック）経由。ここは自由関数の窓口。
//   値（予算・VFX・アップスケーラ）は renderer/QualityPreset.h（生成表 QualityPresetTable.inc）、
//   構成の選択は renderer/QualityPresetPlan.h、Scene への書き込みは core/QualityPresetScene.h。
// ===========================================================================
#include "renderer/QualityPresetPlan.h"

#include <string>

namespace dx12e
{
class Application;

// プリセットを設定する（計画をシーンの設定へ書き、VFX の倍率を切り替える）。
// recordUndo=true なら Undo の 1 回分に積む（Editor モードのとき。MCP の呼び出し中なら「AI: set_quality_preset」の 1 エントリ）。
// 範囲外の id は false を返し、何も変えない。
bool SetQualityPreset(Application& app, QualityPresetId id, bool recordUndo);

// 現在のプリセット。
QualityPresetId GetQualityPreset(const Application& app);

// 今の caps（DXR / DLSS）で選んだプリセットの計画を作る（MCP の結果・UI の縮退表示用）。
QualityPlan CurrentQualityPlan(const Application& app);

// 選んだ後にシーンの設定を手で変えていないか。
bool QualityMatchesScene(const Application& app);

// ---- W1-C8: 新規シーンのルックのプロファイル（"epic" | "classic"。実体は scene/SceneLookProfile.h）----
// 名前が有効か（MCP new_scene {profile} の検査用）。
bool IsNewSceneProfileName(const std::string& name);
// 次の新規シーンのプロファイルを予約する（MCP new_scene {profile}。1 回で消費され、以後は既定の epic へ戻る）。
void SetNextNewSceneProfile(const std::string& name);
// 新規シーンの直後（空シーンの組み立てと gi::ApplyNew の後）に呼ぶ。予約されたプロファイル（無ければ epic）を Scene へ当て、
// 品質プリセットの id・空間スケール・時間履歴を揃える。当てたプロファイル名を返す。Undo は積まない（新規シーンは履歴ごと空）。
//   giNewApplied: gi::ApplyNew が通ったか。
std::string ApplyNewSceneProfile(Application& app, bool giNewApplied);

} // namespace dx12e
