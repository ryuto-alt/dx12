#include "editor/Toast.h"
#include "editor/EditorTheme.h"
#include "editor/EditorIcons.h"
#include "editor/UiWidgets.h"
#include "gui/VirtualInputImGui.h"   // dx12_imgui_find 用アンカー

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>   // BringWindowToDisplayFront（トーストを常に最前面に）
#pragma warning(pop)

#include <cstdio>
#include <mutex>

namespace dx12e::ui
{

namespace
{
std::mutex& Mu() { static std::mutex m; return m; }
ToastQueue& Q() { static ToastQueue q; return q; }
uint32_t g_hoveredId = 0;   // 前フレームでホバーされていた 1 枚（Update で寿命を止める）

const ImVec4& KindColor(ToastKind k)
{
    switch (k)
    {
    case ToastKind::Success: return theme::Good;
    case ToastKind::Warn:    return theme::Warn;
    case ToastKind::Error:   return theme::Bad;
    default:                 return theme::AccentHover;
    }
}
const char* KindIcon(ToastKind k)
{
    switch (k)
    {
    case ToastKind::Success: return ICON_OK;
    case ToastKind::Warn:    return ICON_WARN;
    case ToastKind::Error:   return ICON_ERROR;
    default:                 return ICON_INFO;
    }
}
ImU32 Col(const ImVec4& c, float a) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }
} // namespace

uint32_t Toast(ToastKind kind, std::string message, float seconds)
{
    std::lock_guard<std::mutex> lk(Mu());
    return Q().Push(kind, std::move(message), seconds);
}

size_t ToastLiveCount()
{
    std::lock_guard<std::mutex> lk(Mu());
    return Q().LiveCount();
}

void ToastClearAll()
{
    std::lock_guard<std::mutex> lk(Mu());
    Q().Clear();
}

const char* ToastKindName(ToastKind k)
{
    switch (k)
    {
    case ToastKind::Success: return "success";
    case ToastKind::Warn:    return "warn";
    case ToastKind::Error:   return "error";
    default:                 return "info";
    }
}

bool ParseToastKind(const std::string& s, ToastKind& out)
{
    if (s == "info")    { out = ToastKind::Info;    return true; }
    if (s == "success") { out = ToastKind::Success; return true; }
    if (s == "warn" || s == "warning") { out = ToastKind::Warn; return true; }
    if (s == "error")   { out = ToastKind::Error;   return true; }
    return false;
}

void RenderToasts(float dt, float bottomInset)
{
    std::lock_guard<std::mutex> lk(Mu());
    ToastQueue& q = Q();
    q.Update(dt, g_hoveredId);
    g_hoveredId = 0;
    if (q.Items().empty()) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float kW = 340.0f, kMargin = 16.0f, kGap = 8.0f;
    const float kPadX = 12.0f, kPadY = 10.0f, kIconW = 22.0f, kCloseW = 22.0f, kBar = 3.0f;
    const float wrapW = kW - kBar - kPadX * 2.0f - kIconW - kCloseW;
    const float right = vp->Pos.x + vp->Size.x - kMargin;
    float bottom = vp->Pos.y + vp->Size.y - bottomInset - kMargin;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    // 新しいものが一番下（画面の隅に近い側）。古いものが上へ積み上がる。
    const auto& items = q.Items();
    for (size_t idx = items.size(); idx-- > 0;)
    {
        const ToastItem& t = items[idx];
        const float alpha = ToastQueue::Alpha(t);
        if (alpha <= 0.0f) continue;

        const ImVec2 ts = ImGui::CalcTextSize(t.text.c_str(), nullptr, false, wrapW);
        const float h = (std::max)(44.0f, ts.y + kPadY * 2.0f);
        const float slide = (1.0f - alpha) * 20.0f;               // 出入りで少し右から滑らせる
        const ImVec2 pos(right - kW + slide, bottom - h);

        char name[32];
        std::snprintf(name, sizeof(name), "##Toast%u", t.id);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(kW, h), ImGuiCond_Always);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::Begin(name, nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

        ImGui::SetCursorScreenPos(pos);
        const bool clicked = ImGui::InvisibleButton("##dismiss", ImVec2(kW, h));
        vinput_gui::AnchorLastItem("toast", t.text.c_str());
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) g_hoveredId = t.id;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = pos, p1(pos.x + kW, pos.y + h);
        // 面 + 枠 + 左の種別色バー
        dl->AddRectFilled(ImVec2(p0.x + 1.0f, p0.y + 2.0f), ImVec2(p1.x + 1.0f, p1.y + 3.0f),
                          IM_COL32(0, 0, 0, static_cast<int>(70 * alpha)), 4.0f);   // 影
        dl->AddRectFilled(p0, p1, Col(hovered ? theme::Bg3 : theme::Bg2, alpha), 4.0f);
        dl->AddRect(p0, p1, Col(theme::BorderStrong, alpha), 4.0f);
        dl->AddRectFilled(p0, ImVec2(p0.x + kBar, p1.y), Col(KindColor(t.kind), alpha), 4.0f, ImDrawFlags_RoundCornersLeft);

        // アイコン + 本文（折り返し）
        ui::DrawIconCentered(dl, KindIcon(t.kind), ImVec2(p0.x + kBar + kPadX + kIconW * 0.5f - 4.0f, (p0.y + p1.y) * 0.5f),
                             Col(KindColor(t.kind), alpha), 16.0f);
        const float tx = p0.x + kBar + kPadX + kIconW;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, p0.y + (h - ts.y) * 0.5f),
                    Col(theme::Text, alpha), t.text.c_str(), nullptr, wrapW);

        // 右端: 畳んだ回数 / ホバーで ✕
        if (hovered)
        {
            ui::DrawIconCentered(dl, ICON_CLOSE, ImVec2(p1.x - kPadX - 4.0f, p0.y + 16.0f),
                                 Col(theme::TextMid, alpha), 14.0f);
        }
        else if (t.count > 1)
        {
            char cnt[16];
            std::snprintf(cnt, sizeof(cnt), "x%d", t.count);
            ui::PushMono();
            const ImVec2 cs = ImGui::CalcTextSize(cnt);
            dl->AddText(ImVec2(p1.x - kPadX - cs.x, p0.y + 8.0f), Col(theme::TextDim, alpha), cnt);
            ui::PopMono();
        }

        ImGui::End();
        if (clicked) q.Dismiss(t.id);
        bottom -= h + kGap;
    }
    ImGui::PopStyleVar(2);
}

} // namespace dx12e::ui
