#pragma once

namespace dx12e
{

// コンタクトシャドウ（スクリーン空間レイマーチによる近接遮蔽）の設定。
// SSAOSettings と同じくシーン単位のレンダ設定（ECS コンポーネントではない）。シリアライズ対象。
// 既定 OFF で編集ビューは白(=遮蔽なし)素通し。GPU 非依存のヘッダオンリー定義
// （Scene / SceneSerializer / ContactShadowPass から共用）。
//
// 既定値はリサーチ（UE / Unity HDRP / 実装記事）で見た実用値に合わせてある:
//   レイ長は「接触」スケールの短距離、ステップ 16、厚み判定あり。
struct ContactShadowSettings
{
    bool  enabled      = false;   // 既定 OFF
    float rayLength    = 0.25f;   // レイ長(m)。伸ばすほどサンプル間隔が粗くなりノイズが増える
    float thickness    = 0.20f;   // 遮蔽とみなす深度差の上限(m)。深度バッファは表面しか持たないので必須
    float bias         = 0.02f;   // 自己遮蔽バイアス(m)。レイ始点の押し出し量にも使う
    float intensity    = 1.0f;    // 遮蔽の強さ(0..1)
    int   steps        = 16;      // レイマーチのステップ数(4..32)
    float maxDistance  = 50.0f;   // この距離(m)を超えたらフェードアウト開始
    float fadeDistance = 10.0f;   // フェードにかける距離(m)
};

} // namespace dx12e
