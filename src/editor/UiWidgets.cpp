#include "editor/UiWidgets.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui_internal.h>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace dx12e::ui
{

namespace th = dx12e::theme;

namespace
{

ImU32 Col(const ImVec4& c) { return ImGui::GetColorU32(c); }
// alpha を掛けた色（装飾のフェード用）
ImU32 ColA(const ImVec4& c, float a)
{
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * std::min(1.0f, std::max(0.0f, a))));
}
ImVec4 Lerp4(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
const ImVec4 kWhite(1, 1, 1, 1);
const ImVec4 kClear(0, 0, 0, 0);

// これから出す入力部品の枠矩形（項目の右に文字ラベルが付く場合、GetItemRect はラベルまで含むので、
// 枠だけの矩形は出す前にカーソルと幅から求めておく）。
struct FrameRect
{
    ImVec2 mn, mx;
    static FrameRect Peek()
    {
        FrameRect f;
        f.mn = ImGui::GetCursorScreenPos();
        f.mx = ImVec2(f.mn.x + ImGui::CalcItemWidth(), f.mn.y + ImGui::GetFrameHeight());
        return f;
    }
};

// 直前の項目の枠を、状態に応じた色でなぞる。ホバー = 明るい枠 / アクティブ = アクセント枠。
// Default 以外の案では deco:: の遷移（イージング）+ グロー + 面のうっすらした色味で「案らしさ」を出す。
// id = 遷移状態のキー（0 なら直前の項目の ID）。BeginCombo のようにポップアップを開くと直前の項目が変わる部品は明示する。
void OutlineRect(ImVec2 mn, ImVec2 mx, bool hovered, bool active, float rounding, ImGuiID id = 0)
{
    if (!deco::Active())
    {
        if (!hovered && !active) return;
        ImGui::GetWindowDrawList()->AddRect(
            mn, mx, Col(active ? th::Accent : th::InputBorderHover), rounding, 0, Px(1.0f));
        return;
    }
    if (id == 0) id = ImGui::GetItemID();
    const th::Deco& d = th::CurrentDeco();
    const float h = deco::Ease(id, 0, hovered ? 1.0f : 0.0f);
    const float a = deco::Ease(id, 1, active ? 1.0f : 0.0f);
    if (h <= 0.004f && a <= 0.004f) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float b1 = Px(1.0f);
    // 面: ホバー/アクティブでアクセントをうっすら足す（発光しているように見せる）
    if (d.hoverTint > 0.0f)
        dl->AddRectFilled(ImVec2(mn.x + b1, mn.y + b1), ImVec2(mx.x - b1, mx.y - b1),
                          ColA(th::Accent, d.hoverTint * std::max(h * 0.7f, a * 1.3f)), std::max(0.0f, rounding - b1));
    // 枠: ホバー（案ごとの色）→ アクティブ（アクセント）
    ImVec4 hoverEdge = th::InputBorderHover;
    if (th::CurrentVariant() == th::Variant::A) hoverEdge = th::WithAlpha(th::Accent, 0.50f);
    if (th::CurrentVariant() == th::Variant::B) hoverEdge = th::WithAlpha(th::Hex(0xC8D2FF), 0.32f);
    if (h > 0.004f && a < 0.999f)
        dl->AddRect(mn, mx, ColA(hoverEdge, h * (1.0f - a * 0.5f)), rounding, 0, b1);
    if (a > 0.004f)
    {
        dl->AddRect(mn, mx, ColA(th::Accent, a), rounding, 0, b1);
        deco::Glow(dl, mn, mx, rounding, th::Accent, a);
    }
}

// スライダー描画の共通処理。ImGui のスライダーは「溝 → つまみ → 値の文字」の順に描くので、
// つまみ(塗り)が文字に重なってしまう。値の文字だけをチャンネル 1（上）、溝と塗りをチャンネル 0（下）に
// 分けて描き、順序を「溝 → 塗り → 文字」に入れ替える。
// ※ テーブルの中でも安全: テーブルは自前の DrawSplitter を持ち、ここは別インスタンスを使う。
struct FillSlider
{
    ImDrawList*        dl = nullptr;
    ImDrawListSplitter sp;
    FrameRect          fr;

    void Begin()
    {
        fr = FrameRect::Peek();
        dl = ImGui::GetWindowDrawList();
        sp.Split(dl, 2);
        sp.SetCurrentChannel(dl, 1);
        const ImVec4 clear(0, 0, 0, 0);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        clear);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, clear);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  clear);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,       clear);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, clear);
        ImGui::PushStyleColor(ImGuiCol_Border,           clear);
        PushMono();
    }

    // t = 0..1 の塗り率。Begin 〜 スライダー呼び出し 〜 End の順。valueText = 表示中の値の文字列
    // （つまみの縦線が文字に重なるときは線を描かない）。
    void End(float t, const char* valueText)
    {
        const float textW = valueText ? ImGui::CalcTextSize(valueText).x : 0.0f;   // まだ等幅フォントの間に測る
        PopMono();
        ImGui::PopStyleColor(6);

        const ImVec2 mn = fr.mn;
        const ImVec2 mx = fr.mx;
        const bool hov = ImGui::IsItemHovered();
        const bool act = ImGui::IsItemActive();
        const float r  = ImGui::GetStyle().FrameRounding;

        sp.SetCurrentChannel(dl, 0);
        // 溝（入力欄と同じ凹み）
        dl->AddRectFilled(mn, mx, Col(act ? th::InputBgActive : hov ? th::InputBgHover : th::InputBg), r);
        t = std::min(1.0f, std::max(0.0f, t));
        const float b1px = Px(1.0f);
        const float x0 = mn.x + b1px, x1 = mx.x - b1px;
        const float fx = x0 + (x1 - x0) * t;
        const bool special = deco::Active();
        if (!special)
        {
            // 塗りバー（アクセント 40%）
            if (fx - x0 >= b1px)
            {
                dl->AddRectFilled(ImVec2(x0, mn.y + b1px), ImVec2(fx, mx.y - b1px),
                                  Col(th::WithAlpha(th::Accent, act ? 0.55f : 0.40f)),
                                  std::max(0.0f, r - b1px),
                                  t >= 0.999f ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
            }
        }
        else if (fx - x0 >= b1px)
        {
            // 案ごとの塗り: A = 先端に向かって濃くなるグラデ（ネオンの尾を引く）/ B = 上が明るいガラスの帯 / C = 無彩色の淡い帯 + 先端のシグナル
            const ImVec2 f0(x0, mn.y + b1px), f1(fx, mx.y - b1px);
            const float rr = std::max(0.0f, r - b1px);
            const ImDrawFlags fl = t >= 0.999f ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft;
            const th::Variant v = th::CurrentVariant();
            const float boost = act ? 1.35f : 1.0f;
            if (v == th::Variant::A)
            {
                // 丸みを保つため、まず薄い面（角丸）→ 先端寄りをグラデで重ねる（右端は角を丸めない矩形）
                dl->AddRectFilled(f0, f1, ColA(th::Accent, 0.13f * boost), rr, fl);
                const float gx0 = std::max(f0.x, f1.x - (f1.x - f0.x) * 0.75f);
                if (f1.x - gx0 > 1.0f)
                    dl->AddRectFilledMultiColor(ImVec2(gx0, f0.y), f1, ColA(th::Accent, 0.0f), ColA(th::Accent, 0.36f * boost),
                                                ColA(th::Accent, 0.36f * boost), ColA(th::Accent, 0.0f));
            }
            else if (v == th::Variant::B)
            {
                dl->AddRectFilled(f0, f1, ColA(th::Accent, 0.26f * boost), rr, fl);
                dl->AddRectFilledMultiColor(f0, ImVec2(f1.x, f0.y + (f1.y - f0.y) * 0.55f), ColA(kWhite, 0.10f), ColA(kWhite, 0.10f),
                                            ColA(kWhite, 0.0f), ColA(kWhite, 0.0f));
            }
            else
            {
                dl->AddRectFilled(f0, f1, ColA(kWhite, 0.11f * boost), rr, fl);
            }
        }
        // つまみ = 塗りの先端の細い縦線。値の文字に重なるときは描かない（読みやすさ優先）。
        {
            const float gx = std::min(x1 - b1px, std::max(x0 + b1px, fx));
            const float cx = (mn.x + mx.x) * 0.5f;
            const bool overText = std::fabs(gx - cx) < textW * 0.5f + Px(4.0f);
            if (!overText || act)
            {
                const ImVec4& tip = special ? th::Accent : (act ? th::Text : th::AccentHover);
                const ImVec4& tipCol = (special && act) ? th::AccentHover : tip;
                dl->AddRectFilled(ImVec2(gx - b1px, mn.y + Px(3.0f)), ImVec2(gx + b1px, mx.y - Px(3.0f)),
                                  special ? Col(tipCol) : Col(act ? th::Text : th::AccentHover), Px(1.0f));
                if (special && (fx - x0) >= b1px)
                    deco::Glow(dl, ImVec2(gx - b1px, mn.y + Px(3.0f)), ImVec2(gx + b1px, mx.y - Px(3.0f)), Px(1.0f), th::Accent,
                               act ? 0.9f : 0.45f);
            }
        }
        sp.Merge(dl);

        OutlineRect(mn, mx, hov, act, r);
        if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
};

float FracFloat(float v, float mn, float mx, ImGuiSliderFlags flags)
{
    if (!(mx > mn)) return 0.0f;
    if ((flags & ImGuiSliderFlags_Logarithmic) && mn > 0.0f && v > 0.0f)
        return std::log(v / mn) / std::log(mx / mn);
    return (v - mn) / (mx - mn);
}

} // namespace

