#pragma once

#include <DirectXMath.h>

namespace dx12e
{
// ポストプロセスのパラメータ。Scene が保持し、シーン JSON に保存される。
// Lua やエディタ UI から書き換えてシーンごとに見た目を変えられる。
struct PostProcessSettings
{
    bool  enabled        = true;   // マスタースイッチ（false なら素通し）
    float exposure       = 1.0f;   // 露出（色の乗算）
    float contrast       = 1.0f;   // コントラスト（1=変化なし）
    float saturation     = 1.0f;   // 彩度（1=変化なし, 0=モノクロ寄り）
    DirectX::XMFLOAT3 tint{1.0f, 1.0f, 1.0f};  // 色味の乗算

    float vignette       = 0.0f;   // 周辺減光 0..1
    float bloom          = 0.0f;   // 簡易ブルーム強度 0..1
    float bloomThreshold = 0.75f;  // ブルームの輝度しきい値

    bool  fxaa           = false;  // 簡易アンチエイリアス
    bool  grayscale      = false;  // グレースケール
};
}
