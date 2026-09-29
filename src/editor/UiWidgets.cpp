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
void OutlineRect(ImVec2 mn, ImVec2 mx, bool hovered, bool active, float rounding)
{
    if (!hovered && !active) return;
    ImGui::GetWindowDrawList()->AddRect(
        mn, mx, Col(active ? th::Accent : th::InputBorderHover), rounding, 0, Px(1.0f));
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
        // 塗りバー（アクセント 40%）
        t = std::min(1.0f, std::max(0.0f, t));
        const float b1px = Px(1.0f);
        const float x0 = mn.x + b1px, x1 = mx.x - b1px;
        const float fx = x0 + (x1 - x0) * t;
        if (fx - x0 >= b1px)
        {
            dl->AddRectFilled(ImVec2(x0, mn.y + b1px), ImVec2(fx, mx.y - b1px),
                              Col(th::WithAlpha(th::Accent, act ? 0.55f : 0.40f)),
                              std::max(0.0f, r - b1px),
                              t >= 0.999f ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
        }
        // つまみ = 塗りの先端の細い縦線。値の文字に重なるときは描かない（読みやすさ優先）。
        {
            const float gx = std::min(x1 - b1px, std::max(x0 + b1px, fx));
            const float cx = (mn.x + mx.x) * 0.5f;
            const bool overText = std::fabs(gx - cx) < textW * 0.5f + Px(4.0f);
            if (!overText || act)
                dl->AddRectFilled(ImVec2(gx - b1px, mn.y + Px(3.0f)), ImVec2(gx + b1px, mx.y - Px(3.0f)),
                                  Col(act ? th::Text : th::AccentHover), Px(1.0f));
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

bool IconButton(const char* id, const char* glyph, const char* tooltip, bool active,
                const ImVec4* tint, float sizePx, float iconPx, const ImVec4* activeFace)
{
    if (sizePx <= 0.0f) sizePx = Px(th::size::kToolbarBtn);   // 0 以下 = 既定（論理 28px を倍率換算）
    const ImVec4& face = activeFace ? *activeFace : th::Accent;
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##ib", ImVec2(sizePx, sizePx));
    const bool hov  = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (active)
        dl->AddRectFilled(mn, mx, Col(th::WithAlpha(face, held ? 0.32f : hov ? 0.28f : 0.22f)), Px(3.0f));
    else if (held)
        dl->AddRectFilled(mn, mx, Col(th::Bg4), Px(3.0f));
    else if (hov)
        dl->AddRectFilled(mn, mx, Col(th::Bg3), Px(3.0f));

    ImVec4 ic = tint ? *tint : (active ? (activeFace ? *activeFace : th::AccentHover) : hov ? th::Text : th::TextDim);
    if (tint && !active && !hov) ic = th::Mul(*tint, 0.88f);
    DrawIconCentered(dl, glyph, ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f), Col(ic),
                     iconPx > 0.0f ? iconPx : Px(th::size::kIconPx + 2.0f));
    if (active)
        dl->AddRectFilled(ImVec2(mn.x + Px(5.0f), mx.y - Px(2.0f)), ImVec2(mx.x - Px(5.0f), mx.y), Col(face), Px(1.0f));

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
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active)
        dl->AddRectFilled(mn, mx, Col(th::WithAlpha(th::Accent, held ? 0.32f : hov ? 0.28f : 0.22f)), Px(3.0f));
    else if (held)
        dl->AddRectFilled(mn, mx, Col(th::Bg4), Px(3.0f));
    else if (hov)
        dl->AddRectFilled(mn, mx, Col(th::Bg3), Px(3.0f));
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
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1, 1, 1, 1));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0, 0, 0, 0));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
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
        // 白い ✓（太さ 2px の折れ線。フォントに頼らないので大きさが常に箱に対して一定）
        const ImU32 white = Col(ImVec4(1, 1, 1, 1));
        const ImVec2 a(b0.x + box * 0.24f, b0.y + box * 0.52f);
        const ImVec2 m(b0.x + box * 0.43f, b0.y + box * 0.71f);
        const ImVec2 e(b0.x + box * 0.77f, b0.y + box * 0.30f);
        dl->AddPolyline(std::array<ImVec2, 3>{a, m, e}.data(), 3, white, ImDrawFlags_None, Px(2.0f));
    }
    else
    {
        dl->AddRectFilled(b0, b1, Col(held ? th::InputBgActive : hov ? th::InputBgHover : th::InputBg), r);
        dl->AddRect(b0, b1, Col(hov ? th::InputBorderHover : th::InputBorder), r, 0, Px(1.0f));
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
    OutlineRect(p, ImVec2(p.x + w, p.y + h), hov, open, st.FrameRounding);
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

} // namespace dx12e::ui
