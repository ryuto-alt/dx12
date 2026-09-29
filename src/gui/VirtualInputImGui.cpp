#include "gui/VirtualInputImGui.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui_internal.h>
#pragma warning(pop)

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace dx12e::vinput_gui
{

// ---------------------------------------------------------------------------
// VK → ImGuiKey
// ---------------------------------------------------------------------------
ImGuiKey VkToImGuiKey(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    if (vk >= 0x70 && vk <= 0x7B) return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - 0x70));     // F1..F12
    if (vk >= 0x7C && vk <= 0x87) return static_cast<ImGuiKey>(ImGuiKey_F13 + (vk - 0x7C));    // F13..F24
    if (vk >= 0x60 && vk <= 0x69) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - 0x60));
    switch (vk)
    {
    case 0x0D: return ImGuiKey_Enter;
    case 0x1B: return ImGuiKey_Escape;
    case 0x09: return ImGuiKey_Tab;
    case 0x20: return ImGuiKey_Space;
    case 0x08: return ImGuiKey_Backspace;
    case 0x2E: return ImGuiKey_Delete;
    case 0x2D: return ImGuiKey_Insert;
    case 0x24: return ImGuiKey_Home;
    case 0x23: return ImGuiKey_End;
    case 0x21: return ImGuiKey_PageUp;
    case 0x22: return ImGuiKey_PageDown;
    case 0x25: return ImGuiKey_LeftArrow;
    case 0x26: return ImGuiKey_UpArrow;
    case 0x27: return ImGuiKey_RightArrow;
    case 0x28: return ImGuiKey_DownArrow;
    case 0x14: return ImGuiKey_CapsLock;
    case 0x6A: return ImGuiKey_KeypadMultiply;
    case 0x6B: return ImGuiKey_KeypadAdd;
    case 0x6D: return ImGuiKey_KeypadSubtract;
    case 0x6E: return ImGuiKey_KeypadDecimal;
    case 0x6F: return ImGuiKey_KeypadDivide;
    case 0xBA: return ImGuiKey_Semicolon;
    case 0xBB: return ImGuiKey_Equal;
    case 0xBC: return ImGuiKey_Comma;
    case 0xBD: return ImGuiKey_Minus;
    case 0xBE: return ImGuiKey_Period;
    case 0xBF: return ImGuiKey_Slash;
    case 0xC0: return ImGuiKey_GraveAccent;
    case 0xDB: return ImGuiKey_LeftBracket;
    case 0xDC: return ImGuiKey_Backslash;
    case 0xDD: return ImGuiKey_RightBracket;
    case 0xDE: return ImGuiKey_Apostrophe;
    default:   return ImGuiKey_None;
    }
}

// ---------------------------------------------------------------------------
// ImGui へ流し込む
// ---------------------------------------------------------------------------
void ApplyFrame(ImGuiIO& io, const vinput::Frame& frame, const ApplyContext& ctx)
{
    // 実マウスの WindowFromPoint の代わり。仮想ポインタは常にメインビューポート上にある。
    if (ctx.mainViewportId != 0)
        io.AddMouseViewportEvent(ctx.mainViewportId);

    for (const vinput::Event& e : frame)
    {
        switch (e.type)
        {
        case vinput::Event::Type::MousePos:
        {
            const vinput::ImGuiPoint p = vinput::ClientToImGui(
                e.x, e.y, ctx.viewportPos.x, ctx.viewportPos.y, ctx.displaySize.x, ctx.displaySize.y);
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(p.x, p.y);
            break;
        }
        case vinput::Event::Type::MouseButton:
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMouseButtonEvent(e.code, e.down);
            break;
        case vinput::Event::Type::MouseWheel:
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMouseWheelEvent(e.x, e.y);
            break;
        case vinput::Event::Type::Key:
        {
            // 修飾キーは ImGuiMod_* と左右キーの両方へ（Win32 バックエンドも両方送っている）。
            if (e.code == vinput::kVkControl)
            {
                io.AddKeyEvent(ImGuiMod_Ctrl, e.down);
                io.AddKeyEvent(ImGuiKey_LeftCtrl, e.down);
            }
            else if (e.code == vinput::kVkShift)
            {
                io.AddKeyEvent(ImGuiMod_Shift, e.down);
                io.AddKeyEvent(ImGuiKey_LeftShift, e.down);
            }
            else if (e.code == vinput::kVkMenu)
            {
                io.AddKeyEvent(ImGuiMod_Alt, e.down);
                io.AddKeyEvent(ImGuiKey_LeftAlt, e.down);
            }
            else if (e.code == vinput::kVkLWin)
            {
                io.AddKeyEvent(ImGuiMod_Super, e.down);
                io.AddKeyEvent(ImGuiKey_LeftSuper, e.down);
            }
            else
            {
                const ImGuiKey k = VkToImGuiKey(e.code);
                if (k != ImGuiKey_None) io.AddKeyEvent(k, e.down);
            }
            if (ctx.keySink) ctx.keySink(e.code, e.down);
            break;
        }
        case vinput::Event::Type::Char:
            io.AddInputCharacter(static_cast<unsigned int>(e.code));
            break;
        }
    }
}

