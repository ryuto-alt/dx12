#include "editor/WhatsNewScreen.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>

#include <imgui.h>

#include "core/Version.h"
#include "editor/EditorIcons.h"
#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"

namespace dx12e::whatsnew
{
namespace
{
namespace th = dx12e::theme;

float P(float v) { return ui::Px(v); }
ImU32 U(const ImVec4& c, float a = 1.0f) { ImVec4 v = c; v.w *= a; return ImGui::ColorConvertFloat4ToU32(v); }
ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
ImFont* FBody() { return th::g_fonts.body ? th::g_fonts.body : ImGui::GetFont(); }
ImFont* FBold() { return th::g_fonts.bold ? th::g_fonts.bold : FBody(); }
ImFont* FMono() { return th::g_fonts.mono ? th::g_fonts.mono : FBody(); }

float TextW(ImFont* f, float px, const char* s) { return f->CalcTextSizeA(P(px), FLT_MAX, 0.0f, s).x; }
float TextH(ImFont* f, float px, float wrap, const char* s) { return f->CalcTextSizeA(P(px), FLT_MAX, wrap, s).y; }
// 折り返し付きで描いて、使った高さを返す。
float Wrapped(ImDrawList* dl, ImFont* f, float px, ImVec2 pos, ImU32 col, const char* s, float wrap)
{
    dl->AddText(f, P(px), pos, col, s, nullptr, wrap);
    return TextH(f, px, wrap, s);
}

const char* AreaIcon(const std::string& a)
{
    if (a == "lighting") return ICON_T_SUN;
    if (a == "editor")   return ICON_T_MONITOR;
    if (a == "physics")  return ICON_T_PHYSICS;
    if (a == "world")    return ICON_SPARKLES;
    if (a == "render")   return ICON_T_MATERIAL;
    if (a == "audio")    return ICON_T_AUDIO;
    if (a == "ai")       return ICON_T_BRAIN;
    if (a == "script")   return ICON_T_SCRIPT;
    if (a == "mcp")      return ICON_TERMINAL;
    if (a == "perf")     return ICON_GAUGE;
    return ICON_ROCKET;
}
ImVec4 AreaTint(const std::string& a)
{
    if (a == "lighting") return th::TypeLight;
    if (a == "editor")   return th::AccentHover;
    if (a == "physics")  return th::TypePhysics;
    if (a == "world")    return th::TypeScript;
    if (a == "render")   return th::TypeLight;
    if (a == "audio")    return th::TypeAudio;
    if (a == "ai")       return th::TypeScript;
    if (a == "script")   return th::TypeScript;
    if (a == "mcp")      return th::AccentHover;
    if (a == "perf")     return th::Good;
    return th::TextMid;
}
ImVec4 KindTint(relnotes::Kind k)
{
    switch (k)
    {
    case relnotes::Kind::Feature:     return th::AccentHover;   // 新機能 = アクセント
    case relnotes::Kind::Improvement: return th::Good;          // 改善 = 緑
    default:                          return th::Warn;          // 修正 = 琥珀
    }
}

// 色つきの小さなラベル（角丸の薄い面 + 文字）。幅を返す。
float Pill(ImDrawList* dl, ImVec2 pos, const char* label, const ImVec4& tint)
{
    const float px = 11.5f;
    const float w = TextW(FBold(), px, label) + P(16.0f);
    const float h = P(20.0f);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), U(tint, 0.16f), P(10.0f));
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), U(tint, 0.34f), P(10.0f), 0, 1.0f);
    dl->AddText(FBold(), P(px), ImVec2(pos.x + P(8.0f), pos.y + (h - P(px)) * 0.5f - P(0.5f)), U(Mix(tint, th::Text, 0.35f)), label);
    return w;
}

std::string GithubTag(const State& s) { return "v" + s.to; }

} // namespace

void Setup(State& s, const std::string& from, const std::string& to, bool manual)
{
    s.from = from;
    s.to = to;
    s.manual = manual;
    s.releases = manual ? relnotes::History(relnotes::All(), to) : relnotes::Range(relnotes::All(), from, to);
    s.expanded.assign(s.releases.size(), 0);
    if (!s.expanded.empty()) s.expanded[0] = 1;      // 新しい版だけ開く
}

std::string TitleText(const State& s)
{
    int f[3];
    const bool upgraded = !s.manual && relnotes::ParseVersion(s.from, f) && relnotes::CompareVersions(s.from, s.to) < 0;
    if (upgraded) return "v" + s.from + " \xE2\x86\x92 v" + s.to + " に更新しました";
    return "v" + s.to + " の更新内容";
}

