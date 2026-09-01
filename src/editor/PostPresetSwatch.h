#pragma once

// ===== ポストプロセス・プリセットのサムネイル =====
//
// プリセットのボタンに「文字」しか無かったので、押してみるまでどんな絵になるか
// 分からなかった。ここでは小さな見本画（空 / 地面 / 建物 / 太陽）に、そのプリセットの
// 設定を CPU で当てて描く。
//
// ★実際のシーンを縮小レンダリングするのではなく、PostProcessSettings の値から描く。
//   プリセットの数値を直せばサムネイルも一緒に変わる＝説明が実装とズレない。
//   （本物のレンダリングにすると、プリセット 13 枚ぶんのオフスクリーン RT と
//     ポストパスの再実行が要る。見当をつけるだけの絵にそこまで払う価値はない）
//
// 描画は ImDrawList の矩形・円・線だけ。テクスチャも GPU リソースも増やさない。

#include <algorithm>
#include <cmath>
#include <cstdint>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include "renderer/PostProcessSettings.h"

namespace dx12e
{
namespace postswatch
{

struct Rgb { float r, g, b; };

inline float Luma(const Rgb& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
inline float Sat01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// 色まわりのエフェクトを、シェーダーとおおむね同じ順で当てる。
// （見当をつけるための絵なので、厳密な一致より「傾向が合っていること」を優先する）
inline Rgb ApplyColorOps(Rgb c, const PostProcessSettings& p)
{
    if (p.contrastOn)
    {
        c.r = (c.r - 0.5f) * p.contrast + 0.5f;
        c.g = (c.g - 0.5f) * p.contrast + 0.5f;
        c.b = (c.b - 0.5f) * p.contrast + 0.5f;
    }
    if (p.saturationOn)
    {
        const float l = Luma(c);
        c.r = l + (c.r - l) * p.saturation;
        c.g = l + (c.g - l) * p.saturation;
        c.b = l + (c.b - l) * p.saturation;
    }
    if (p.grayscaleOn)
    {
        const float l = Luma(c);
        c.r += (l - c.r) * p.grayscale;
        c.g += (l - c.g) * p.grayscale;
        c.b += (l - c.b) * p.grayscale;
    }
    if (p.sepiaOn)
    {
        const float l = Luma(c);
        const Rgb s{ l * 1.07f, l * 0.90f, l * 0.68f };
        c.r += (s.r - c.r) * p.sepia;
        c.g += (s.g - c.g) * p.sepia;
        c.b += (s.b - c.b) * p.sepia;
    }
    if (p.tintOn)
    {
        c.r *= p.tint.x; c.g *= p.tint.y; c.b *= p.tint.z;
    }
    if (p.warmthOn)
    {
        c.r += p.warmth * 0.12f;
        c.b -= p.warmth * 0.12f;
    }
    if (p.posterizeOn && p.posterize > 1)
    {
        const float n = static_cast<float>(p.posterize);
        c.r = std::floor(Sat01(c.r) * n) / (n - 1.0f);
        c.g = std::floor(Sat01(c.g) * n) / (n - 1.0f);
        c.b = std::floor(Sat01(c.b) * n) / (n - 1.0f);
    }
    return c;
}

inline ImU32 ToU32(Rgb c, float alpha = 1.0f)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(Sat01(c.r), Sat01(c.g), Sat01(c.b), alpha));
}

// 見本画の素の色（u,v は 0..1、v は上が 0）。
inline Rgb SceneColor(float u, float v)
{
    const float kHorizon = 0.62f;

    // 建物のシルエット（左寄り）
    if (u > 0.10f && u < 0.36f && v > 0.28f && v < kHorizon) return { 0.17f, 0.18f, 0.23f };
    if (u > 0.40f && u < 0.52f && v > 0.42f && v < kHorizon) return { 0.22f, 0.21f, 0.24f };

    // 太陽（明るい＝ブルームが乗る所）
    const float dx = (u - 0.74f), dy = (v - 0.20f) * 0.62f;
    if (dx * dx + dy * dy < 0.0075f) return { 1.6f, 1.5f, 1.25f };

    if (v < kHorizon)
    {
        // 空（上ほど濃い青）
        const float t = v / kHorizon;
        return { 0.24f + t * 0.50f, 0.40f + t * 0.40f, 0.72f + t * 0.16f };
    }
    // 地面（奥ほど明るい＝手前へ向かって暗く）
    const float t = (v - kHorizon) / (1.0f - kHorizon);
    return { 0.34f - t * 0.14f, 0.36f - t * 0.14f, 0.27f - t * 0.11f };
}

// 決定的な擬似乱数（グレイン用。フレームごとに散らつかせない）
inline float Hash01(int i)
{
    uint32_t x = static_cast<uint32_t>(i) * 2654435761u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
    return static_cast<float>(x & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
}

// [a,b] の矩形へプリセット 1 件のサムネイルを描く。
inline void DrawSwatch(ImDrawList* dl, ImVec2 a, ImVec2 b, const PostProcessSettings& p)
{
    const float w = b.x - a.x, h = b.y - a.y;
    if (w <= 2.0f || h <= 2.0f) return;

    dl->PushClipRect(a, b, true);

    // ---- 線画モードは絵を捨てて輪郭だけになるので、最初に分岐する ----
    if (p.outlineOn && p.outlineOnly)
    {
        const Rgb bg{ p.outlineBg.x, p.outlineBg.y, p.outlineBg.z };
        const Rgb ln{ p.outlineColor.x, p.outlineColor.y, p.outlineColor.z };
        dl->AddRectFilled(a, b, ToU32(bg));
        const float t = 1.0f + p.outlineThickness * 0.6f;
        const ImU32 lc = ToU32(ln);
        dl->AddLine(ImVec2(a.x, a.y + h * 0.62f), ImVec2(b.x, a.y + h * 0.62f), lc, t);
        dl->AddRect(ImVec2(a.x + w * 0.10f, a.y + h * 0.28f),
                    ImVec2(a.x + w * 0.36f, a.y + h * 0.62f), lc, 0.0f, 0, t);
        dl->AddRect(ImVec2(a.x + w * 0.40f, a.y + h * 0.42f),
                    ImVec2(a.x + w * 0.52f, a.y + h * 0.62f), lc, 0.0f, 0, t);
        dl->AddCircle(ImVec2(a.x + w * 0.74f, a.y + h * 0.20f), h * 0.11f, lc, 20, t);
        dl->PopClipRect();
        return;
    }

    // ---- 下地 ----
    if (p.pixelizeOn && p.pixelSize > 1.0f)
    {
        // ピクセル化: 見本画をブロック単位でサンプルして塗る（そのままドット絵に見える）
        const float block = std::max<float>(3.0f, std::min<float>(16.0f, p.pixelSize * 1.6f));
        for (float y = a.y; y < b.y; y += block)
            for (float x = a.x; x < b.x; x += block)
            {
                const float u = (x + block * 0.5f - a.x) / w;
                const float v = (y + block * 0.5f - a.y) / h;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min<float>(x + block, b.x), std::min<float>(y + block, b.y)),
                                  ToU32(ApplyColorOps(SceneColor(u, v), p)));
            }
    }
    else
    {
        const float hy = a.y + h * 0.62f;
        const Rgb skyTop = ApplyColorOps(SceneColor(0.5f, 0.0f), p);
        const Rgb skyBot = ApplyColorOps(SceneColor(0.5f, 0.60f), p);
        const Rgb grdTop = ApplyColorOps(SceneColor(0.5f, 0.64f), p);
        const Rgb grdBot = ApplyColorOps(SceneColor(0.5f, 1.0f), p);
        dl->AddRectFilledMultiColor(a, ImVec2(b.x, hy),
                                    ToU32(skyTop), ToU32(skyTop), ToU32(skyBot), ToU32(skyBot));
        dl->AddRectFilledMultiColor(ImVec2(a.x, hy), b,
                                    ToU32(grdTop), ToU32(grdTop), ToU32(grdBot), ToU32(grdBot));

        const Rgb bld1 = ApplyColorOps(SceneColor(0.20f, 0.40f), p);
        const Rgb bld2 = ApplyColorOps(SceneColor(0.46f, 0.50f), p);
        dl->AddRectFilled(ImVec2(a.x + w * 0.10f, a.y + h * 0.28f),
                          ImVec2(a.x + w * 0.36f, hy), ToU32(bld1));
        dl->AddRectFilled(ImVec2(a.x + w * 0.40f, a.y + h * 0.42f),
                          ImVec2(a.x + w * 0.52f, hy), ToU32(bld2));

        const Rgb sun = ApplyColorOps(SceneColor(0.74f, 0.20f), p);
        dl->AddCircleFilled(ImVec2(a.x + w * 0.74f, a.y + h * 0.20f), h * 0.11f, ToU32(sun), 20);
    }

