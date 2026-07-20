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

// 再生モード。animMode としてコンポーネントに保存する（既定=ループ）。
enum FlipbookMode
{
    kFlipbookLoop     = 0,   // 0,1,2,..,n-1,0,1,.. と無限ループ
    kFlipbookOnce     = 1,   // 最終フレームで停止（爆発・被弾など単発演出）
    kFlipbookPingPong = 2,   // 0..n-1..0 と往復（呼吸・アイドル）
};

// 経過時間 → フレーム番号。モードに応じて [0, animFrames) に畳む。
// animFrames>0 / 正しい fps は呼び出し側で保証（fps<=0 は 8 とみなす）。
inline int ComputeFlipbookFrame(int animFrames, float animFps, int animMode, float time)
{
    const float fps = (animFps > 0.0f) ? animFps : 8.0f;
    const long long idx = static_cast<long long>(std::floor(time * fps));

    if (animMode == kFlipbookOnce)
    {
        if (idx <= 0) return 0;
        return (idx >= animFrames) ? (animFrames - 1) : static_cast<int>(idx);
    }

    if (animMode == kFlipbookPingPong && animFrames > 1)
    {
        // 往復の周期は 2n-2（両端のフレームが2回連続で出ないようにする）
        const long long period = 2LL * animFrames - 2;
        long long p = idx % period;
        if (p < 0) p += period;                       // 負の時刻も周期内へ折り返す
        return static_cast<int>((p < animFrames) ? p : (period - p));
    }

    long long p = idx % animFrames;
    if (p < 0) p += animFrames;                       // 負の時刻でも [0, frames) に収める
    return static_cast<int>(p);
}

// フリップブック: テクスチャを cols x rows グリッドとみなし、animMode に従って選んだフレームの
// セル UV を返す（ユーザー指定の uvMin/uvMax は無視）。
//  - animFrames : 総フレーム数（>0 で有効。呼び出し側で保証）
//  - animFps    : 再生速度（<=0 は 8 とみなす）
//  - animCols   : シートの列数（<=0 は animFrames = 横1行ストリップ）
//  - animRow    : シート内の開始行（複数アニメを行で並べたシート用）
//  - animRows   : シートの総行数（<=0 は自動 = animRow + ceil(animFrames/cols)。
//                 アニメ行の下に別アニメ行が続くシートでは明示指定する）
//  - animMode   : FlipbookMode（ループ/単発/往復）
inline SpriteUvRect ComputeFlipbookUvEx(int animFrames, float animFps, int animCols,
                                        int animRow, int animRows, int animMode, float time)
{
    const int cols = (animCols > 0) ? animCols : animFrames;

    const int frame = ComputeFlipbookFrame(animFrames, animFps, animMode, time);

    const int animRowSpan = (animFrames + cols - 1) / cols;   // このアニメが占める行数
    const int rows = (animRows > 0) ? animRows : (animRow + animRowSpan);

    const int col = frame % cols;
    const int row = animRow + frame / cols;

    const float cw = 1.0f / static_cast<float>(cols);
    const float ch = 1.0f / static_cast<float>(rows);
    return { col * cw, row * ch, (col + 1) * cw, (row + 1) * ch };
}

inline SpriteUvRect ComputeFlipbookUv(int animFrames, float animFps, int animCols,
                                      int animRow, int animRows, float time)
{
    return ComputeFlipbookUvEx(animFrames, animFps, animCols, animRow, animRows,
                               kFlipbookLoop, time);
}

// once モードの再生が終わっているか（イベント発火/自動非表示の判定用）。
inline bool IsFlipbookFinished(int animFrames, float animFps, int animMode, float time)
{
    if (animMode != kFlipbookOnce || animFrames <= 0) return false;
    const float fps = (animFps > 0.0f) ? animFps : 8.0f;
    return (time * fps) >= static_cast<float>(animFrames);
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