Action Draw(State& s)
{
    Action act = Action::None;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const float pad = P(26.0f);
    const float contentW = ws.x - pad * 2.0f;
    const relnotes::Release* top = s.releases.empty() ? nullptr : s.releases.front();

    // ================= 見出し =================
    float hy = wp.y + P(24.0f);
    {
        int f[3];
        const bool upgraded = !s.manual && relnotes::ParseVersion(s.from, f) && relnotes::CompareVersions(s.from, s.to) < 0;
        const float px = 23.0f;
        float x = wp.x + pad;
        const float lineH = P(px) + P(4.0f);
        auto seg = [&](const char* t, const ImVec4& c) {
            dl->AddText(FBold(), P(px), ImVec2(x, hy), U(c), t);
            x += TextW(FBold(), px, t);
        };
        const std::string vTo = "v" + s.to;
        if (upgraded)
        {
            const std::string vFrom = "v" + s.from;
            seg(vFrom.c_str(), th::TextDim);
            x += P(12.0f);
            ui::DrawIconCentered(dl, ICON_ARROW_RIGHT, ImVec2(x + P(9.0f), hy + P(px) * 0.5f + P(1.0f)), U(th::TextFaint), P(18.0f));
            x += P(30.0f);
            seg(vTo.c_str(), th::AccentHover);
            x += P(10.0f);
            seg("に更新しました", th::Text);
        }
        else
        {
            seg(vTo.c_str(), th::AccentHover);
            x += P(10.0f);
            seg("の更新内容", th::Text);
        }
        hy += lineH + P(6.0f);
        if (top)
        {
            char line[512];
            std::snprintf(line, sizeof(line), "%s  ・  %s", top->date.c_str(), top->headline.c_str());
            hy += Wrapped(dl, FBody(), 14.0f, ImVec2(wp.x + pad, hy), U(th::TextMid), line, contentW) + P(4.0f);
        }
        if (!s.manual && s.releases.size() > 1)
        {
            char note[160];
            std::snprintf(note, sizeof(note), "間の %d つの版の更新内容をまとめて表示しています（版ごとに開閉できます）。", static_cast<int>(s.releases.size()));
            hy += Wrapped(dl, FBody(), 12.5f, ImVec2(wp.x + pad, hy), U(th::TextDim), note, contentW) + P(2.0f);
        }
        hy += P(14.0f);
        ui::deco::EdgeLine(dl, ImVec2(wp.x + pad, hy), ImVec2(wp.x + ws.x - pad, hy), th::Accent, 0.55f);
        hy += P(1.0f);
    }

    // ================= 本文（スクロール） =================
    const float footerH = P(62.0f);
    ImGui::SetCursorScreenPos(ImVec2(wp.x, hy));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, P(8.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    const float bodyH = (wp.y + ws.y) - hy - footerH;
    if (ImGui::BeginChild("##whatsnew_body", ImVec2(ws.x, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs))
    {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 base0 = ImGui::GetCursorScreenPos();
        const float x0 = base0.x + pad;
        const float w = ws.x - pad * 2.0f - P(6.0f);        // スクロールバーぶんの余白
        float y = base0.y + P(16.0f);
        const bool multi = s.releases.size() > 1;

        for (size_t ri = 0; ri < s.releases.size(); ++ri)
        {
            const relnotes::Release& r = *s.releases[ri];
            ImGui::PushID(static_cast<int>(ri));
            bool open = s.expanded[ri] != 0;

            // ---- 版の見出し行（複数の版のときだけ。単独は画面の見出しが担う）----
            if (multi)
            {
                const float rh = P(46.0f);
                const ImVec2 a(x0, y), b(x0 + w, y + rh);
                ImGui::SetCursorScreenPos(a);
                const bool clicked = ImGui::InvisibleButton("##rel", ImVec2(w, rh));
                const bool hov = ImGui::IsItemHovered();
                if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (clicked) { s.expanded[ri] = open ? 0 : 1; open = !open; }
                const float h = ui::deco::Ease(ImGui::GetItemID(), 0, hov ? 1.0f : 0.0f);
                cdl->AddRectFilled(a, b, U(Mix(th::Bg2, th::Bg3, h)), P(6.0f));
                cdl->AddRect(a, b, U(Mix(th::Border, th::AccentHover, h * 0.6f)), P(6.0f), 0, 1.0f);
                ui::DrawIconCentered(cdl, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ImVec2(a.x + P(20.0f), a.y + rh * 0.5f), U(th::TextDim), P(14.0f));
                const std::string v = "v" + r.version;
                const float vw = TextW(FBold(), 15.0f, v.c_str());
                cdl->AddText(FBold(), P(15.0f), ImVec2(a.x + P(38.0f), a.y + (rh - P(15.0f)) * 0.5f - P(1.0f)), U(ri == 0 ? th::AccentHover : th::Text), v.c_str());
                cdl->AddText(FMono(), P(12.5f), ImVec2(a.x + P(38.0f) + vw + P(12.0f), a.y + (rh - P(12.5f)) * 0.5f), U(th::TextFaint), r.date.c_str());
                // 件数のラベル（右寄せ）。幅が足りなければ見出し文を優先して省く。
                float rx = b.x - P(12.0f);
                const relnotes::Kind ks[] = { relnotes::Kind::Fix, relnotes::Kind::Improvement, relnotes::Kind::Feature };
                for (relnotes::Kind k : ks)
                {
                    const int n = relnotes::CountKind(r, k);
                    if (n == 0) continue;
                    char lab[48];
                    std::snprintf(lab, sizeof(lab), "%s %d", relnotes::KindLabel(k), n);
                    const float pw = TextW(FBold(), 11.5f, lab) + P(16.0f);
                    rx -= pw;
                    Pill(cdl, ImVec2(rx, a.y + (rh - P(20.0f)) * 0.5f), lab, KindTint(k));
                    rx -= P(6.0f);
                }
                // 見出し文（残り幅に収める。収まらなければ省略記号）
                const float hx = a.x + P(38.0f) + vw + P(12.0f) + TextW(FMono(), 12.5f, r.date.c_str()) + P(14.0f);
                const float availW = rx - hx - P(6.0f);
                if (availW > P(60.0f))
                {
                    std::string t = r.headline;
                    while (t.size() > 3 && TextW(FBody(), 13.0f, t.c_str()) > availW)
                    {
                        size_t cut = t.size() - 1;
                        while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;   // UTF-8 の先頭バイトまで戻す
                        t.resize(cut);
                        if (t.size() > 3 && TextW(FBody(), 13.0f, (t + "…").c_str()) <= availW) { t += "…"; break; }
                    }
                    cdl->AddText(FBody(), P(13.0f), ImVec2(hx, a.y + (rh - P(13.0f)) * 0.5f - P(0.5f)), U(th::TextMid), t.c_str());
                }
                y += rh + P(12.0f);
            }

            if (open)
            {
                // ---- 注目カード（2 列）----
                const float gap = P(12.0f);
                const float cw = (w - gap) * 0.5f;
                const float cpad = P(16.0f);
                const float iconD = P(36.0f);
                const float textX = cpad + iconD + P(12.0f);
                const float titleW = cw - textX - cpad;
                const float bodyW = cw - cpad * 2.0f;
                for (size_t i = 0; i < r.highlights.size(); i += 2)
                {
                    float rowH = 0.0f;
                    float hs[2] = { 0, 0 };
                    for (size_t c = 0; c < 2 && i + c < r.highlights.size(); ++c)
                    {
                        const relnotes::Highlight& hl = r.highlights[i + c];
                        const float head = std::max(iconD, P(15.0f) + P(4.0f) + TextH(FBold(), 14.5f, titleW, hl.title.c_str()));
                        hs[c] = cpad + head + P(10.0f) + TextH(FBody(), 13.0f, bodyW, hl.body.c_str()) + cpad;
                        rowH = std::max(rowH, hs[c]);
                    }
                    for (size_t c = 0; c < 2 && i + c < r.highlights.size(); ++c)
                    {
                        const relnotes::Highlight& hl = r.highlights[i + c];
                        const ImVec2 a(x0 + static_cast<float>(c) * (cw + gap), y);
                        const ImVec2 b(a.x + cw, y + rowH);
                        ImGui::PushID(static_cast<int>(i + c));
                        ImGui::SetCursorScreenPos(a);
                        ImGui::InvisibleButton("##card", ImVec2(cw, rowH));
                        const bool hov = ImGui::IsItemHovered();
                        const float h = ui::deco::Ease(ImGui::GetItemID(), 0, hov ? 1.0f : 0.0f);
                        const ImVec4 tint = AreaTint(hl.area);
                        cdl->AddRectFilled(a, b, U(Mix(th::Bg2, th::Bg3, h * 0.7f)), P(8.0f));
                        cdl->AddRect(a, b, U(Mix(th::Border, tint, 0.25f + 0.45f * h)), P(8.0f), 0, 1.0f);
                        // アイコン（分野の色のうすい円）
                        const ImVec2 ic(a.x + cpad + iconD * 0.5f, a.y + cpad + iconD * 0.5f);
                        cdl->AddCircleFilled(ic, iconD * 0.5f, U(tint, 0.16f), 32);
                        cdl->AddCircle(ic, iconD * 0.5f, U(tint, 0.38f), 32, 1.0f);
                        ui::DrawIconCentered(cdl, AreaIcon(hl.area), ic, U(Mix(tint, th::Text, 0.25f)), P(18.0f));
                        // 分野名 + 題
                        cdl->AddText(FBody(), P(11.5f), ImVec2(a.x + textX, a.y + cpad - P(1.0f)), U(Mix(tint, th::TextMid, 0.3f)), relnotes::AreaLabel(hl.area));
                        const float th_ = Wrapped(cdl, FBold(), 14.5f, ImVec2(a.x + textX, a.y + cpad + P(15.0f)), U(th::Text), hl.title.c_str(), titleW);
                        const float head = std::max(iconD, P(15.0f) + P(4.0f) + th_);
                        Wrapped(cdl, FBody(), 13.0f, ImVec2(a.x + cpad, a.y + cpad + head + P(10.0f)), U(th::TextMid), hl.body.c_str(), bodyW);
                        if (!hl.detail.empty())
                        {
                            ui::DrawIconCentered(cdl, ICON_INFO, ImVec2(b.x - P(16.0f), b.y - P(16.0f)), U(th::TextFaint, 0.55f + 0.45f * h), P(12.0f));
                            if (hov)
                            {
                                ImGui::BeginTooltip();
                                ImGui::PushTextWrapPos(P(420.0f));
                                ImGui::TextUnformatted(hl.detail.c_str());
                                ImGui::PopTextWrapPos();
                                ImGui::EndTooltip();
                            }
                        }
                        ImGui::PopID();
                    }
                    y += rowH + gap;
                }

                // ---- 種類別の一覧 ----
                const relnotes::Kind kinds[] = { relnotes::Kind::Feature, relnotes::Kind::Improvement, relnotes::Kind::Fix };
                for (relnotes::Kind k : kinds)
                {
                    const int n = relnotes::CountKind(r, k);
                    if (n == 0) continue;
                    y += P(10.0f);
                    const ImVec4 tint = KindTint(k);
                    const float pw = Pill(cdl, ImVec2(x0, y), relnotes::KindLabel(k), tint);
                    char cnt[24];
                    std::snprintf(cnt, sizeof(cnt), "%d 件", n);
                    cdl->AddText(FBody(), P(12.5f), ImVec2(x0 + pw + P(10.0f), y + P(2.0f)), U(th::TextDim), cnt);
                    y += P(20.0f) + P(10.0f);
                    const float tx = x0 + P(20.0f);
                    const float tw = w - P(20.0f);
                    for (const relnotes::Item& it : r.items)
                    {
                        if (it.kind != k) continue;
                        const float th1 = TextH(FBody(), 14.0f, tw, it.title.c_str());
                        cdl->AddCircleFilled(ImVec2(x0 + P(8.0f), y + P(8.5f)), P(2.4f), U(tint, 0.9f), 10);
                        cdl->AddText(FBody(), P(14.0f), ImVec2(tx, y), U(th::Text), it.title.c_str(), nullptr, tw);
                        y += th1;
                        if (!it.body.empty())
                        {
                            y += P(2.0f);
                            y += Wrapped(cdl, FBody(), 13.0f, ImVec2(tx, y), U(th::TextDim), it.body.c_str(), tw);
                        }
                        y += P(9.0f);
                    }
                }
            }
            y += P(multi ? 14.0f : 6.0f);
            ImGui::PopID();
        }

        // スクロール量の確定（描いた総高さを 1 つのダミーで申告する）
        ImGui::SetCursorScreenPos(base0);
        ImGui::Dummy(ImVec2(ws.x - P(2.0f), (y - base0.y) + P(8.0f)));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // ================= フッタ =================
    const float fy = wp.y + ws.y - footerH;
    dl->AddLine(ImVec2(wp.x, fy), ImVec2(wp.x + ws.x, fy), U(th::Border), 1.0f);
    const float btnH = P(32.0f);
    ImGui::SetCursorScreenPos(ImVec2(wp.x + pad, fy + (footerH - btnH) * 0.5f));
    {
        const std::string label = std::string(ICON_EXTERNAL "  GitHub で全文を見る（") + GithubTag(s) + "）";
        if (ImGui::Button(label.c_str(), ImVec2(0.0f, btnH))) act = Action::OpenGithub;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("既定のブラウザでリリースページを開きます");
    }
    ImGui::SetCursorScreenPos(ImVec2(wp.x + ws.x - pad - P(130.0f), fy + (footerH - btnH) * 0.5f));
    if (ui::PrimaryButton("閉じる", ImVec2(P(130.0f), btnH))) act = Action::Close;

    // Esc で閉じる（ImGui のモーダルは既定では Esc で閉じない）
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        act = Action::Close;
    return act;
}

} // namespace dx12e::whatsnew
