#pragma once

// ===== シーントランジションのサムネイル =====
//
// プリセットのタイルに「文字」しか無いと、押してみるまでどんな切り替わり方か
// 分からない。ここでは小さな見本画（空 / 地面 / 建物 / 太陽）の上に、その型の
// 暗幕を「途中まで閉じた状態」で描く＝形がそのまま見える。
//
// ★見本画は PostPresetSwatch.h のものを使い回す（素の PostProcessSettings を渡すと
//   味付け無しの絵になる）。ポストのプリセットとトランジションのプリセットで
//   同じ絵を覆うので、タイルが並んだときに見比べやすい。
//
// ★暗幕の式は shaders/post/Transition.hlsl の TransPS と一対一で対応させること。
//   片方だけ直すと「サムネイルと実物が違う」になる。対応表は Transition.hlsl の先頭。
//   ただし HLSL は 1 ピクセルずつ判定するのに対し、ここは ImDrawList の矩形・円・
//   三角形で同じ領域を塗る（テクスチャも GPU リソースも増やさない）。
//
// 描画はすべて ImDrawList。オフスクリーン RT もシェーダーの再実行も要らない。

#include <cmath>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include "editor/PostPresetSwatch.h"   // 見本画（postswatch::DrawSwatch）
#include "renderer/SceneTransition.h"

namespace dx12e
{
namespace transwatch
{

// ブラインド(Blinds)の帯の本数。Transition.hlsl の kBlindCount と同じ値にすること。
inline constexpr int kBlindCount = 8;

// サムネイルで見せる被覆率。0 だと何も起きていない絵、1 だとただの黒面になるので、
// 「形が一番よく分かる途中」を出す。
inline constexpr float kSwatchProgress = 0.55f;

// [a,b] の外側を塗り潰して、半径 r の円の内側だけ残す（アイリス用）。
// 太い輪を 1 本描くだけで「円の外を覆う」が作れる（PostPresetSwatch の魚眼と同じ手）。
inline void CoverOutsideCircle(ImDrawList* dl, ImVec2 c, float r, float span, ImU32 col)
{
    if (r < 0.0f) r = 0.0f;
    dl->AddCircle(c, r + span * 0.5f, col, 64, span);
}

// 中心からのマンハッタン距離が d を超える所を塗る（菱形アイリス用）。
// 円と違って 1 本の図形では作れないので、横帯に割って左右を塗る。
inline void CoverOutsideDiamond(ImDrawList* dl, ImVec2 a, ImVec2 b, float d, ImU32 col)
{
    const float w = b.x - a.x, h = b.y - a.y;
    const float cx = a.x + w * 0.5f, cy = a.y + h * 0.5f;
    const float band = 2.0f;
    for (float y = a.y; y < b.y; y += band)
    {
        const float y1   = (std::min)(y + band, b.y);
        // 帯の中で菱形が一番細くなる側の行で判定する＝塗り残しを作らない
        const float dy   = (std::max)(std::fabs(y - cy), std::fabs(y1 - cy));
        const float half = d - dy;                    // この行で見えている幅の半分
        if (half <= 0.0f)
        {
            dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, y1), col);
            continue;
        }
        dl->AddRectFilled(ImVec2(a.x, y), ImVec2((std::max)(a.x, cx - half), y1), col);
        dl->AddRectFilled(ImVec2((std::min)(b.x, cx + half), y), ImVec2(b.x, y1), col);
    }
}

// 12時から時計回りに angle ラジアンぶんの扇を塗る（時計ワイプ用）。
// progress が半周を超えると凹多角形になるので、扇形ではなく三角形の連結で描く。
inline void CoverClockSector(ImDrawList* dl, ImVec2 c, float radius, float angle, ImU32 col)
{
    if (angle <= 0.0f) return;
    const int   segs = (std::max)(3, static_cast<int>(angle / 0.15f) + 1);
    const float step = angle / static_cast<float>(segs);
    for (int k = 0; k < segs; ++k)
    {
        const float t0 = step * static_cast<float>(k);
        const float t1 = step * static_cast<float>(k + 1);
        // 12時起点・時計回り: x = sin(t), y = -cos(t)
        const ImVec2 p0(c.x + std::sin(t0) * radius, c.y - std::cos(t0) * radius);
        const ImVec2 p1(c.x + std::sin(t1) * radius, c.y - std::cos(t1) * radius);
        dl->AddTriangleFilled(c, p0, p1, col);
    }
}