// ---------------------------------------------------------------------------
// フォント
// ---------------------------------------------------------------------------
void PushBold() { ImGui::PushFont(th::g_fonts.bold, 0.0f); }
void PopBold()  { ImGui::PopFont(); }
void PushMono() { ImGui::PushFont(th::g_fonts.mono, 0.0f); }
void PopMono()  { ImGui::PopFont(); }

// ---------------------------------------------------------------------------
// アイコン
// ---------------------------------------------------------------------------
void Icon(const char* glyph, const ImVec4& color)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(glyph);
    ImGui::PopStyleColor();
}

void DrawIconCentered(ImDrawList* dl, const char* glyph, ImVec2 center, ImU32 col, float px)
{
    ImFont* font = ImGui::GetFont();
    if (px <= 0.0f) px = ImGui::GetFontSize();
    ImVec2 pos;
    bool placed = false;
    if (font)
    {
        unsigned int cp = 0;
        ImTextCharFromUtf8(&cp, glyph, nullptr);
        ImFontBaked* baked = font->GetFontBaked(px);
        const ImFontGlyph* g = baked ? baked->FindGlyph(static_cast<ImWchar>(cp)) : nullptr;
        if (g && g->Visible)
        {
            // グリフの実際の外形の中心を中心へ合わせる（ペン位置基準の縦横ずれを吸収）
            pos = ImVec2(center.x - (g->X0 + g->X1) * 0.5f, center.y - (g->Y0 + g->Y1) * 0.5f);
            placed = true;
        }
    }
    if (!placed)
    {
        const ImVec2 ts = ImGui::CalcTextSize(glyph);
        pos = ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f);
    }
    pos.x = std::floor(pos.x + 0.5f);
    pos.y = std::floor(pos.y + 0.5f);
    dl->AddText(font, px, pos, col, glyph);
}

namespace
{
// ツールバー等のフラットなアイコンボタンの面。Default は従来どおり。
//   A = ホバーでアクセントがにじむ / アクティブは面 + 下辺のネオンライン（にじみ付き）
//   B = ガラスの丸い面（上が明るい）+ 光の縁 / C = 面を塗らず、下辺のシグナルラインだけ（ホバーは平たい Bg3）
void ToolButtonFace(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool active, bool hov, bool held, const ImVec4& face, ImGuiID id)
{
    if (!deco::Active())
    {
        if (active)
            dl->AddRectFilled(mn, mx, Col(th::WithAlpha(face, held ? 0.32f : hov ? 0.28f : 0.22f)), Px(3.0f));
        else if (held)
            dl->AddRectFilled(mn, mx, Col(th::Bg4), Px(3.0f));
        else if (hov)
            dl->AddRectFilled(mn, mx, Col(th::Bg3), Px(3.0f));
        if (active)
            dl->AddRectFilled(ImVec2(mn.x + Px(5.0f), mx.y - Px(2.0f)), ImVec2(mx.x - Px(5.0f), mx.y), Col(face), Px(1.0f));
        return;
    }
    const th::Deco& d = th::CurrentDeco();
    const th::Variant v = th::CurrentVariant();
    const float h = deco::Ease(id, 0, hov ? 1.0f : 0.0f);
    const float a = deco::Ease(id, 1, active ? 1.0f : 0.0f);
    const float rr = Px(v == th::Variant::B ? 6.0f : v == th::Variant::C ? 2.0f : 4.0f);
    const float b1 = Px(1.0f);
    if (v == th::Variant::C)
    {
        if (held) dl->AddRectFilled(mn, mx, Col(th::Bg4), rr);
        else if (h > 0.004f) dl->AddRectFilled(mn, mx, ColA(th::Bg3, h), rr);
        if (a > 0.004f)
            dl->AddRectFilled(ImVec2(mn.x + Px(4.0f), mx.y - Px(2.0f)), ImVec2(mx.x - Px(4.0f), mx.y), ColA(face, a), 0.0f);
        return;
    }
    if (v == th::Variant::B)
    {
        // ガラスの面: ホバー = 白 6% + 縁 / アクティブ = アクセントの帯（上が明るい）+ 光の縁
        if (held) dl->AddRectFilled(mn, mx, Col(th::Bg4), rr);
        else if (h > 0.004f)
        {
            dl->AddRectFilled(mn, mx, ColA(kWhite, 0.06f * h), rr);
            dl->AddRect(mn, mx, ColA(kWhite, 0.10f * h), rr, 0, b1);
        }
        if (a > 0.004f)
        {
            dl->AddRectFilled(mn, mx, ColA(face, (held ? 0.30f : 0.22f) * a), rr);
            dl->AddRectFilledMultiColor(ImVec2(mn.x + rr * 0.5f, mn.y + b1), ImVec2(mx.x - rr * 0.5f, mn.y + (mx.y - mn.y) * 0.5f),
                                        ColA(kWhite, 0.13f * a), ColA(kWhite, 0.13f * a), ColA(kWhite, 0.0f), ColA(kWhite, 0.0f));
            dl->AddRect(mn, mx, ColA(face, 0.55f * a), rr, 0, b1);
        }
        return;
    }
    // A: ネオン・エッジ
    if (held) dl->AddRectFilled(mn, mx, Col(th::Bg4), rr);
    else if (h > 0.004f)
    {
        dl->AddRectFilled(mn, mx, ColA(th::Bg3, h), rr);
        dl->AddRectFilled(mn, mx, ColA(th::Accent, d.hoverTint * h), rr);
    }
    if (a > 0.004f)
    {
        dl->AddRectFilled(mn, mx, ColA(face, (held ? 0.26f : 0.18f) * a), rr);
        // 下辺のネオンライン + 上へにじむ光
        const float lx0 = mn.x + Px(5.0f), lx1 = mx.x - Px(5.0f);
        dl->AddRectFilledMultiColor(ImVec2(lx0, mx.y - Px(9.0f)), ImVec2(lx1, mx.y - Px(2.0f)),
                                    ColA(face, 0.0f), ColA(face, 0.0f), ColA(face, 0.30f * a), ColA(face, 0.30f * a));
        dl->AddRectFilled(ImVec2(lx0, mx.y - Px(2.0f)), ImVec2(lx1, mx.y), ColA(face, a), Px(1.0f));
    }
}
} // namespace

