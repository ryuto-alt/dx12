#pragma once
// ---------------------------------------------------------------------------
// BlendTree — ブレンドツリーの重み計算（純関数）。
//
// 1D: ソート済みサンプル列に対する区間線形補間。
// 2D: Gradient Band Interpolation（Cartesian / Polar）。
//     出典: Rune Skovbo Johansen, "Automated Semi-Procedural Animation for
//     Character Locomotion", Aarhus University, §6.3 / §6.3.1（式 6.6/6.7, 6.14/6.15）。
//     Unity の "Freeform Cartesian" / "Freeform Directional" はこの論文の実装。
//
//     h_i(p) = min over j != i of ( 1 - dot(vec(p_i→p), vec(p_i→p_j)) / |vec(p_i→p_j)|² )
//     w_i(p) = h_i(p) / Σ h_j(p)
//
//     論文は h を「重み関数の非ゼロ領域」として扱っており、実装では
//     **h_i = max(h_i, 0) にクランプしてから正規化する**必要がある
//     （クランプ無しだと Σ が 0 や負になって破綻する）。Σh == 0 のときは最近傍に 1.0。
//
// 2D は .animfsm からはまだ参照していない（計画05 Step 7）。式は裏取り済みで
// テストもあるので、使うときに調べ直さなくて済む。
//
// 依存は STL + cmath だけ。tests/ から直接ビルドして検証できる。
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstddef>
#include "core/Types.h"