int InputQueueSize()
{
    ImGuiContext* g = ImGui::GetCurrentContext();
    return g ? g->InputEventsQueue.Size : 0;
}

void TruncateInputQueue(int keep)
{
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g || keep < 0) return;
    if (keep < g->InputEventsQueue.Size) g->InputEventsQueue.resize(keep);
}

void SetImGuiVirtualMode(bool on)
{
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return;
    ImGuiIO& io = ImGui::GetIO();
    if (on)
    {
        // ★OS のカーソル形状（SetCursor）を ImGui に触らせない。
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.WantSetMousePos = false;
        // 実フォーカスが無くても ImGui は「入力を受け付ける」状態のままにする。
        io.AddFocusEvent(true);
        // これまでの実マウスの状態を捨てる（押しっぱなしのボタン / 直前の実カーソル位置でホバーしたまま、
        // を避ける）。AI が最初に動かすまで、どのウィジェットもホバーされない。
        for (int b = 0; b < 5; ++b) io.AddMouseButtonEvent(b, false);
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    else
    {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    }
}

// ---------------------------------------------------------------------------
// 仮想カーソル
// ---------------------------------------------------------------------------
namespace
{
struct Ripple { float x, y; double t0; int button; };
std::vector<Ripple> g_ripples;
uint64_t g_seenClickSeq = 0;
int      g_lastPrimCount = 0;

constexpr double kRippleLife = 0.55;

ImU32 ButtonColor(int button, float alpha)
{
    const int a = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    switch (button)
    {
    case 1:  return IM_COL32(255, 160, 40, a);    // 右 = 橙
    case 2:  return IM_COL32(80, 220, 120, a);    // 中 = 緑
    default: return IM_COL32(70, 160, 255, a);    // 左 = 青
    }
}
} // namespace

void ResetVirtualCursorState()
{
    g_ripples.clear();
    g_seenClickSeq = 0;
    g_lastPrimCount = 0;
}

int LastCursorPrimCount() { return g_lastPrimCount; }

void DrawVirtualCursor()
{
    g_lastPrimCount = 0;
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return;
    const vinput::Queue::Applied& st = vinput::Global().State();
    if (!st.hasPos) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);   // ★メインビューポート指定（引数なしは別 OS 窓へ描く罠）
    const double now = ImGui::GetTime();
    const float dk = ImGui::GetStyle().FontScaleDpi > 0.0f ? ImGui::GetStyle().FontScaleDpi : 1.0f;   // 表示倍率（仮想カーソルの大きさ）

    const vinput::ImGuiPoint pos = vinput::ClientToImGui(
        st.x, st.y, vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);
    const ImVec2 tip(pos.x, pos.y);

    // 新しいクリックがあれば波紋を起こす（押した位置は Pump 時点のポインタ位置）。
    if (st.clickSeq != g_seenClickSeq)
    {
        g_seenClickSeq = st.clickSeq;
        const vinput::ImGuiPoint cp = vinput::ClientToImGui(
            st.clickX, st.clickY, vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);
        g_ripples.push_back({cp.x, cp.y, now, st.clickButton});
        if (g_ripples.size() > 16) g_ripples.erase(g_ripples.begin());
    }

    // 波紋: 半径が広がりながら消える二重リング。
    for (size_t i = 0; i < g_ripples.size();)
    {
        const float age = static_cast<float>((now - g_ripples[i].t0) / kRippleLife);
        if (age >= 1.0f) { g_ripples.erase(g_ripples.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
        const float ease = 1.0f - (1.0f - age) * (1.0f - age);
        const float r = (6.0f + 26.0f * ease) * dk;
        const float alpha = 1.0f - age;
        const ImVec2 c(g_ripples[i].x, g_ripples[i].y);
        dl->AddCircle(c, r, ButtonColor(g_ripples[i].button, alpha), 32, 3.0f * dk);
        dl->AddCircle(c, r * 0.55f, ButtonColor(g_ripples[i].button, alpha * 0.7f), 24, 2.0f * dk);
        g_lastPrimCount += 2;
        ++i;
    }

    // 押している間は先端に塗りつぶしの円（ドラッグ中だと分かる）。
    for (int b = 0; b < 3; ++b)
    {
        if (!st.button[b]) continue;
        dl->AddCircleFilled(tip, 9.0f * dk, ButtonColor(b, 0.35f), 20);
        dl->AddCircle(tip, 9.0f * dk, ButtonColor(b, 0.95f), 20, 2.0f * dk);
        g_lastPrimCount += 2;
    }

    // 矢印本体（白地に黒縁。どんな背景でも見える）。先端が (tip) に来る。
    const float s = 1.25f * dk;
    const ImVec2 arrow[7] = {
        ImVec2(0.0f, 0.0f),   ImVec2(0.0f, 17.0f),  ImVec2(4.2f, 13.4f), ImVec2(7.4f, 20.6f),
        ImVec2(10.2f, 19.4f), ImVec2(7.0f, 12.4f),  ImVec2(12.0f, 12.4f),
    };
    ImVec2 pts[7];
    for (int i = 0; i < 7; ++i) pts[i] = ImVec2(tip.x + arrow[i].x * s, tip.y + arrow[i].y * s);
    dl->AddConcavePolyFilled(pts, 7, IM_COL32(255, 255, 255, 245));     // 矢印全体（凹多角形）
    dl->AddPolyline(pts, 7, IM_COL32(15, 15, 20, 255), ImDrawFlags_Closed, 1.6f * dk);
    g_lastPrimCount += 2;

    // 「AI」タグ。人間のカーソルと見分けが付くように。
    const char* tag = "AI";
    const ImVec2 ts = ImGui::CalcTextSize(tag);
    const ImVec2 t0(tip.x + 14.0f * s, tip.y + 16.0f * s);
    const ImVec2 t1(t0.x + ts.x + 10.0f * dk, t0.y + ts.y + 4.0f * dk);
    dl->AddRectFilled(t0, t1, IM_COL32(70, 130, 255, 235), 5.0f * dk);
    dl->AddText(ImVec2(t0.x + 5.0f * dk, t0.y + 2.0f * dk), IM_COL32(255, 255, 255, 255), tag);
    g_lastPrimCount += 2;
}

// ---------------------------------------------------------------------------
// find
// ---------------------------------------------------------------------------
namespace
{
std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string DisplayTitle(const char* name)
{
    std::string s(name ? name : "");
    const size_t hash = s.find("##");
    if (hash != std::string::npos) s.erase(hash);
    return s;
}
} // namespace

bool MatchLabel(const std::string& text, const std::string& label, bool contains)
{
    if (label.empty()) return true;
    const std::string a = Lower(text), b = Lower(label);
    return contains ? (a.find(b) != std::string::npos) : (a == b);
}

std::vector<WindowInfo> CollectWindows(bool includeChildren)
{
    std::vector<WindowInfo> out;
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return out;
    for (ImGuiWindow* w : g->Windows)
    {
        if (!w) continue;
        const bool child = (w->Flags & ImGuiWindowFlags_ChildWindow) != 0;
        if (child && !includeChildren) continue;
        WindowInfo wi;
        wi.name    = w->Name ? w->Name : "";
        wi.title   = DisplayTitle(w->Name);
        wi.x = w->Pos.x; wi.y = w->Pos.y; wi.w = w->Size.x; wi.h = w->Size.y;
        wi.visible   = w->Active && !w->Hidden;
        wi.collapsed = w->Collapsed;
        wi.isPopup   = (w->Flags & ImGuiWindowFlags_Popup) != 0;
        wi.isChild   = child;
        wi.docked    = w->DockNode != nullptr || w->DockIsActive;
        wi.dockTabVisible = w->DockTabIsVisible;
        // ドック内のタブ見出しの矩形（クリックでそのウィンドウが前面に来る）。
        //   タブは ImGuiTabBar が持つ「バー先頭からのオフセット + 幅」から復元する。
        if (w->DockIsActive && w->DockNode && w->DockNode->TabBar)
        {
            ImGuiTabBar* tb = w->DockNode->TabBar;
            const ImGuiTabItem* tab = ImGui::TabBarFindTabByID(tb, w->TabId);
            if (tab && tab->Width > 0.0f)
            {
                float x0 = tb->BarRect.Min.x + tab->Offset - tb->ScrollingAnim;
                float x1 = x0 + tab->Width;
                x0 = (std::max)(x0, tb->BarRect.Min.x);   // スクロールで見切れた分は落とす
                x1 = (std::min)(x1, tb->BarRect.Max.x);
                if (x1 > x0)
                {
                    wi.hasTabRect = true;
                    wi.tx = x0;
                    wi.ty = tb->BarRect.Min.y;
                    wi.tw = x1 - x0;
                    wi.th = tb->BarRect.GetHeight();
                }
            }
        }
        const ImGuiWindow* root = w->RootWindow;
        wi.focused = g->NavWindow && g->NavWindow->RootWindow == root && !child;
        wi.hovered = g->HoveredWindow && g->HoveredWindow->RootWindow == root && !child;
        out.push_back(std::move(wi));
    }
    return out;
}

HoverInfo QueryHoverInfo()
{
    HoverInfo h;
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return h;
    if (g->HoveredWindow && g->HoveredWindow->RootWindow)
        h.hoveredWindow = g->HoveredWindow->RootWindow->Name ? g->HoveredWindow->RootWindow->Name : "";
    if (g->NavWindow && g->NavWindow->RootWindow)
        h.focusedWindow = g->NavWindow->RootWindow->Name ? g->NavWindow->RootWindow->Name : "";
    const ImGuiIO& io = g->IO;
    h.wantCaptureMouse    = io.WantCaptureMouse;
    h.wantCaptureKeyboard = io.WantCaptureKeyboard;
    h.wantTextInput       = io.WantTextInput;
    h.activeId  = g->ActiveId;
    h.hoveredId = g->HoveredId;
    return h;
}


// ---------------------------------------------------------------------------
// アンカー登録
// ---------------------------------------------------------------------------
namespace
{
std::string CurrentRootWindowName()
{
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g || !g->CurrentWindow) return {};
    const ImGuiWindow* root = g->CurrentWindow->RootWindow ? g->CurrentWindow->RootWindow : g->CurrentWindow;
    return root->Name ? root->Name : "";
}
} // namespace

void AnchorLastItem(const char* kind, const char* label)
{
    if (!vinput::Enabled() || !label || !*label) return;
    if (!ImGui::GetCurrentContext()) return;
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    if (!ImGui::IsRectVisible(mn, mx)) return;   // スクロール/クリップで見えていない要素は登録しない
    vinput::Anchor a;
    a.kind   = kind ? kind : "item";
    a.label  = label;
    a.window = CurrentRootWindowName();
    a.x0 = mn.x; a.y0 = mn.y; a.x1 = mx.x; a.y1 = mx.y;
    vinput::Global().AddAnchor(std::move(a));
}

void AnchorProperty(const char* label, ImVec2 labelMin, ImVec2 labelMax, ImVec2 valueMin, ImVec2 valueMax)
{
    if (!vinput::Enabled() || !label || !*label) return;
    if (!ImGui::GetCurrentContext()) return;
    if (!ImGui::IsRectVisible(valueMin, valueMax)) return;
    vinput::Anchor a;
    a.kind   = "property";
    a.label  = label;
    a.window = CurrentRootWindowName();
    a.x0 = valueMin.x; a.y0 = valueMin.y; a.x1 = valueMax.x; a.y1 = valueMax.y;
    a.hasAux = true;
    a.ax0 = labelMin.x; a.ay0 = labelMin.y; a.ax1 = labelMax.x; a.ay1 = labelMax.y;
    vinput::Global().AddAnchor(std::move(a));
}
} // namespace dx12e::vinput_gui