bool IconButton(const char* id, const char* glyph, const char* tooltip, bool active,
                const ImVec4* tint, float sizePx, float iconPx, const ImVec4* activeFace)
{
    if (sizePx <= 0.0f) sizePx = Px(th::size::kToolbarBtn);   // 0 以下 = 既定（論理 28px を倍率換算）
    const ImVec4& face = activeFace ? *activeFace : th::Accent;
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##ib", ImVec2(sizePx, sizePx));
    const bool hov  = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImGuiID bid = ImGui::GetItemID();
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ToolButtonFace(dl, mn, mx, active, hov, held, face, bid);

    ImVec4 ic = tint ? *tint : (active ? (activeFace ? *activeFace : th::AccentHover) : hov ? th::Text : th::TextDim);
    if (tint && !active && !hov) ic = th::Mul(*tint, 0.88f);
    DrawIconCentered(dl, glyph, ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f), Col(ic),
                     iconPx > 0.0f ? iconPx : Px(th::size::kIconPx + 2.0f));

    if (tooltip && *tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

bool IconDropdownButton(const char* id, const char* glyph, const char* tooltip, bool active, float sizePx)
{
    if (sizePx <= 0.0f) sizePx = Px(th::size::kToolbarBtn);
    const float w = sizePx + Px(14.0f);
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##idb", ImVec2(w, sizePx));
    const bool hov  = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImGuiID bid = ImGui::GetItemID();
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!deco::Active())
    {
        // 現行: 下辺のアクセントラインは付けない（IconButton と違い、面だけ）
        if (active)
            dl->AddRectFilled(mn, mx, Col(th::WithAlpha(th::Accent, held ? 0.32f : hov ? 0.28f : 0.22f)), Px(3.0f));
        else if (held)
            dl->AddRectFilled(mn, mx, Col(th::Bg4), Px(3.0f));
        else if (hov)
            dl->AddRectFilled(mn, mx, Col(th::Bg3), Px(3.0f));
    }
    else
        ToolButtonFace(dl, mn, mx, active, hov, held, th::Accent, bid);
    const ImVec4& ic = active ? th::AccentHover : (hov ? th::Text : th::TextDim);
    const float cy = (mn.y + mx.y) * 0.5f;
    DrawIconCentered(dl, glyph, ImVec2(mn.x + sizePx * 0.5f, cy), Col(ic), Px(th::size::kIconPx + 2.0f));
    DrawIconCentered(dl, ICON_CHEVRON_DOWN, ImVec2(mx.x - Px(9.0f), cy), Col(ic), Px(13.0f));
    if (tooltip && *tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

// ---------------------------------------------------------------------------
// ボタン
// ---------------------------------------------------------------------------
bool PrimaryButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button,        th::Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th::AccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  th::AccentPressed);
    ImGui::PushStyleColor(ImGuiCol_Text,          th::OnAccent);
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0, 0, 0, 0));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    if (deco::Active())
    {
        // A = ホバー/押下でネオンのにじみ / B = 上が明るいガラスの艶 + 光の縁 / C = 平たいシグナル塗り（装飾なし）
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float rr = ImGui::GetStyle().FrameRounding;
        const ImGuiID bid = ImGui::GetItemID();
        const float hov = deco::Ease(bid, 0, ImGui::IsItemHovered() ? 1.0f : 0.0f);
        if (th::CurrentVariant() == th::Variant::A)
            deco::Glow(dl, mn, mx, rr, th::Accent, 0.35f + 0.65f * hov);
        else if (th::CurrentVariant() == th::Variant::B)
        {
            dl->AddRectFilledMultiColor(ImVec2(mn.x + rr * 0.5f, mn.y + Px(1.0f)), ImVec2(mx.x - rr * 0.5f, mn.y + (mx.y - mn.y) * 0.55f),
                                        ColA(kWhite, 0.20f), ColA(kWhite, 0.20f), ColA(kWhite, 0.0f), ColA(kWhite, 0.0f));
            dl->AddRect(mn, mx, ColA(kWhite, 0.16f + 0.10f * hov), rr, 0, Px(1.0f));
        }
    }
    return r;
}

bool DangerButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button,        th::WithAlpha(th::Bad, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th::WithAlpha(th::Bad, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  th::WithAlpha(th::Bad, 0.34f));
    ImGui::PushStyleColor(ImGuiCol_Text,          th::Bad);
    ImGui::PushStyleColor(ImGuiCol_Border,        th::WithAlpha(th::Bad, 0.70f));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    return r;
}

// ---------------------------------------------------------------------------
// 入力
// ---------------------------------------------------------------------------
void OutlineLastItem()
{
    OutlineRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                ImGui::IsItemHovered(), ImGui::IsItemActive(), ImGui::GetStyle().FrameRounding);
}

bool DragFloat(const char* id, float* v, float speed, float mn, float mx, const char* fmt,
               ImGuiSliderFlags flags)
{
    const FrameRect fr = FrameRect::Peek();
    PushMono();
    const bool ch = ImGui::DragFloat(id, v, speed, mn, mx, fmt, flags);
    PopMono();
    OutlineRect(fr.mn, fr.mx, ImGui::IsItemHovered(), ImGui::IsItemActive(), ImGui::GetStyle().FrameRounding);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    return ch;
}

bool DragInt(const char* id, int* v, float speed, int mn, int mx, const char* fmt, ImGuiSliderFlags flags)
{
    const FrameRect fr = FrameRect::Peek();
    PushMono();
    const bool ch = ImGui::DragInt(id, v, speed, mn, mx, fmt, flags);
    PopMono();
    OutlineRect(fr.mn, fr.mx, ImGui::IsItemHovered(), ImGui::IsItemActive(), ImGui::GetStyle().FrameRounding);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    return ch;
}

