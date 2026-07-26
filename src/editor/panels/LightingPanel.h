#pragma once

// ===== ライティング・パネル =====
// シーンの「光まわり」を 1 画面で詰めるための独立フローティング窓。
//   ・シーン内ライトの一覧（種別 / 名前 / 色 / 強度、クリックで選択、目玉で一時ミュート）
//     ＋ GPU へ送れる灯数の上限（クラスタード: 合計 1024 灯）に対する使用数の警告
//     ＋ クラスタデバッグ表示（ライト複雑度ヒートマップ / クラスタ境界）
//   ・太陽（最初の DirectionalLight）… 時刻 / 方位 / 高度 / 色温度 / 強度 / 環境光
//   ・影 … シーンの ON/OFF・CSM 品質・カスケード可視化
//   ・スカイ / IBL … envMapPath・iblIntensity・skyboxIntensity・drawSkybox
//   ・ライティング・プリセット … 昼/夕暮れ/夜/屋内/ホラー/スタジオ（適用は Undo 1 エントリ）
//
// 影とスカイの実体は Application が持っている値なので参照で受け取る。
// 既存の「エンジン設定」「Skybox / IBL」窓は残したまま、こちらからも同じ状態を触る。
//
// パネルは VfxEditor 等と同じ独立フローティング窓として開く（EditorContext::AnyToolWindowOpen
// には含めない＝右下タブ領域を占有しない）。

#include "core/Types.h"

namespace dx12e
{

class Scene;
class EditorContext;

// ctx.showLighting が false なら何もしない。EditorLayer が毎フレーム 1 回呼ぶ。
void RenderLightingPanel(Scene* scene,
                         EditorContext& ctx,
                         i32& shadowQualityIndex,
                         u32& shadowMapSize,
                         bool& shadowMapDirty,
                         f32& cascadeSplitLambda,
                         f32& cascadeBlendBand,
                         bool& showCascadeDebug);

} // namespace dx12e