    // ---- ブルーム: 太陽のまわりに滲みを重ねる ----
    if (p.bloomOn && p.bloom > 0.01f)
    {
        const ImVec2 c(a.x + w * 0.74f, a.y + h * 0.20f);
        const Rgb glow = ApplyColorOps({ 1.0f, 0.96f, 0.85f }, p);
        for (int i = 3; i >= 1; --i)
            dl->AddCircleFilled(c, h * (0.11f + 0.09f * i),
                                ToU32(glow, std::min<float>(0.30f, p.bloom * 0.16f)), 24);
    }

    // ---- 水中のゆらぎ ----
    if (p.waveOn && p.waveAmp > 0.0f)
    {
        const ImU32 col = IM_COL32(255, 255, 255, 40);
        for (int i = 0; i < 3; ++i)
        {
            const float y = a.y + h * (0.25f + 0.22f * i);
            ImVec2 pts[9];
            for (int k = 0; k < 9; ++k)
                pts[k] = ImVec2(a.x + w * (k / 8.0f),
                                y + std::sin(k * 0.9f + i * 1.7f) * h * 0.035f);
            dl->AddPolyline(pts, 9, col, 0, 1.5f);
        }
    }

    // ---- グリッチ: 横帯をずらして RGB を分離する ----
    if (p.glitchOn && p.glitch > 0.01f)
    {
        for (int i = 0; i < 3; ++i)
        {
            const float y  = a.y + h * (0.22f + 0.26f * i);
            const float bh = h * 0.07f;
            const float ox = w * (0.06f + 0.05f * i) * (i == 1 ? -1.0f : 1.0f) * p.glitch;
            dl->AddRectFilled(ImVec2(a.x + ox, y), ImVec2(b.x + ox, y + bh),
                              IM_COL32(255, 40, 90, 90));
            dl->AddRectFilled(ImVec2(a.x - ox, y + bh * 0.4f), ImVec2(b.x - ox, y + bh * 1.4f),
                              IM_COL32(40, 220, 255, 80));
        }
    }

