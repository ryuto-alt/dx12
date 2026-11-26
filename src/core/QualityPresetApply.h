#pragma once
// ===========================================================================
// 品質プリセットの設定 / 取得（W1-C1: 口だけ。設定値の適用とシーンへの反映は C7）。
//   Application の private に触れる処理は AppAccess（lane-C ブロック）経由。ここは自由関数の窓口。
//   値（予算・VFX・アップスケーラ）は renderer/QualityPreset.h（生成表 QualityPresetTable.inc）。
// ===========================================================================
#include "renderer/QualityPreset.h"

namespace dx12e
{
class Application;

// プリセットを設定する。recordUndo=true なら Undo の 1 回分に積む（C7 から有効。C1 では状態の記録だけ）。
// 範囲外の id は false を返し、何も変えない。
bool SetQualityPreset(Application& app, QualityPresetId id, bool recordUndo);

// 現在のプリセット。
QualityPresetId GetQualityPreset(const Application& app);

} // namespace dx12e
