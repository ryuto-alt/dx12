#pragma once

namespace dx12e
{

// TAA（テンポラルアンチエイリアス）のシーン単位設定。
// SSAOSettings / ContactShadowSettings と同じ流儀（ECS コンポーネントではないレンダ設定）。
//
// なぜ PostProcessSettings に入れないか:
//   PostProcessSettings は uber パス 1 枚に渡す「マスク付きエフェクト」の集合だが、TAA は
//   チェーンの構造そのものを変える（深度+速度プリパスの強制・投影行列のジッタ・FXAA 排他）。
//   性質が違うので SSAO と同じく独立させる。
struct TaaSettings
{
    bool  enabled       = false;  // 既定 OFF
    int   sampleCount   = 8;      // ハルトン列の周期（4/8/16）。少ないほどシャープだが収束時のジッタが目立つ
    float feedbackMin   = 0.88f;  // 履歴の最小比率（現フレームと食い違うピクセル）
    float feedbackMax   = 0.97f;  // 履歴の最大比率（安定しているピクセル）
    float varianceGamma = 1.0f;   // 近傍クリップの許容幅 μ±γσ（小さいほどゴーストが減りチラつきが増える）
    float jitterScale   = 1.0f;   // ジッタ量の倍率（1.0 = ±0.5px）。ブラーが強すぎるなら下げる
    bool  debugVelocity = false;  // 速度バッファを画面に可視化する（目視検証用。シリアライズ対象外）
};

} // namespace dx12e
