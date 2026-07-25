#pragma once

#include "core/Types.h"

namespace dx12e
{

// PCSS（Percentage-Closer Soft Shadows）— CSM の固定幅 PCF を
// 「ブロッカー探索 → 可変ペナンブラ」へ置き換える設定。TaaSettings と同じ流儀（ヘッダのみ）。
//
// ★OFF（既定）のときシェーダは従来の 3x3 PCF 経路をそのまま通る＝**絵はビット一致**。
//
// 物理モデル: 太陽を「角半径 θ の面光源」とみなす。遮蔽物が受光点の d メートル上に
// あるとき、半影の世界半径は d * tan(θ)。CSM は正射なので、シャドウマップの
// 深度差（0..1）にカスケードの world サイズを掛けたものが d になり、
// それを UV へ戻すと world サイズが約分されて **penumbraUV = (zR - zB) * tanTheta** になる。
// ＝カスケードごとの補正が要らず、境界で半影の太さが不連続にならない。
struct ShadowPcssSettings
{
    bool enabled = false;      // 既定 OFF（従来と同じ絵）

    // 太陽の角半径の tan。0.5 度（実際の太陽）だと tan≒0.0044 で影がほぼ硬いので、
    // ゲーム的な見栄えのため既定は誇張してある。0.05 ≒ 2.9 度。
    f32 lightTanAngle = 0.05f;   // 0.001 .. 0.5

    // 半影の上限（シャドウマップのテクセル数）。塗り面積＝コストの上限を決める。
    f32 maxPenumbraTexels = 16.0f;   // 1 .. 64

    // ブロッカー探索の半径（テクセル数）。大きすぎると「重要な遮蔽物を飛ばして影に穴が開く」、
    // 小さすぎると遠くの遮蔽物を見つけられず半影が伸びない。既定は maxPenumbra と同じ。
    f32 blockerSearchTexels = 16.0f; // 1 .. 64

    // 時間方向ディザ（TAA が有効なときだけ効かせる）。
    // フレーム連番 × 黄金比でサンプル回転位相を回すと、TAA の蓄積で収束してノイズが消える。
    // TAA が無効なときに回すと画面がちらつくだけなので、エンジン側が自動で 0 に落とす。
    bool temporalDither = true;
};

} // namespace dx12e
