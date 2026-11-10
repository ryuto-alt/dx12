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

    // ---- 時間方向アップスケーラ（renderer/UpscalerMath.h）----
    // enabled=false … アップスケーラ off（従来）。enabled=true の上で upscaler が TAA か DLSS かを選ぶ。
    // 既定は TAA ＝ 既存シーンの絵は変わらない。DLSS が使えない環境では実行時に TAA へ縮退する（保存値は変えない）。
    int   upscaler      = 0;      // upscale::Kind（0 = エンジン TAA / 1 = DLSS）
    int   dlssQuality   = 1;      // upscale::DlssQuality（0 = DLAA / 1 = Quality / 2 = Balanced / 3 = Performance）
};

} // namespace dx12e
