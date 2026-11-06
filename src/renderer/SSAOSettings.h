#pragma once

namespace dx12e
{

// SSAO のグローバル設定（ECS コンポーネントではなくシーン単位のレンダ設定）。
// シリアライズ対象。既定 OFF で編集ビューは白(AO=1.0)素通し。
// GPU 非依存のヘッダオンリー定義（Scene / SceneSerializer / SSAOPass から共用）。
struct SSAOSettings
{
    bool  enabled     = false;   // 既定 OFF
    float radius      = 0.5f;    // ワールド(ビュー)空間半径
    float bias        = 0.025f;  // 自己遮蔽バイアス
    float intensity   = 1.0f;    // 遮蔽の強さ
    float power       = 1.5f;    // pow() のべき指数（コントラスト）
    int   sampleCount = 16;      // 8 / 16
    bool  blur        = true;    // 4x4 ボックスブラー
    // 拡散 AO に Multi-bounce（Jimenez 2016, GTAO）を掛ける。暗い隙間でも明るい色の面は周囲からの
    // 反射光で完全には暗くならない近似（albedo 依存）。既定 OFF ＝ 既存シーンの絵は 1 ビットも変わらない。
    bool  multiBounce = false;
};

} // namespace dx12e