bool SliderFloat(const char* id, float* v, float mn, float mx, const char* fmt, ImGuiSliderFlags flags)
{
    FillSlider fs;
    fs.Begin();
    const bool ch = ImGui::SliderFloat(id, v, mn, mx, fmt, flags);
    char txt[64];
    std::snprintf(txt, sizeof(txt), fmt ? fmt : "%.3f", static_cast<double>(*v));
    fs.End(FracFloat(*v, mn, mx, flags), txt);
    return ch;
}

bool SliderInt(const char* id, int* v, int mn, int mx, const char* fmt, ImGuiSliderFlags flags)
{
    FillSlider fs;
    fs.Begin();
    const bool ch = ImGui::SliderInt(id, v, mn, mx, fmt, flags);
    char txt[64];
    std::snprintf(txt, sizeof(txt), fmt ? fmt : "%d", *v);
    fs.End(FracFloat(static_cast<float>(*v), static_cast<float>(mn), static_cast<float>(mx), flags), txt);
    return ch;
}

bool Checkbox(const char* label, bool* v)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    const bool hasLabel = labelEnd > label;
    const float box    = Px(th::size::kCheckBox);
    const float frameH = ImGui::GetFrameHeight();
    const ImVec2 labelSz = hasLabel ? ImGui::CalcTextSize(label, labelEnd, true) : ImVec2(0, 0);
    const ImVec2 total(box + (hasLabel ? st.ItemInnerSpacing.x + Px(2.0f) + labelSz.x : 0.0f), frameH);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, total);
    if (pressed) *v = !*v;
    const bool hov  = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b0(p.x, p.y + (frameH - box) * 0.5f);
    const ImVec2 b1(b0.x + box, b0.y + box);
    const float r = Px(2.0f);
    if (*v)
    {
        const ImVec4& fill = held ? th::AccentPressed : hov ? th::AccentHover : th::Accent;
        dl->AddRectFilled(b0, b1, Col(fill), r);
        if (deco::Active())
        {
            // B = 上が明るいガラスの艶 + 光の縁 / A = ホバー・フォーカスでネオンのにじみ / C = 装飾なし（平たいシグナル塗り）
            if (th::CurrentVariant() == th::Variant::B)
            {
                dl->AddRectFilledMultiColor(ImVec2(b0.x + Px(1.5f), b0.y + Px(1.0f)), ImVec2(b1.x - Px(1.5f), b0.y + box * 0.5f),
                                            ColA(kWhite, 0.24f), ColA(kWhite, 0.24f), ColA(kWhite, 0.0f), ColA(kWhite, 0.0f));
                dl->AddRect(b0, b1, ColA(kWhite, 0.20f), r, 0, Px(1.0f));
            }
            else if (th::CurrentVariant() == th::Variant::A)
                deco::Glow(dl, b0, b1, r, th::Accent, 0.30f + 0.70f * deco::Ease(ImGui::GetItemID(), 0, hov ? 1.0f : 0.0f));
        }
        // ✓（太さ 2px の折れ線。フォントに頼らないので大きさが常に箱に対して一定。既定は白 / C は暗色）
        const ImU32 white = Col(th::OnAccent);
        const ImVec2 a(b0.x + box * 0.24f, b0.y + box * 0.52f);
        const ImVec2 m(b0.x + box * 0.43f, b0.y + box * 0.71f);
        const ImVec2 e(b0.x + box * 0.77f, b0.y + box * 0.30f);
        dl->AddPolyline(std::array<ImVec2, 3>{a, m, e}.data(), 3, white, ImDrawFlags_None, Px(2.0f));
    }
    else
    {
        dl->AddRectFilled(b0, b1, Col(held ? th::InputBgActive : hov ? th::InputBgHover : th::InputBg), r);
        dl->AddRect(b0, b1, Col(hov ? th::InputBorderHover : th::InputBorder), r, 0, Px(1.0f));
        if (deco::Active() && hov)
        {
            const th::Deco& dd = th::CurrentDeco();
            if (dd.hoverTint > 0.0f) dl->AddRectFilled(b0, b1, ColA(th::Accent, dd.hoverTint), r);
            if (th::CurrentVariant() == th::Variant::A) dl->AddRect(b0, b1, ColA(th::Accent, 0.5f), r, 0, Px(1.0f));
            if (th::CurrentVariant() == th::Variant::B) dl->AddRect(b0, b1, ColA(th::Hex(0xC8D2FF), 0.32f), r, 0, Px(1.0f));
        }
    }
    if (focused)
        dl->AddRect(ImVec2(b0.x - Px(2.0f), b0.y - Px(2.0f)), ImVec2(b1.x + Px(2.0f), b1.y + Px(2.0f)), Col(th::Accent), r + Px(1.0f), 0, Px(1.0f));

    if (hasLabel)
    {
        const float ty = p.y + (frameH - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(b1.x + st.ItemInnerSpacing.x + Px(2.0f), std::floor(ty + 0.5f)),
                    Col(th::Text), label, labelEnd);
    }
    return pressed;
}

bool BeginCombo(const char* id, const char* preview, ImGuiComboFlags flags)
{
    if (flags & ImGuiComboFlags_NoPreview)
        return ImGui::BeginCombo(id, preview ? preview : "", flags);   // 矢印だけの形はそのまま

    ImDrawList* dl = ImGui::GetWindowDrawList();   // 親窓の DrawList（BeginCombo が開くとポップアップ側へ切り替わる）
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::CalcItemWidth();
    const float h = ImGui::GetFrameHeight();

    // 枠・背景は ImGui に描かせ、プレビュー文字と右の chevron は自前で描く（標準の矢印ボタンは巨大な三角のため）
    const ImGuiID comboId = ImGui::GetID(id);   // 開くと「直前の項目」がポップアップ側へ変わるので、遷移状態のキーは先に取る
    const bool open = ImGui::BeginCombo(id, "", flags | ImGuiComboFlags_NoArrowButton);
    const bool hov = !open && ImGui::IsItemHovered();

    const float chevW = Px(24.0f);
    const ImVec4 clip(p.x, p.y, p.x + w - chevW, p.y + h);
    if (preview && *preview)
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(p.x + st.FramePadding.x, p.y + st.FramePadding.y), Col(th::Text),
                    preview, nullptr, 0.0f, &clip);
    DrawIconCentered(dl, ICON_CHEVRON_DOWN, ImVec2(p.x + w - Px(13.0f), p.y + h * 0.5f),
                     Col(open || hov ? th::Text : th::TextDim), Px(14.0f));
    OutlineRect(p, ImVec2(p.x + w, p.y + h), hov, open, st.FrameRounding, comboId);
    return open;
}