// シークバー早送り（type 4）。閉じフェーズの途中を描く。
inline void DrawSeek(ImDrawList* dl, ImVec2 a, ImVec2 b, float progress, float total)
{
    const float w = b.x - a.x, h = b.y - a.y;
    const ImU32 navy = IM_COL32(4, 5, 12, 255);            // シェーダーの float3(0.016,0.020,0.048)
    const ImU32 gold = IM_COL32(255, 158, 38, 255);        // float3(1.0, 0.62, 0.15)

    // 暗幕。エッジは x = uv.x + (uv.y-0.5)*0.10 の等値線＝上が右、下が左へ寄る斜め。
    const float topX = a.x + w * (progress + 0.05f);
    const float botX = a.x + w * (progress - 0.05f);
    dl->AddQuadFilled(ImVec2(a.x, a.y), ImVec2(topX, a.y), ImVec2(botX, b.y), ImVec2(a.x, b.y), navy);

    // プレイヘッドのグロー（覆われた側へ尾を引く）
    for (int k = 1; k <= 3; ++k)
    {
        const float t  = static_cast<float>(k) * 0.02f;    // 尾の長さ（uv 比）
        const int   al = 150 / k;
        dl->AddQuadFilled(ImVec2(topX - w * t, a.y), ImVec2(topX, a.y),
                          ImVec2(botX, b.y), ImVec2(botX - w * t, b.y),
                          IM_COL32(255, 158, 38, al));
    }

    // ">>" チェビロン 2 枚がヘッドを追走する
    for (int k = 0; k < 2; ++k)
    {
        const float cx = a.x + w * (progress - 0.050f - 0.045f * static_cast<float>(k));
        const float cy = a.y + h * 0.5f;
        const float sz = h * 0.10f;
        const ImU32 cc = IM_COL32(255, 158, 38, k == 0 ? 235 : 150);
        dl->AddLine(ImVec2(cx - sz * 0.55f, cy - sz), ImVec2(cx, cy), cc, 1.6f);
        dl->AddLine(ImVec2(cx - sz * 0.55f, cy + sz), ImVec2(cx, cy), cc, 1.6f);
    }

    // シークバー（トラック + 充填 + ノブ）。未被覆側にも薄く重なる
    const float barY = a.y + h * 0.935f;
    dl->AddLine(ImVec2(a.x, barY), ImVec2(b.x, barY), IM_COL32(90, 90, 97, 217), 2.0f);
    dl->AddLine(ImVec2(a.x, barY), ImVec2(a.x + w * total, barY), gold, 2.0f);
    dl->AddCircleFilled(ImVec2(a.x + w * total, barY), 2.6f, IM_COL32(255, 190, 90, 255), 12);
}

// [a,b] の矩形へ、type の暗幕を progress ぶん閉じた状態で描く（見本画は描かない）。
inline void DrawCurtain(ImDrawList* dl, ImVec2 a, ImVec2 b, TransitionType type, float progress)
{
    const float w = b.x - a.x, h = b.y - a.y;
    if (w <= 2.0f || h <= 2.0f) return;

    const ImU32 black = IM_COL32(0, 0, 0, 255);
    const ImVec2 c(a.x + w * 0.5f, a.y + h * 0.5f);
    // アイリス / 菱形の「1.0 = 角に届く距離」。HLSL の maxd をピクセル空間へ置き換えたもの
    //（HLSL の aspect 補正は、uv をピクセルへ戻すと等方＝ただのピクセル距離になる）。
    const float maxRadius = std::sqrt(w * w + h * h) * 0.5f;
    const float maxManh   = (w + h) * 0.5f;

    switch (type)
    {
    case TransitionType::FadeBlack:
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, static_cast<int>(progress * 255.0f)));
        break;

    case TransitionType::FadeWhite:
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, static_cast<int>(progress * 255.0f)));
        break;

    case TransitionType::Wipe:
        dl->AddRectFilled(a, ImVec2(a.x + w * progress, b.y), black);
        break;

    case TransitionType::WipeVertical:
        dl->AddRectFilled(a, ImVec2(b.x, a.y + h * progress), black);
        break;

    case TransitionType::Circle:
        CoverOutsideCircle(dl, c, maxRadius * (1.0f - progress), (std::max)(w, h) * 2.0f, black);
        break;

    case TransitionType::Diamond:
        CoverOutsideDiamond(dl, a, b, maxManh * (1.0f - progress), black);
        break;

    case TransitionType::Blinds:
        for (int k = 0; k < kBlindCount; ++k)
        {
            const float y0 = a.y + h * (static_cast<float>(k) / static_cast<float>(kBlindCount));
            const float y1 = a.y + h * ((static_cast<float>(k) + progress) / static_cast<float>(kBlindCount));
            dl->AddRectFilled(ImVec2(a.x, y0), ImVec2(b.x, y1), black);
        }
        break;

    case TransitionType::RadialClock:
        // 半径は角まで届かせる（扇が矩形をはみ出す分はクリップで落ちる）
        CoverClockSector(dl, c, maxRadius * 2.0f, progress * 6.2831853f, black);
        break;

    case TransitionType::Seek:
        // total は「遷移全体の進行」。閉じフェーズの途中なので progress の半分。
        DrawSeek(dl, a, b, progress, progress * 0.5f);
        break;
    }
}

// [a,b] へプリセット 1 件のサムネイルを描く（見本画 + 暗幕）。
inline void DrawSwatch(ImDrawList* dl, ImVec2 a, ImVec2 b, TransitionType type,
                       float progress = kSwatchProgress)
{
    if (b.x - a.x <= 2.0f || b.y - a.y <= 2.0f) return;
    dl->PushClipRect(a, b, true);
    postswatch::DrawSwatch(dl, a, b, PostProcessSettings{});   // 味付け無しの見本画
    DrawCurtain(dl, a, b, type, progress);
    dl->PopClipRect();
}

} // namespace transwatch
} // namespace dx12e