    // ---- 色収差: 縁に赤/シアンのズレを置く ----
    if (p.chromaticOn && p.chromatic > 0.01f)
    {
        const float o = std::min<float>(4.0f, 1.0f + p.chromatic * 5.0f);
        const int   al = static_cast<int>(std::min<float>(150.0f, 50.0f + p.chromatic * 160.0f));
        dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + o, b.y), IM_COL32(255, 60, 60, al));
        dl->AddRectFilled(ImVec2(b.x - o, a.y), ImVec2(b.x, b.y), IM_COL32(60, 200, 255, al));
    }

    // ---- 走査線 ----
    if (p.scanlineOn && p.scanline > 0.01f)
    {
        const int al = static_cast<int>(std::min<float>(200.0f, p.scanline * 230.0f));
        for (float y = a.y; y < b.y; y += 3.0f)
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(0, 0, 0, al), 1.0f);
    }

    // ---- グレイン（粒子）----
    if (p.grainOn && p.grain > 0.01f)
    {
        const int n  = static_cast<int>(40 + p.grain * 220.0f);
        const int al = static_cast<int>(std::min<float>(190.0f, 70.0f + p.grain * 190.0f));
        const float s = std::max<float>(1.0f, p.grainSize * 0.8f);
        for (int i = 0; i < n; ++i)
        {
            const float x = a.x + Hash01(i * 3 + 1) * w;
            const float y = a.y + Hash01(i * 3 + 2) * h;
            const int   v = (Hash01(i * 3 + 3) > 0.5f) ? 255 : 0;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + s, y + s), IM_COL32(v, v, v, al));
        }
    }

    // ---- 魚眼 / レンズ歪み: 円の外を落とす（内接円の外側を太い輪で塗り潰す）----
    if (p.lensOn && p.lens > 0.05f && p.lensCircular)
    {
        const ImVec2 c(a.x + w * 0.5f, a.y + h * 0.5f);
        const float  r = h * 0.5f * (1.0f - std::min<float>(0.35f, p.lens * 0.28f));
        const float  t = std::max<float>(w, h);
        dl->AddCircle(c, r + t * 0.5f, IM_COL32(0, 0, 0, 255), 48, t);
    }

    // ---- ビネット（周辺減光）: 四辺のグラデーションで近似 ----
    if (p.vignetteOn && p.vignette > 0.01f)
    {
        const Rgb vc{ p.vignetteColor.x, p.vignetteColor.y, p.vignetteColor.z };
        const ImU32 solid = ToU32(vc, std::min<float>(0.85f, p.vignette * 0.8f));
        const ImU32 clear = ToU32(vc, 0.0f);
        // 半径が小さいほど内側まで食い込む。★上限を切らないと四辺の帯が中央で重なって
        //   全体が真っ暗になり、控えめなプリセット（シネマ）まで強烈に見えてしまう。
        const float band = std::min<float>(0.30f,
                           std::max<float>(0.10f, 0.42f - p.vignetteRadius * 0.30f));
        const float bx = w * band, by = h * band;
        dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + by), solid, solid, clear, clear);
        dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - by), b, clear, clear, solid, solid);
        dl->AddRectFilledMultiColor(a, ImVec2(a.x + bx, b.y), solid, clear, clear, solid);
        dl->AddRectFilledMultiColor(ImVec2(b.x - bx, a.y), b, clear, solid, solid, clear);
    }

    // ---- 輪郭線（下地を残すモード）----
    if (p.outlineOn && !p.outlineOnly)
    {
        const Rgb ln{ p.outlineColor.x, p.outlineColor.y, p.outlineColor.z };
        const float t = 1.0f + p.outlineThickness * 0.5f;
        dl->AddRect(ImVec2(a.x + w * 0.10f, a.y + h * 0.28f),
                    ImVec2(a.x + w * 0.36f, a.y + h * 0.62f), ToU32(ln), 0.0f, 0, t);
    }

    dl->PopClipRect();
}

} // namespace postswatch
} // namespace dx12e
