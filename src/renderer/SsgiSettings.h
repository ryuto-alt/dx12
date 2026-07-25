#pragma once

namespace dx12e
{

// スクリーン空間 GI（間接拡散）の設定。既定 OFF。
// 前フレームのシーンカラー（リニア HDR・トーンマップ前）を間接光のソースにする。
//
// ★前フレームカラーには前フレームの SSGI が焼き込まれている＝多重バウンスが時間方向に
//   自然に発生する。その代わりエネルギーが発散し得るので intensity は 1.0 未満、
//   積分値は clampValue でクランプする（firefly 対策も兼ねる）。
// ★AO は IBL 項にだけ掛け、SSGI 項には掛けない（二重計上の解消。Forward.hlsl 参照）。
struct SsgiSettings
{
    bool  enabled     = false;  // 既定 OFF
    float intensity   = 0.8f;   // 間接拡散の強さ。1.0 超は多重バウンスで発散しうる
    float radius      = 6.0f;   // レイの最大到達距離(m)
    float thickness   = 0.5f;   // 深度差をヒットとみなす上限(m)
    int   rayCount    = 2;      // 1 ピクセルあたりのレイ数(1..4)
    int   stepCount   = 12;     // 1 レイのステップ数(4..24)
    float clampValue  = 4.0f;   // 積分結果の輝度クランプ（firefly / 発散対策）
    float feedback    = 0.92f;  // 時間蓄積の履歴比率(0.8..0.98)。0.98 超は TAA と二重残像
    bool  iblFallback = true;   // レイがミスしたら irradiance キューブを積む（★カメラ回転で明るさが変動しないために必須）
};

} // namespace dx12e
