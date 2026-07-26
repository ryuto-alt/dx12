#pragma once

// TAA のジッタ生成（純関数・ヘッダオンリー・GPU 非依存）。
// TaaPass から使い、tests/velocity_math_test.cpp で単体検証する。
//
// ここを間違えると「静止しているのに速度バッファが 0 にならない」＝ TAA の履歴が
// 毎フレーム破綻するという、症状の割に原因が見えにくい事故になる。数式は単体テストで守る。

#include "core/Types.h"

namespace dx12e
{

// ラディカル逆関数（ハルトン列）。戻り値は (0, 1)。
// 出典: https://alextardif.com/TAA.html
inline float HaltonRadicalInverse(u32 i, u32 b)
{
    float f = 1.0f;
    float r = 0.0f;
    while (i > 0)
    {
        f /= static_cast<float>(b);
        r += f * static_cast<float>(i % b);
        i /= b;
    }
    return r;
}

// index 番目のサブピクセルジッタを NDC の平行移動量として返す。
//
//   Halton(index+1) は (0,1) なので -0.5 して [-0.5, 0.5) の「px 単位のオフセット」にする。
//   NDC は幅 2.0 に vpW ピクセルが並ぶので 1px = 2/vpW。したがって ±0.5px = ±1/vpW。
//   y は「NDC は上が +、画面は下が +」で符号が反転する。
//
// ★この値は 2 か所で完全に同一でなければならない:
//     1) 投影行列へ後乗算する平行移動 T(jx, jy, 0)
//     2) 速度シェーダが clip.xy -= jitter * clip.w で引き算する量
//   片方だけ符号やスケールを変えると、静止時に速度が乗ってゴーストする。
struct TaaJitterNdc { float x; float y; };

inline TaaJitterNdc ComputeTaaJitterNdc(u32 index, u32 vpW, u32 vpH, float scale = 1.0f)
{
    const float hx = HaltonRadicalInverse(index + 1, 2) - 0.5f;
    const float hy = HaltonRadicalInverse(index + 1, 3) - 0.5f;
    const float w = static_cast<float>((vpW > 0) ? vpW : 1);
    const float h = static_cast<float>((vpH > 0) ? vpH : 1);
    return { 2.0f * hx * scale / w, -2.0f * hy * scale / h };
}

} // namespace dx12e
