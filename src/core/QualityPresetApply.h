#pragma once
// ===========================================================================
// 品質プリセットの設定 / 取得（W1-C7 で本体）。
//   Application の private に触れる処理は AppAccess（lane-C ブロック）経由。ここは自由関数の窓口。
//   値（予算・VFX・アップスケーラ）は renderer/QualityPreset.h（生成表 QualityPresetTable.inc）、
//   構成の選択は renderer/QualityPresetPlan.h、Scene への書き込みは core/QualityPresetScene.h。
// ===========================================================================
#include "renderer/QualityPresetPlan.h"

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

} // namespace dx12e
