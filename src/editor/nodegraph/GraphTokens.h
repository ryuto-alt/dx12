#pragma once

// ===== ノードグラフの見た目トークン（theme:: から導出）=====
// 色・グロー量は全部ここで theme:: のトークン（EditorTheme.h）と Deco（ThemeVariants.h）から作る。GraphView は 16 進値を持たない。
// アイデンティティ案（--theme-variant a|b|c）を差し替えるとグラフ全体の色が追従する。
// 寸法は GraphLayout の論理単位（GraphView が ui::Px / zoom を掛ける）なのでここには持たない。

#include "editor/EditorTheme.h"
#include "editor/nodegraph/GraphTypes.h"

namespace dx12e::ng
{

struct GraphTokens
{
    ImU32 canvasBg, canvasBgEdge, gridMinor, gridMajor, gridAxis;
    ImU32 nodeBody, nodeBodyHover, nodeBorder, nodeBorderHover, nodeInner, nodeShadow;
    ImU32 text, textMid, textDim, textFaint, onHeader;
    ImU32 slotBg, slotBorder, slotBorderHover, slotText;
    ImU32 accent, accentHover, good, warn, bad;
    ImU32 headerCat[kCategorySlots];       // ヘッダの面
    ImU32 headerCatBright[kCategorySlots]; // ヘッダの上端ハイライト / ミニマップ
    ImU32 pinType[kPinTypeSlots];          // ピン・ワイヤの型色
    ImU32 commentFill[kCommentColors], commentBorder[kCommentColors], commentTitle[kCommentColors];
    ImU32 popupBg, popupBorder;
    float glow = 0.0f;                     // 0 = グロー無し（案 C 等）。選択・ホバー・ワイヤの発光の強さ
    ImVec4 accentV;                        // ImGui へ渡す用
};

namespace tokendetail
{
inline ImVec4 Lerp(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
}
inline ImU32 U(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImU32 UA(const ImVec4& c, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, a)); }
} // namespace tokendetail

inline GraphTokens MakeGraphTokens()
{
    using namespace tokendetail;
    GraphTokens t;
    const ImVec4 white(1, 1, 1, 1), black(0, 0, 0, 1);

    const ImVec4 bg0 = theme::Bg0, bg1 = theme::Bg1, bg2 = theme::Bg2, bg3 = theme::Bg3;
    t.canvasBg     = U(Lerp(bg0, black, 0.18f));
    t.canvasBgEdge = U(Lerp(bg0, black, 0.42f));
    t.gridMinor    = UA(Lerp(bg1, theme::Text, 0.10f), 0.55f);
    t.gridMajor    = UA(Lerp(bg1, theme::Text, 0.20f), 0.80f);
    t.gridAxis     = UA(Lerp(bg1, theme::Text, 0.32f), 0.90f);

    t.nodeBody        = U(Lerp(bg2, bg1, 0.10f));
    t.nodeBodyHover   = U(Lerp(bg2, bg3, 0.55f));
    t.nodeBorder      = UA(Lerp(theme::BorderStrong, theme::Border, 0.35f), 1.0f);
    t.nodeBorderHover = U(Lerp(theme::BorderStrong, theme::Text, 0.28f));
    t.nodeInner       = U(Lerp(bg1, bg0, 0.55f));
    t.nodeShadow      = UA(black, 0.34f);

    t.text     = U(theme::Text);
    t.textMid  = U(theme::TextMid);
    t.textDim  = U(theme::TextDim);
    t.textFaint = U(theme::TextFaint);
    t.onHeader = U(Lerp(theme::Text, white, 0.5f));

    t.slotBg          = U(theme::InputBg);
    t.slotBorder      = U(theme::InputBorder);
    t.slotBorderHover = U(theme::InputBorderHover);
    t.slotText        = U(theme::Text);

    t.accent      = U(theme::Accent);
    t.accentHover = U(theme::AccentHover);
    t.good = U(theme::Good); t.warn = U(theme::Warn); t.bad = U(theme::Bad);
    t.accentV = theme::Accent;

    // カテゴリ色: 種別色（Type*）を再利用する。定数=琥珀 / パラメータ=マゼンタ / テクスチャ=ティール / 座標=青 /
    // 数学=紫 / ベクトル=緑 / 色=橙 / ユーティリティ=灰 / その他=中性 / 出力=アクセント。
    const ImVec4 catBase[kCategorySlots] = {
        theme::TypeLight, theme::TypeUi, theme::TypeAudio, theme::TypeCamera, theme::TypeScript,
        theme::TypePhysics, theme::TypeScene, theme::TypeMesh, theme::TypeEmpty, theme::Accent};
    for (int i = 0; i < kCategorySlots; ++i)
    {
        t.headerCat[i]       = U(Lerp(bg2, catBase[i], 0.46f));
        t.headerCatBright[i] = U(Lerp(catBase[i], white, 0.10f));
    }

    // ピンの型色（色 + 形の二重符号化。形は IGraphModel::PinTypeInfo の shape）
    const ImVec4 pinBase[kPinTypeSlots] = {
        theme::TypePhysics,   // 0 Float
        theme::TypeAudio,     // 1 Vec2
        theme::TypeLight,     // 2 Vec3
        theme::TypeUi,        // 3 Vec4
        theme::TypeScene,     // 4 Texture
        theme::TypeScript,    // 5 Bool
        theme::TypeMesh,      // 6 Any（多相）
        theme::TextDim,       // 7 Wildcard
        theme::TypeCamera};   // 8 Int（マテリアルグラフの整数）
    for (int i = 0; i < kPinTypeSlots; ++i) t.pinType[i] = U(pinBase[i]);

    // コメント色 6 種
    const ImVec4 cm[kCommentColors] = {theme::Accent, theme::TypeAudio, theme::TypePhysics, theme::TypeLight, theme::TypeScene, theme::TypeUi};
    for (int i = 0; i < kCommentColors; ++i)
    {
        t.commentFill[i]   = UA(cm[i], 0.085f);
        t.commentBorder[i] = UA(cm[i], 0.55f);
        t.commentTitle[i]  = UA(Lerp(bg1, cm[i], 0.34f), 0.92f);
    }
    t.popupBg = U(theme::Bg2);
    t.popupBorder = U(theme::BorderStrong);

    // グロー: 案の Deco::glow があればそれ。現行（Deco::glow=0）は控えめな既定 0.55、案 C のように「光らせない」案は 0。
    const float dg = theme::CurrentDeco().glow;
    t.glow = dg > 0.0f ? dg : (theme::IsDefaultVariant() ? 0.55f : 0.0f);
    return t;
}

} // namespace dx12e::ng