namespace dx12e
{

struct BlendPoint2D
{
    f32 x = 0.0f;
    f32 y = 0.0f;
};

// ---------------------------------------------------------------------------
// 1D — 区間線形補間
//
// values は**昇順にソート済み**であること。x を挟む 2 点 values[k] <= x < values[k+1] に対して
//   w[k+1] = (x - values[k]) / (values[k+1] - values[k]),  w[k] = 1 - w[k+1]
// 範囲外は端でクランプ（端のサンプルが重み 1）。それ以外の重みは 0。
// 重みの合計は必ず 1（count >= 1 のとき）。count == 0 なら何も書かない。
//
// 上の Cartesian Gradient Band に 1D の点列を入れると、この区間線形補間と一致する
// （blend_tree_test.cpp で確認している）。
// ---------------------------------------------------------------------------
inline void Eval1DBlend(const f32* values, size_t count, f32 x, f32* outWeights)
{
    if (count == 0 || !outWeights) return;
    for (size_t i = 0; i < count; ++i) outWeights[i] = 0.0f;

    if (count == 1) { outWeights[0] = 1.0f; return; }

    if (x <= values[0])            { outWeights[0] = 1.0f; return; }
    if (x >= values[count - 1])    { outWeights[count - 1] = 1.0f; return; }

    for (size_t i = 0; i + 1 < count; ++i)
    {
        const f32 a = values[i];
        const f32 b = values[i + 1];
        if (x < a || x > b) continue;

        const f32 span = b - a;
        if (span <= 0.0f)   // 同じ値のサンプルが並んでいる（不正データ）
        {
            outWeights[i] = 1.0f;
            return;
        }
        const f32 t = (x - a) / span;
        outWeights[i]     = 1.0f - t;
        outWeights[i + 1] = t;
        return;
    }
    // ここには来ない（上のクランプで拾いきる）が、念のため最近傍へ
    outWeights[count - 1] = 1.0f;
}

// ---------------------------------------------------------------------------
// 2D — Cartesian Gradient Band Interpolation（論文 式 6.6 / 6.7）
// ---------------------------------------------------------------------------
inline void GradientBandCartesian(const BlendPoint2D* points, size_t n,
                                  BlendPoint2D p, f32* outWeights)
{
    if (n == 0 || !outWeights) return;
    if (n == 1) { outWeights[0] = 1.0f; return; }

    f32 total = 0.0f;
    for (size_t i = 0; i < n; ++i)
    {
        f32 h = 1.0f;   // j が 1 つも無い場合の上限
        const f32 pix = p.x - points[i].x;
        const f32 piy = p.y - points[i].y;

        for (size_t j = 0; j < n; ++j)
        {
            if (j == i) continue;
            const f32 ijx = points[j].x - points[i].x;
            const f32 ijy = points[j].y - points[i].y;
            const f32 len2 = ijx * ijx + ijy * ijy;
            if (len2 <= 1e-12f) continue;   // 同一点のサンプル

            // 標準的なベクトル射影から「vec(p_i→p_j) を掛けない」スカラー
            const f32 proj = (pix * ijx + piy * ijy) / len2;
            h = (std::min)(h, 1.0f - proj);
        }

        // 負にクランプしてから正規化する（論文の「非ゼロ領域」の扱い）
        outWeights[i] = (std::max)(h, 0.0f);
        total += outWeights[i];
    }

    if (total > 1e-8f)
    {
        const f32 inv = 1.0f / total;
        for (size_t i = 0; i < n; ++i) outWeights[i] *= inv;
        return;
    }

    // Σh == 0 のフォールバック: 最近傍 1 個に 1.0
    size_t best = 0;
    f32 bestD2 = 3.4e38f;
    for (size_t i = 0; i < n; ++i)
    {
        const f32 dx = p.x - points[i].x, dy = p.y - points[i].y;
        const f32 d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
        outWeights[i] = 0.0f;
    }
    outWeights[best] = 1.0f;
}

// ---------------------------------------------------------------------------
// 2D — Polar Gradient Band Interpolation（論文 式 6.14 / 6.15）
//
// ベクトルの定義だけを差し替える:
//   vec(p_i→p_j) = ( (|p_j| - |p_i|) / ((|p_j| + |p_i|)/2) , angle(p_j, p_i) * alpha )
//   vec(p_i→p)   = ( (|p|   - |p_i|) / ((|p_j| + |p_i|)/2) , angle(p,   p_i) * alpha )
// 分母は**どちらもペアごとの平均長**であることに注意（p 側も p_j との平均を使う）。
// alpha = 2 が論文の推奨値（補間は大きさより方向に 2 倍影響される。角度はラジアン）。
//
// 制約: 隣接サンプルが 180 度以上離れていると補間は ill-defined（論文 p.63 図6.12）。
// ---------------------------------------------------------------------------
inline void GradientBandPolar(const BlendPoint2D* points, size_t n,
                              BlendPoint2D p, f32 alpha, f32* outWeights)
{
    if (n == 0 || !outWeights) return;
    if (n == 1) { outWeights[0] = 1.0f; return; }

    auto len   = [](BlendPoint2D v) { return std::sqrt(v.x * v.x + v.y * v.y); };
    // a から b への符号付き角度（ラジアン、-pi..pi）。どちらかが 0 長なら 0。
    auto angle = [&](BlendPoint2D a, BlendPoint2D b) -> f32 {
        const f32 la = len(a), lb = len(b);
        if (la <= 1e-8f || lb <= 1e-8f) return 0.0f;
        const f32 dot   = a.x * b.x + a.y * b.y;
        const f32 cross = b.x * a.y - b.y * a.x;   // b → a の符号
        return std::atan2(cross, dot);
    };

    const f32 lp = len(p);

    f32 total = 0.0f;
    for (size_t i = 0; i < n; ++i)
    {
        const f32 li = len(points[i]);
        f32 h = 1.0f;

        for (size_t j = 0; j < n; ++j)
        {
            if (j == i) continue;
            const f32 lj = len(points[j]);
            const f32 avg = (lj + li) * 0.5f;
            if (avg <= 1e-8f) continue;

            const f32 ijMag = (lj - li) / avg;
            const f32 ijAng = angle(points[j], points[i]) * alpha;
            const f32 len2 = ijMag * ijMag + ijAng * ijAng;
            if (len2 <= 1e-12f) continue;

            const f32 ipMag = (lp - li) / avg;
            const f32 ipAng = angle(p, points[i]) * alpha;

            const f32 proj = (ipMag * ijMag + ipAng * ijAng) / len2;
            h = (std::min)(h, 1.0f - proj);
        }

        outWeights[i] = (std::max)(h, 0.0f);
        total += outWeights[i];
    }

    if (total > 1e-8f)
    {
        const f32 inv = 1.0f / total;
        for (size_t i = 0; i < n; ++i) outWeights[i] *= inv;
        return;
    }

    size_t best = 0;
    f32 bestD2 = 3.4e38f;
    for (size_t i = 0; i < n; ++i)
    {
        const f32 dx = p.x - points[i].x, dy = p.y - points[i].y;
        const f32 d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
        outWeights[i] = 0.0f;
    }
    outWeights[best] = 1.0f;
}

} // namespace dx12e
