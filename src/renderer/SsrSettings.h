#pragma once

namespace dx12e
{

// スクリーン空間反射（SSR）の設定。SSAOSettings / ContactShadowSettings と同じ流儀の
// シーン単位レンダ設定（ECS コンポーネントではない）。シリアライズ対象。既定 OFF。
//
// 反射は「前フレームのシーンカラー」から取るので 1 フレーム遅れる。
// 深度プリパスの G-Buffer（法線/ラフネス）を必要とするため、有効にすると
// 深度+速度プリパスが常時走る（TAA が OFF でも）。
// 既定値はリサーチ（FidelityFX SSSR / Frostbite stochastic SSR / Babylon.js SSR）の実用値。
struct SsrSettings
{
    bool  enabled         = false;  // 既定 OFF
    float intensity       = 1.0f;   // 反射の強さ（confidence への乗算 0..1）
    float maxDistance     = 50.0f;  // レイの最大到達距離(m)
    float thickness       = 0.5f;   // 深度差をヒットとみなす上限(m)。深度バッファは表面しか持たない
    int   maxSteps        = 48;     // DDA の最大ステップ数(16..128)
    float stride          = 2.0f;   // DDA の 1 ステップのピクセル数(1..8)。大きいほど速く粗い
    float roughnessCutoff = 0.6f;   // これを超えるラフネスはレイを打たず IBL に任せる
    float edgeFade        = 0.15f;  // 画面端フェード幅（NDC 比 0..0.5）
    float bias            = 0.05f;  // レイ始点の押し出し(m)。自己交差対策
};

} // namespace dx12e
