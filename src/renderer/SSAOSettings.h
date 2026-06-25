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
};

} // namespace dx12e