bool Combo(const char* id, int* idx, const char* const items[], int count, int /*popupMaxItems*/)
{
    const char* preview = (*idx >= 0 && *idx < count) ? items[*idx] : "";
    bool changed = false;
    if (BeginCombo(id, preview))
    {
        for (int i = 0; i < count; ++i)
        {
            const bool sel = (i == *idx);
            if (ImGui::Selectable(items[i], sel))
            {
                *idx = i;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool Combo(const char* id, int* idx, const char* itemsSeparatedByZeros, int popupMaxItems)
{
    std::vector<const char*> v;
    for (const char* p = itemsSeparatedByZeros; p && *p; p += std::strlen(p) + 1)
        v.push_back(p);
    return Combo(id, idx, v.data(), static_cast<int>(v.size()), popupMaxItems);
}

bool InputText(const char* id, char* buf, size_t size, ImGuiInputTextFlags flags)
{
    const FrameRect fr = FrameRect::Peek();
    const bool ch = ImGui::InputText(id, buf, size, flags);
    OutlineRect(fr.mn, fr.mx, ImGui::IsItemHovered(), ImGui::IsItemActive(), ImGui::GetStyle().FrameRounding);
    return ch;
}

bool InputTextWithHint(const char* id, const char* hint, char* buf, size_t size, ImGuiInputTextFlags flags)
{
    const FrameRect fr = FrameRect::Peek();
    const bool ch = ImGui::InputTextWithHint(id, hint, buf, size, flags);
    OutlineRect(fr.mn, fr.mx, ImGui::IsItemHovered(), ImGui::IsItemActive(), ImGui::GetStyle().FrameRounding);
    return ch;
}

bool ColorEdit3(const char* id, float* col, ImGuiColorEditFlags flags)
{
    const bool ch = ImGui::ColorEdit3(id, col, flags);
    if (flags & ImGuiColorEditFlags_NoInputs) OutlineLastItem();
    return ch;
}

bool ColorEdit4(const char* id, float* col, ImGuiColorEditFlags flags)
{
    const bool ch = ImGui::ColorEdit4(id, col, flags);
    if (flags & ImGuiColorEditFlags_NoInputs) OutlineLastItem();
    return ch;
}

bool SearchField(const char* id, char* buf, size_t size, const char* hint)
{
    const float padX = Px(26.0f);
    const FrameRect fr = FrameRect::Peek();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, ImGui::GetStyle().FramePadding.y));
    const bool ch = ImGui::InputTextWithHint(id, hint, buf, size);
    ImGui::PopStyleVar();
    const ImVec2 mn = fr.mn;
    const ImVec2 mx = fr.mx;
    const bool hov = ImGui::IsItemHovered();
    const bool act = ImGui::IsItemActive();
    OutlineRect(mn, mx, hov, act, ImGui::GetStyle().FrameRounding);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = (mn.y + mx.y) * 0.5f;
    DrawIconCentered(dl, ICON_SEARCH, ImVec2(mn.x + Px(13.0f), cy), Col(act ? th::Text : th::TextDim), Px(15.0f));

    bool cleared = false;
    if (buf[0] != '\0')
    {
        const ImVec2 c(mx.x - Px(13.0f), cy);
        const float hit = Px(9.0f);
        const bool overX = ImGui::IsMouseHoveringRect(ImVec2(c.x - hit, c.y - hit), ImVec2(c.x + hit, c.y + hit));
        DrawIconCentered(dl, ICON_CLOSE, c, Col(overX ? th::Text : th::TextFaint), Px(14.0f));
        if (overX && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            buf[0] = '\0';
            cleared = true;
        }
    }
    return ch || cleared;
}

// ---------------------------------------------------------------------------
// 見出し
// ---------------------------------------------------------------------------
bool SectionHeader(const char* label, const char* glyph, const ImVec4* tint, ImGuiTreeNodeFlags flags)
{
    const float h = Px(th::size::kHeaderH);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(8.0f), (h - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);   // 帯は面で見せる（枠線なし）
    ImGui::PushStyleColor(ImGuiCol_Header,        th::Bg2);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, th::Bg3);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  th::Bg4);
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0, 0, 0, 0));   // ImGui 標準の矢印と文字は隠して自前で描く
    const bool open = ImGui::CollapsingHeader(label, flags | ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);

    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    const float cy = (mn.y + mx.y) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float x = mn.x + Px(8.0f);
    DrawIconCentered(dl, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ImVec2(x + Px(7.0f), cy),
                     Col(th::TextDim), Px(14.0f));
    x += Px(21.0f);
    if (glyph && *glyph)
    {
        DrawIconCentered(dl, glyph, ImVec2(x + Px(8.0f), cy), Col(tint ? *tint : th::TextDim), Px(16.0f));
        x += Px(25.0f);
    }
    PushBold();
    const char* end = ImGui::FindRenderedTextEnd(label);
    const float ty = std::floor(cy - ImGui::GetTextLineHeight() * 0.5f + 0.5f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, ty), Col(th::Text), label, end);
    PopBold();
    if (deco::Active())
    {
        // 見出しの「光のアクセント」: A = 左端の小さなネオン片（開いている時だけ点灯）/ B = 上端の光のヘアライン /
        // C = 左端のシグナル片（開いている時）
        const th::HeaderStyle hs = th::CurrentDeco().headerStyle;
        if (hs == th::HeaderStyle::GlowTick)
        {
            const ImVec2 t0(mn.x, cy - Px(6.0f)), t1(mn.x + Px(2.0f), cy + Px(6.0f));
            dl->AddRectFilled(t0, t1, ColA(th::AccentHover, open ? 1.0f : 0.35f), Px(1.0f));
            if (open) deco::Glow(dl, t0, t1, Px(1.0f), th::Accent, 0.8f);
        }
        else if (hs == th::HeaderStyle::Glass)
        {
            const float mid = (mn.x + mx.x) * 0.5f;
            const float y = mn.y;
            dl->AddRectFilledMultiColor(ImVec2(mn.x + Px(4.0f), y), ImVec2(mid, y + Px(1.0f)),
                                        ColA(kWhite, 0.0f), ColA(kWhite, 0.16f), ColA(kWhite, 0.16f), ColA(kWhite, 0.0f));
            dl->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(mx.x - Px(4.0f), y + Px(1.0f)),
                                        ColA(kWhite, 0.16f), ColA(kWhite, 0.0f), ColA(kWhite, 0.0f), ColA(kWhite, 0.16f));
        }
        else if (hs == th::HeaderStyle::SignalTick && open)
        {
            dl->AddRectFilled(ImVec2(mn.x, cy - Px(7.0f)), ImVec2(mn.x + Px(2.0f), cy + Px(7.0f)), Col(th::Accent), 0.0f);
        }
    }
    return open;
}

bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags)
{
    return SectionHeader(label, nullptr, nullptr, flags);
}

bool HeaderMenuButton()
{
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    const float s = Px(22.0f);
    const ImVec2 pos(mx.x - s - Px(4.0f), mn.y + ((mx.y - mn.y) - s) * 0.5f);
    const ImVec2 save = ImGui::GetCursorScreenPos();

    ImGui::SetCursorScreenPos(pos);
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("##hdrmenu", ImVec2(s, s));
    const bool hov  = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (held || hov)
        dl->AddRectFilled(pos, ImVec2(pos.x + s, pos.y + s), Col(held ? th::Bg4 : th::Bg3), Px(3.0f));
    DrawIconCentered(dl, ICON_ELLIPSIS, ImVec2(pos.x + s * 0.5f, pos.y + s * 0.5f),
                     Col(hov ? th::Text : th::TextFaint), Px(16.0f));
    if (hov) ImGui::SetTooltip("コンポーネントのメニュー");
    ImGui::SetCursorScreenPos(save);
    return clicked;
}

// ---------------------------------------------------------------------------
// メニュー
// ---------------------------------------------------------------------------
bool MenuItem(const char* icon, const char* label, const char* shortcut, bool selected, bool enabled)
{
    return ImGui::MenuItemEx(label, icon, shortcut, selected, enabled);
}

