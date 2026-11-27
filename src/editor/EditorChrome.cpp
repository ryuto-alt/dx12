#include "editor/EditorChrome.h"
#include "editor/EditorTheme.h"
#include "editor/ThemeGraphite.gen.h"   // GgDeco::caliper だけ読む
#include "editor/UiMotion.h"
#include "editor/UiWidgets.h"

#include <cmath>

namespace dx12e
{
namespace th = theme;

namespace
{
bool  g_caliperOn = true;
bool  g_haveLogo  = false;
ImRect g_logo;
// 直前に確定したパネルの範囲（フォーカスが ## 窓やポップアップにある間はこれを保つ）。
bool  g_haveSpan = false;
float g_spanX0 = 0.0f, g_spanX1 = 0.0f;
bool  g_spanFocused = true;
} // namespace

bool EditorChrome::Active()
{
    if (th::CurrentVariant() != th::Variant::Graphite) return false;
    const th::Spec& sp = th::CurrentSpec();
    return sp.gg != nullptr && sp.gg->deco.caliper;
}

void EditorChrome::SetCaliperEnabled(bool on) { g_caliperOn = on; }
bool EditorChrome::CaliperEnabled() { return g_caliperOn; }

float EditorChrome::LogoSizePx()
{
    return ui::Px(th::CurrentVariant() == th::Variant::Graphite ? 16.0f : 20.0f);
}

void EditorChrome::NoteLogoRect(ImVec2 mn, ImVec2 mx)
{
    g_logo = ImRect(mn, mx);
    g_haveLogo = true;
}

ImRect EditorChrome::LogoRectScreen()
{
    if (g_haveLogo) return g_logo;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float sz = LogoSizePx();
    const float x = (vp ? vp->Pos.x : 0.0f) + ui::Px(8.0f);
    const float y = (vp ? vp->Pos.y : 0.0f) + (ui::Px(th::size::kTitleBarH) - sz) * 0.5f;
    return ImRect(ImVec2(x, y), ImVec2(x + sz, y + sz));
}

bool EditorChrome::FocusedPanelSpan(float& x0, float& x1, bool& focusInWindow)
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* nav = g.NavWindow;
    if (nav && !g.NavWindowingTarget)
    {
        ImGuiWindow* r = nav->RootWindow ? nav->RootWindow : nav;
        const bool skip = (r->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Modal | ImGuiWindowFlags_ChildMenu)) != 0
                       || (r->Name && r->Name[0] == '#' && r->Name[1] == '#')
                       || r->Viewport != ImGui::GetMainViewport();
        if (!skip)
        {
            if (r->DockNode && r->DockIsActive)
            {
                g_spanX0 = r->DockNode->Pos.x;
                g_spanX1 = r->DockNode->Pos.x + r->DockNode->Size.x;
            }
            else
            {
                g_spanX0 = r->Pos.x;
                g_spanX1 = r->Pos.x + r->Size.x;
            }
            g_haveSpan = true;
            g_spanFocused = true;
        }
    }
    else if (!nav)
        g_spanFocused = false;   // どのパネルにもフォーカスが無い間は、直前の範囲を減光で残す
    x0 = g_spanX0; x1 = g_spanX1; focusInWindow = g_spanFocused;
    return g_haveSpan;
}

void EditorChrome::DrawTitleBar(const ImRect& bar, ImDrawList* dl)
{
    if (!Active() || !dl || bar.GetWidth() <= 0.0f || bar.GetHeight() <= 0.0f) return;
    ui::deco::ScaleRuler(dl, bar.Min, bar.Max);
    if (!g_caliperOn) return;
    float x0 = 0.0f, x1 = 0.0f; bool focused = true;
    if (!FocusedPanelSpan(x0, x1, focused)) return;
    // 範囲はタイトルバーの外へ出さない。スライド（kCaliperSlide）は端ごとに行き先へ寄る。
    x0 = (std::max)(x0, bar.Min.x);
    x1 = (std::min)(x1, bar.Max.x);
    const float ax0 = ui::anim::AnimatePx(ImGui::GetID("##caliperX0"), x0, ui::anim::dur::kCaliperSlide);
    const float ax1 = ui::anim::AnimatePx(ImGui::GetID("##caliperX1"), x1, ui::anim::dur::kCaliperSlide);
    // EndMenuBar 後の窓の InnerClipRect はメニューバーの下から始まるため、intersect すると高さ 0 になって全部切れる。
    dl->PushClipRect(bar.Min, bar.Max, false);
    ui::deco::Caliper(dl, ax0, ax1, bar.Max.y, focused);
    dl->PopClipRect();
}

} // namespace dx12e
