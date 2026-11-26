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

    // ---- W1-C1: LS-02a（Epic の太陽影）が使う欄を先に足した（C1 は形だけ。読むのは C6）。
    //      既定はすべて従来と同じ挙動＝何も読まなければ絵はビット一致。 ----

    // Epic の構成: RT の太陽影が静的な物を描き、動的 CSM は動的な物（スキンド・半透明）だけを描く。
    // true のとき 2048 D16 の 3 カスケード + 下の補正を使う。false は従来（2048 D32 の PCF）。
    bool epicDynamicCsm = false;

    // pancaking: 影 PSO の DepthClipEnable=FALSE（近面より手前の遮蔽物を z=0 に潰して残す）。epicDynamicCsm のときだけ有効。
    bool pancaking = true;

    // 受光側の法線オフセット: worldPos += Ngeom * texelWorld[c] * (base + slope * (1 - N.L))。V2 の中だけで使う。
    f32 receiverNormalOffsetBase  = 1.5f;   // 0 .. 4（テクセル数）
    f32 receiverNormalOffsetSlope = 2.0f;   // 0 .. 4

    // 影を描く側のラスタ DepthBias の倍率。シーン全体が shadingRevision=2 のときだけ掛ける（V1 の受光面に当てない）。
    f32 casterBiasScaleV2 = 0.25f;          // 0.05 .. 1

    // Gather ベースの PCSS（ブロッカー探索 GatherRed + フィルタタップ数）。動的 CSM の分にだけ使う。
    bool gatherPcss        = true;
    u32  gatherFilterTaps  = 24;            // 8 .. 32

    // カスケード境界のブレンド帯を IGN のディザで片方だけサンプルする（TAA / DLSS に平均させる）。
    bool cascadeDitherBlend = true;
};

} // namespace dx12e