bool MenuItem(const char* icon, const char* label, const char* shortcut, bool* selected, bool enabled)
{
    if (ImGui::MenuItemEx(label, icon, shortcut, selected ? *selected : false, enabled))
    {
        if (selected) *selected = !*selected;
        return true;
    }
    return false;
}

void PushMenuStyle()
{
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, Px(9.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(6.0f), Px(6.0f)));
}

void PopMenuStyle()
{
    ImGui::PopStyleVar(2);
}

// ===========================================================================
// deco:: — テーマ・バリアントの共通装飾ヘルパ（宣言と方針は UiWidgets.h）
// ===========================================================================
namespace deco
{

bool Active() { return !th::IsDefaultVariant(); }

namespace
{
// (id, slot) → 0..1。ImGui のウィンドウ単位のストレージではなくグローバルに持つ（ポップアップの開閉で窓が変わっても続く）。
ImGuiStorage& EaseStore() { static ImGuiStorage s; return s; }
} // namespace

float Ease(ImGuiID id, int slot, float target)
{
    const float tau = th::CurrentDeco().easeSec;
    if (tau <= 0.0f) return target;
    ImGuiStorage& st = EaseStore();
    const ImGuiID key = ImHashData(&slot, sizeof(slot), id);
    float v = st.GetFloat(key, 0.0f);
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    v += (target - v) * (1.0f - std::exp(-dt / tau));      // 1 次遅れ（ease-out 相当）
    if (std::fabs(target - v) < 0.003f) v = target;
    st.SetFloat(key, v);
    return v;
}

void Glow(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding, const ImVec4& col, float strength)
{
    const float s = strength * th::CurrentDeco().glow;
    if (s <= 0.01f || !dl) return;
    const float px = PxF(1.0f);
    const int steps = 5;
    // 窓のクリップより外（テーブルのセル境界・入力欄の余白）にも出すため、窓全体へ広げて描く
    ImGuiWindow* w = ImGui::GetCurrentWindowRead();
    const bool widen = (w != nullptr);
    if (widen) dl->PushClipRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), false);
    for (int i = 1; i <= steps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps + 1);
        const float a = s * 0.30f * (1.0f - t) * (1.0f - t);
        const float d = px * static_cast<float>(i);
        dl->AddRect(ImVec2(mn.x - d, mn.y - d), ImVec2(mx.x + d, mx.y + d), ColA(col, a), rounding + d, 0, px);
    }
    if (widen) dl->PopClipRect();
}

void RowFace(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool selected, bool hovered, bool held)
{
    if (!Active())
    {
        // 現行（ヒエラルキーの PaintRowBg そのまま）
        const ImVec4* col = nullptr;
        if (held)          col = &th::SelectionActive;
        else if (selected) col = hovered ? &th::SelectionActive : &th::Selection;
        else if (hovered)  col = &th::Bg3;
        if (col) dl->AddRectFilled(mn, mx, ImGui::GetColorU32(*col));
        if (selected)
            dl->AddRectFilled(mn, ImVec2(mn.x + Px(2.0f), mx.y), ImGui::GetColorU32(th::Accent));
        return;
    }
    const th::RowStyle rs = th::CurrentDeco().rowStyle;
    const float b1 = Px(1.0f);
    if (rs == th::RowStyle::NeonBar)
    {
        // 選択 = 左から右へ薄れる面 + 左 2px のネオンバー + バーの右ににじむ光 / ホバー = アクセントがうっすら乗る
        if (selected || held)
        {
            const float k = held ? 1.45f : hovered ? 1.2f : 1.0f;
            dl->AddRectFilledMultiColor(mn, mx, ColA(th::Accent, 0.30f * k), ColA(th::Accent, 0.08f * k),
                                        ColA(th::Accent, 0.08f * k), ColA(th::Accent, 0.30f * k));
            dl->AddRectFilledMultiColor(ImVec2(mn.x + Px(2.0f), mn.y), ImVec2(mn.x + Px(16.0f), mx.y),
                                        ColA(th::AccentHover, 0.30f), ColA(th::AccentHover, 0.0f),
                                        ColA(th::AccentHover, 0.0f), ColA(th::AccentHover, 0.30f));
            dl->AddRectFilled(mn, ImVec2(mn.x + Px(2.0f), mx.y), ImGui::GetColorU32(th::AccentHover));
        }
        else if (hovered)
        {
            dl->AddRectFilled(mn, mx, ColA(th::Bg3, 0.85f));
            dl->AddRectFilledMultiColor(mn, mx, ColA(th::Accent, 0.10f), ColA(th::Accent, 0.0f), ColA(th::Accent, 0.0f), ColA(th::Accent, 0.10f));
            dl->AddRectFilled(mn, ImVec2(mn.x + b1, mx.y), ColA(th::Accent, 0.55f));
        }
    }
    else if (rs == th::RowStyle::GlassPill)
    {
        // 行の中に浮かぶガラスの丸い面（左右 4px・上下 1px 内側）
        const ImVec2 a(mn.x + Px(4.0f), mn.y + b1), b(mx.x - Px(4.0f), mx.y - b1);
        const float r = Px(5.0f);
        if (selected || held)
        {
            dl->AddRectFilled(a, b, ColA(th::Accent, held ? 0.34f : hovered ? 0.30f : 0.25f), r);
            dl->AddRectFilledMultiColor(ImVec2(a.x + r * 0.6f, a.y + b1), ImVec2(b.x - r * 0.6f, a.y + (b.y - a.y) * 0.5f),
                                        ColA(kWhite, 0.10f), ColA(kWhite, 0.10f), ColA(kWhite, 0.0f), ColA(kWhite, 0.0f));
            dl->AddRect(a, b, ColA(th::AccentHover, 0.42f), r, 0, b1);
        }
        else if (hovered)
        {
            dl->AddRectFilled(a, b, ColA(kWhite, 0.055f), r);
            dl->AddRect(a, b, ColA(kWhite, 0.09f), r, 0, b1);
        }
    }
    else   // SignalBar
    {
        // 面は無彩色。選択は左端の 3px シグナルバーだけが色を持つ
        if (held)          dl->AddRectFilled(mn, mx, ImGui::GetColorU32(th::SelectionActive));
        else if (selected) dl->AddRectFilled(mn, mx, ImGui::GetColorU32(hovered ? th::SelectionActive : th::Selection));
        else if (hovered)  dl->AddRectFilled(mn, mx, ImGui::GetColorU32(th::SelectionHover));
        if (selected)
            dl->AddRectFilled(mn, ImVec2(mn.x + Px(3.0f), mx.y), ImGui::GetColorU32(th::Accent));
    }
}

void CardSelected(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding)
{
    if (!Active())
    {
        dl->AddRect(mn, mx, ImGui::GetColorU32(th::Accent), rounding, 0, Px(2.0f));
        return;
    }
    const th::Variant v = th::CurrentVariant();
    if (v == th::Variant::A)
    {
        dl->AddRect(mn, mx, ImGui::GetColorU32(th::AccentHover), rounding, 0, Px(1.0f));
        Glow(dl, mn, mx, rounding, th::Accent, 1.0f);
    }
    else if (v == th::Variant::B)
    {
        dl->AddRect(mn, mx, ColA(th::AccentHover, 0.85f), rounding, 0, Px(1.0f));
        dl->AddRect(ImVec2(mn.x + Px(1.0f), mn.y + Px(1.0f)), ImVec2(mx.x - Px(1.0f), mx.y - Px(1.0f)), ColA(kWhite, 0.10f),
                    std::max(0.0f, rounding - Px(1.0f)), 0, Px(1.0f));
        Glow(dl, mn, mx, rounding, th::Accent, 0.7f);
    }
    else
        dl->AddRect(mn, mx, ImGui::GetColorU32(th::Accent), rounding, 0, Px(2.0f));
}

