#pragma once

#include <cmath>

namespace dx12e
{

// Sprite2D のフリップブック/UVスクロールの UV 計算（純関数・GPU/ECS 非依存 → 単体テスト可能。
// tests/sprite_anim_test.cpp 参照）。Application が毎フレーム Submit 前に呼んで uvMin/uvMax を差し替える。
struct SpriteUvRect
{
    float u0, v0, u1, v1;
};

// フリップブック: テクスチャを cols x rows グリッドとみなし、frame = floor(t*fps) % frames の
// セル UV を返す（ユーザー指定の uvMin/uvMax は無視）。
//  - animFrames : 総フレーム数（>0 で有効。呼び出し側で保証）
//  - animFps    : 再生速度（<=0 は 8 とみなす）
//  - animCols   : シートの列数（<=0 は animFrames = 横1行ストリップ）
//  - animRow    : シート内の開始行（複数アニメを行で並べたシート用）
//  - animRows   : シートの総行数（<=0 は自動 = animRow + ceil(animFrames/cols)。
//                 アニメ行の下に別アニメ行が続くシートでは明示指定する）
inline SpriteUvRect ComputeFlipbookUv(int animFrames, float animFps, int animCols,
                                      int animRow, int animRows, float time)
{
    const int   cols = (animCols > 0) ? animCols : animFrames;
    const float fps  = (animFps > 0.0f) ? animFps : 8.0f;

    int frame = static_cast<int>(std::floor(time * fps)) % animFrames;
    if (frame < 0) frame += animFrames;   // 負の時刻でも [0, frames) に収める

    const int animRowSpan = (animFrames + cols - 1) / cols;   // このアニメが占める行数
    const int rows = (animRows > 0) ? animRows : (animRow + animRowSpan);

    const int col = frame % cols;
    const int row = animRow + frame / cols;

    const float cw = 1.0f / static_cast<float>(cols);
    const float ch = 1.0f / static_cast<float>(rows);
    return { col * cw, row * ch, (col + 1) * cw, (row + 1) * ch };
}

// UVスクロール: uvMin/uvMax の両方へ (scrollU*t, scrollV*t) を加算（矩形サイズは不変）。
// サンプラーは WRAP なので値域は問わないが、長時間再生での float 精度劣化を防ぐため
// オフセットを [0,1) に折り返す。
inline SpriteUvRect ComputeScrollUv(float u0, float v0, float u1, float v1,
                                    float scrollU, float scrollV, float time)
{
    float du = scrollU * time;
    float dv = scrollV * time;
    du -= std::floor(du);
    dv -= std::floor(dv);
    return { u0 + du, v0 + dv, u1 + du, v1 + dv };
}

} // namespace dx12e