void SelectionBar(ImDrawList* dl, ImVec2 mn, ImVec2 mx)
{
    if (!Active() || th::CurrentVariant() == th::Variant::B)
    {
        dl->AddRectFilled(mn, ImVec2(mn.x + Px(2.0f), mx.y), ImGui::GetColorU32(th::Accent));
        return;
    }
    if (th::CurrentVariant() == th::Variant::A)
    {
        dl->AddRectFilledMultiColor(ImVec2(mn.x + Px(2.0f), mn.y), ImVec2(mn.x + Px(14.0f), mx.y),
                                    ColA(th::AccentHover, 0.30f), ColA(th::AccentHover, 0.0f),
                                    ColA(th::AccentHover, 0.0f), ColA(th::AccentHover, 0.30f));
        dl->AddRectFilled(mn, ImVec2(mn.x + Px(2.0f), mx.y), ImGui::GetColorU32(th::AccentHover));
        return;
    }
    dl->AddRectFilled(mn, ImVec2(mn.x + Px(3.0f), mx.y), ImGui::GetColorU32(th::Accent));
}

namespace
{
// 影: 対象矩形の外側へ多重の細い枠（外ほど薄い）を重ねる疑似ブラー。矩形の内側には一切描かない
// （窓の描画後に足すので、内側へ食い込むと中身の上に乗ってしまう）。下側だけ少し厚く落とす。
void LayeredShadow(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding, float strength, ImVec2 clipMin, ImVec2 clipMax)
{
    if (strength <= 0.01f) return;
    dl->PushClipRect(clipMin, clipMax, false);
    const float px = PxF(1.0f);
    const int n = static_cast<int>(std::round(14.0f * std::max(1.0f, th::Scale())));
    for (int i = 1; i <= n; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n + 1);
        const float a = 0.34f * strength * std::pow(1.0f - t, 2.3f);
        const float d = px * static_cast<float>(i) * (14.0f / static_cast<float>(n));
        dl->AddRect(ImVec2(mn.x - d, mn.y - d * 0.55f), ImVec2(mx.x + d, mx.y + d * 1.35f),
                    IM_COL32(0, 0, 0, static_cast<int>(a * 255.0f)), rounding + d, 0, px * 1.3f);
    }
    dl->PopClipRect();
}

// 上端の光のヘアライン（中央が最も明るく左右へ消える）。
void TopHairline(ImDrawList* dl, float x0, float x1, float y, const ImVec4& col, float a)
{
    const float mid = (x0 + x1) * 0.5f;
    const float h = PxF(1.0f);
    dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(mid, y + h), ColA(col, 0.0f), ColA(col, a), ColA(col, a), ColA(col, 0.0f));
    dl->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(x1, y + h), ColA(col, a), ColA(col, 0.0f), ColA(col, 0.0f), ColA(col, a));
}

bool StartsWith(const char* s, const char* prefix) { return std::strncmp(s, prefix, std::strlen(prefix)) == 0; }
} // namespace

void FloatingCard(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding, const ImVec4& face, float alpha)
{
    auto colAlpha = [&](const ImVec4& c, float a) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); };
    if (!Active())
    {
        // 現行のトースト（影 1 枚 + 面 + 強い枠）
        dl->AddRectFilled(ImVec2(mn.x + Px(1.0f), mn.y + Px(2.0f)), ImVec2(mx.x + Px(1.0f), mx.y + Px(3.0f)),
                          IM_COL32(0, 0, 0, static_cast<int>(70 * alpha)), rounding);
        dl->AddRectFilled(mn, mx, colAlpha(face, alpha), rounding);
        dl->AddRect(mn, mx, colAlpha(th::BorderStrong, alpha), rounding, 0, Px(1.0f));
        return;
    }
    const th::Deco& d = th::CurrentDeco();
    const float r = std::max(rounding, Px(th::CurrentVariant() == th::Variant::B ? 8.0f : th::CurrentVariant() == th::Variant::A ? 5.0f : 3.0f));
    if (d.layeredShadow)
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        LayeredShadow(dl, mn, mx, r, d.shadowStrength * alpha, vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y));
    }
    dl->AddRectFilled(mn, mx, colAlpha(face, alpha), r);
    if (th::CurrentVariant() == th::Variant::B)
    {
        dl->AddRectFilledMultiColor(ImVec2(mn.x + r * 0.5f, mn.y + Px(1.0f)), ImVec2(mx.x - r * 0.5f, mn.y + (mx.y - mn.y) * 0.5f),
                                    ColA(kWhite, 0.07f * alpha), ColA(kWhite, 0.07f * alpha), ColA(kWhite, 0.0f), ColA(kWhite, 0.0f));
        dl->AddRect(mn, mx, ColA(kWhite, 0.13f * alpha), r, 0, Px(1.0f));
        TopHairline(dl, mn.x + r, mx.x - r, mn.y, kWhite, 0.30f * alpha);
    }
    else
        dl->AddRect(mn, mx, colAlpha(th::BorderStrong, alpha), r, 0, Px(1.0f));
}

void PaintChrome()
{
    if (!Active()) return;
    ImGuiContext* gp = ImGui::GetCurrentContext();
    if (!gp) return;
    ImGuiContext& g = *gp;
    const th::Deco& d = th::CurrentDeco();
    const th::Variant v = th::CurrentVariant();
    const float rounding = g.Style.WindowRounding;

    for (ImGuiWindow* w : g.Windows)
    {
        if (!w->Active || w->Hidden || w->SkipItems || w->IsFallbackWindow) continue;
        // ★ドック窓は ImGui 内部で ChildWindow フラグが付く（ドックのホスト窓の子として描かれる）。本物の子窓だけ除く。
        if ((w->Flags & ImGuiWindowFlags_ChildWindow) && !w->DockIsActive) continue;
        const ImVec2 mn = w->Pos;
        const ImVec2 mx(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
        ImDrawList* dl = w->DrawList;
        if (!dl) continue;
        const ImGuiViewport* vp = w->Viewport ? w->Viewport : ImGui::GetMainViewport();
        const ImVec2 vpMin = vp->Pos, vpMax(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);

        // --- ポップアップ / メニュー / パレット / ツールチップ: 影 + 光の縁 ---
        if (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip))
        {
            if (w->Flags & ImGuiWindowFlags_Tooltip)
            {
                if (d.layeredShadow) LayeredShadow(dl, mn, mx, g.Style.PopupRounding, d.shadowStrength * 0.6f, vpMin, vpMax);
                continue;
            }
            if (d.layeredShadow) LayeredShadow(dl, mn, mx, g.Style.PopupRounding, d.shadowStrength, vpMin, vpMax);
            dl->PushClipRect(mn, mx, false);
            if (d.rimAlpha > 0.0f)
            {
                const float b1 = PxF(1.0f);
                dl->AddRect(ImVec2(mn.x + b1, mn.y + b1), ImVec2(mx.x - b1, mx.y - b1), ColA(kWhite, d.rimAlpha * 0.55f),
                            std::max(0.0f, g.Style.PopupRounding - b1), 0, b1);
                dl->AddRectFilledMultiColor(ImVec2(mn.x + g.Style.PopupRounding * 0.5f, mn.y + b1),
                                            ImVec2(mx.x - g.Style.PopupRounding * 0.5f, mn.y + Px(28.0f)),
                                            ColA(kWhite, 0.045f), ColA(kWhite, 0.045f), ColA(kWhite, 0.0f), ColA(kWhite, 0.0f));
                TopHairline(dl, mn.x + g.Style.PopupRounding, mx.x - g.Style.PopupRounding, mn.y, kWhite, 0.34f);
            }
            else if (v == th::Variant::A)
            {
                // 縁に沿ってごく淡いネオンの内側線
                const float b1 = PxF(1.0f);
                dl->AddRect(ImVec2(mn.x + b1, mn.y + b1), ImVec2(mx.x - b1, mx.y - b1), ColA(th::Accent, 0.10f),
                            std::max(0.0f, g.Style.PopupRounding - b1), 0, b1);
                TopHairline(dl, mn.x + g.Style.PopupRounding, mx.x - g.Style.PopupRounding, mn.y, th::AccentHover, 0.55f);
            }
            dl->PopClipRect();
            continue;
        }

        // --- ツールバー: 下端の光の線（A）---
        if (StartsWith(w->Name, "##Toolbar"))
        {
            if (d.toolbarLine)
            {
                dl->PushClipRect(mn, mx, false);
                TopHairline(dl, mn.x, mx.x, mx.y - PxF(1.0f), th::AccentHover, 0.60f);
                dl->PopClipRect();
            }
            continue;
        }

        // --- ステータスバー: 上端の線（A = 左から消えていくネオン線）---
        if (StartsWith(w->Name, "##StatusBar"))
        {
            if (d.statusTopLine)
            {
                dl->PushClipRect(mn, mx, false);
                dl->AddRectFilledMultiColor(ImVec2(mn.x, mn.y), ImVec2(mn.x + (mx.x - mn.x) * 0.55f, mn.y + PxF(1.0f)),
                                            ColA(th::Accent, 0.60f), ColA(th::Accent, 0.0f), ColA(th::Accent, 0.0f), ColA(th::Accent, 0.60f));
                dl->PopClipRect();
            }
            continue;
        }

        // --- パネル（ドック窓 / フローティングのツール窓）---
        if (w->Flags & ImGuiWindowFlags_NoBackground) continue;
        if (StartsWith(w->Name, "##")) continue;
        if (w->Size.x >= vp->Size.x * 0.95f && w->Size.y >= vp->Size.y * 0.90f) continue;   // 全画面（ランチャー等）
        const bool focused = g.NavWindow && (g.NavWindow == w || g.NavWindow->RootWindow == w) && !g.NavWindowingTarget;

        // ガラスの背後の光（B）: 窓面が半透明なので、ドック窓の背後（背景リスト）に敷いたグラデが透けて見える。
        // フローティング窓には敷かない（背後の 3D ビューがそのまま透けるほうが「ガラス」らしい）。
        if (d.backdropTop.w > 0.0f && w->DockIsActive)
        {
            const float ins = std::max(1.0f, rounding * 0.3f);
            ImDrawList* bg = ImGui::GetBackgroundDrawList(const_cast<ImGuiViewport*>(vp));
            bg->AddRectFilledMultiColor(ImVec2(mn.x + ins, mn.y + ins), ImVec2(mx.x - ins, mx.y - ins),
                                        ColA(d.backdropTop, 1.0f), ColA(d.backdropTop, 1.0f),
                                        ColA(d.backdropBottom, 1.0f), ColA(d.backdropBottom, 1.0f));
        }

        dl->PushClipRect(mn, mx, false);
        const float px = PxF(1.0f);
        if (d.panelSheen)
        {
            const float hh = std::min(w->Size.y, Px(150.0f));
            dl->AddRectFilledMultiColor(mn, ImVec2(mx.x, mn.y + hh), ColA(kWhite, 0.040f), ColA(kWhite, 0.040f),
                                        ColA(kWhite, 0.0f), ColA(kWhite, 0.0f));
        }
        if (d.rimAlpha > 0.0f)
            dl->AddRect(ImVec2(mn.x + px * 0.5f, mn.y + px * 0.5f), ImVec2(mx.x - px * 0.5f, mx.y - px * 0.5f),
                        ColA(kWhite, d.rimAlpha * (focused ? 0.95f : 0.62f)), rounding, 0, px);
        if (d.panelTopLine)
            TopHairline(dl, mn.x + rounding, mx.x - rounding, mn.y, kWhite, focused ? 0.38f : 0.24f);
        if (d.panelFocusEdge && focused)
        {
            // フォーカス中のパネルだけ、縁に沿って細いネオンのエッジライト（1px + 内側へ薄れる 4px）
            dl->AddRect(ImVec2(mn.x + px * 0.5f, mn.y + px * 0.5f), ImVec2(mx.x - px * 0.5f, mx.y - px * 0.5f),
                        ColA(th::Accent, 0.55f), rounding, 0, px);
            for (int i = 1; i <= 4; ++i)
            {
                const float t = static_cast<float>(i) / 5.0f;
                const float o = px * (static_cast<float>(i) + 0.5f);
                dl->AddRect(ImVec2(mn.x + o, mn.y + o), ImVec2(mx.x - o, mx.y - o), ColA(th::Accent, 0.14f * (1.0f - t) * (1.0f - t) * 2.2f),
                            rounding, 0, px);
            }
            TopHairline(dl, mn.x, mx.x, mn.y, th::AccentHover, 0.85f);
        }
        dl->PopClipRect();

        // --- 選択タブの光（タブ帯はドックのホスト窓が描く。そこへ足す）---
        if (w->DockNode && w->DockNode->TabBar && w->DockNode->HostWindow && w->DockIsActive && w->DockTabIsVisible
            && (v == th::Variant::A || v == th::Variant::B))
        {
            ImGuiTabBar* tb = w->DockNode->TabBar;
            if (tb->SelectedTabId == w->TabId)
            {
                ImGuiTabItem* tab = ImGui::TabBarFindTabByID(tb, w->TabId);
                if (tab)
                {
                    ImDrawList* hdl = w->DockNode->HostWindow->DrawList;
                    const float tx0 = tb->BarRect.Min.x + tab->Offset - tb->ScrollingAnim;
                    const float tx1 = tx0 + tab->Width;
                    const float ty = tb->BarRect.Min.y;
                    hdl->PushClipRect(tb->BarRect.Min, tb->BarRect.Max, true);
                    const ImVec4& lc = (v == th::Variant::A) ? th::Accent : kWhite;
                    const float a = (v == th::Variant::A) ? 0.24f : 0.07f;
                    const float ov = std::max(px, g.Style.TabBarOverlineSize);
                    hdl->AddRectFilledMultiColor(ImVec2(tx0, ty + ov), ImVec2(tx1, ty + ov + Px(10.0f)),
                                                 ColA(lc, a), ColA(lc, a), ColA(lc, 0.0f), ColA(lc, 0.0f));
                    hdl->PopClipRect();
                }
            }
        }
    }
}

} // namespace deco

} // namespace dx12e::ui
