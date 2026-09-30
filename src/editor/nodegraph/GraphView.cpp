// ノードグラフ ImGui ビュー: 状態・入力・コマンド（描画は GraphViewDraw.cpp、ポップアップは GraphViewUi.cpp）。

#include "editor/nodegraph/GraphView.h"

#include "core/CpuScope.h"
#include "editor/EditorCommandTable.h"
#include "editor/UiWidgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace dx12e::ng
{

namespace
{
constexpr float kDragThresholdPx = 4.0f;
constexpr float kAutoPanEdgePx = 26.0f;

float Dist(ImVec2 a, ImVec2 b) { const float dx = a.x - b.x, dy = a.y - b.y; return std::sqrt(dx * dx + dy * dy); }

ImGuiKey KeyFromName(const std::string& k)
{
    if (k.size() == 1)
    {
        const char c = k[0];
        if (c >= 'A' && c <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
        if (c >= '0' && c <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
    }
    if (k.size() >= 2 && k[0] == 'F')
    {
        const int n = std::atoi(k.c_str() + 1);
        if (n >= 1 && n <= 12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (n - 1));
    }
    if (k == "ESC") return ImGuiKey_Escape;
    if (k == "DEL") return ImGuiKey_Delete;
    if (k == "ENTER") return ImGuiKey_Enter;
    if (k == "TAB") return ImGuiKey_Tab;
    if (k == "SPACE") return ImGuiKey_Space;
    if (k == "GRAVE") return ImGuiKey_GraveAccent;
    if (k == "UP") return ImGuiKey_UpArrow;
    if (k == "DOWN") return ImGuiKey_DownArrow;
    if (k == "LEFT") return ImGuiKey_LeftArrow;
    if (k == "RIGHT") return ImGuiKey_RightArrow;
    if (k == "HOME") return ImGuiKey_Home;
    if (k == "END") return ImGuiKey_End;
    return ImGuiKey_None;
}

bool ChordPressed(const std::string& chord)
{
    if (chord.empty()) return false;
    const cmd::ChordSpec sp = cmd::ParseChordSpec(chord);
    if (!sp.valid) return false;
    const ImGuiKey key = KeyFromName(sp.key);
    if (key == ImGuiKey_None) return false;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl != sp.ctrl || io.KeyShift != sp.shift || io.KeyAlt != sp.alt) return false;
    return ImGui::IsKeyPressed(key, false);
}

const char* const kKeyCommands[] = {
    "edit.undo", "edit.redo", "edit.copy", "edit.paste", "edit.duplicate", "edit.delete", "edit.focus", "edit.selectNone",
    "graph.selectAll", "graph.frameAll", "graph.palette", "graph.comment", "graph.selectUpstream", "graph.selectDownstream",
    "graph.alignLeft", "graph.alignRight", "graph.alignTop", "graph.alignBottom", "graph.alignCenterH", "graph.alignCenterV",
    "graph.distributeH", "graph.distributeV", "graph.resetZoom",
};
} // namespace

GraphView::GraphView() = default;

void GraphView::SetDocument(GraphDocument* doc)
{
    m_doc = doc;
    m_sel.Clear();
    m_anim.clear();
    m_pinAnim.clear();
    m_z.clear();
    m_mode = Mode::Idle;
    if (m_doc)
    {
        m_doc->Layout().SetMeasure([this](const std::string& s, bool title)
        {
            ImFont* f = title ? FontBold() : FontBody();
            if (!f) return GraphLayout::ApproxWidth(s, title);
            const LayoutConfig& c = m_doc->Layout().Config();
            // ★DPI に依らない一定の大きさ（units そのまま）で測る: 倍率ごとにフォントのヒンティングで幅が変わると、
            //   同じグラフのノード幅が 100% と 150% で違ってしまう（保存した位置に対して重なりが変わる）。
            //   描画は units × scale の大きさなので幅は線形に伸びる（丸め 8 単位 + 余白で吸収）。
            const float px = title ? c.titleUnits : c.textUnits;
            return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s.c_str()).x + 2.0f;
        });
    }
}

ImFont* GraphView::FontBody() const { return theme::g_fonts.body ? theme::g_fonts.body : ImGui::GetFont(); }
ImFont* GraphView::FontBold() const { return theme::g_fonts.bold ? theme::g_fonts.bold : FontBody(); }
ImFont* GraphView::FontMono() const { return theme::g_fonts.mono ? theme::g_fonts.mono : FontBody(); }

float GraphView::Now() const
{
    return m_set.fixedTime >= 0.0f ? m_set.fixedTime : static_cast<float>(m_time);
}

float GraphView::TitleHeightFor(float zoom) const
{
    return std::min(72.0f, std::max(26.0f, 22.0f / std::max(0.05f, zoom * m_dpi)));
}

const char* GraphView::ModeName() const
{
    switch (m_mode)
    {
    case Mode::Idle: return "idle";
    case Mode::Panning: return "pan";
    case Mode::BoxSelect: return "box";
    case Mode::MoveItems: return m_dragStarted ? "move" : "press";
    case Mode::ResizeComment: return "resize";
    case Mode::LinkDrag: return "link";
    case Mode::CutStroke: return "cut";
    case Mode::SlotDrag: return "slot";
    case Mode::MinimapDrag: return "minimap";
    }
    return "?";
}

std::string GraphView::StatusLine() const
{
    char buf[320];
    const size_t undo = m_doc ? m_doc->History().UndoDepth() : 0;
    const size_t redo = m_doc ? m_doc->History().RedoDepth() : 0;
    std::snprintf(buf, sizeof(buf), "nodes=%d edges=%d selN=%d selC=%d undo=%zu redo=%zu zoom=%.2f mode=%s hover=%d pal=%d ctx=%d drawn=%d/%d ms=%.2f",
                  m_stats.nodes, m_stats.edges, static_cast<int>(m_sel.nodes.size()), static_cast<int>(m_sel.comments.size()),
                  undo, redo, m_zoom, ModeName(), static_cast<int>(m_hover.kind), m_paletteOpen ? 1 : 0, m_ctxOpen ? 1 : 0,
                  m_stats.drawnNodes, m_stats.drawnEdges, m_stats.cpuMs);
    return buf;
}

// ---------------------------------------------------------------------------
// ビュー
// ---------------------------------------------------------------------------
void GraphView::SetViewState(const ViewState& v, bool animate)
{
    m_panTarget = v.pan;
    m_zoomTarget = ClampZoom(v.zoom);
    if (animate && m_set.animate)
    {
        m_framing = true;
        m_sPanX.Snap(m_pan.x); m_sPanY.Snap(m_pan.y); m_sZoom.Snap(m_zoom);
    }
    else
    {
        m_pan = m_panTarget; m_zoom = m_zoomTarget; m_framing = false; m_zoomAnimating = false;
    }
}

void GraphView::FrameAll(bool animate)
{
    if (!m_doc) return;
    const Rect b = m_doc->ContentBounds();
    if (IsEmptyRect(b)) { SetViewState(ViewState{Vec2(0, 0), 1.0f}, animate); return; }
    Viewport v = m_vp;
    v.origin = Vec2(m_canvasMin.x, m_canvasMin.y);
    v.size = Vec2(m_canvasMax.x - m_canvasMin.x, m_canvasMax.y - m_canvasMin.y);
    v.dpi = m_dpi;
    v.FitRect(b, ui::Px(60.0f), 1.0f);
    SetViewState(ViewState{v.pan, v.zoom}, animate);
}

void GraphView::FrameSelection(bool animate)
{
    if (!m_doc) return;
    if (m_sel.Empty()) { FrameAll(animate); return; }
    const Rect b = m_doc->BoundsOf(m_sel);
    if (IsEmptyRect(b)) return;
    Viewport v = m_vp;
    v.origin = Vec2(m_canvasMin.x, m_canvasMin.y);
    v.size = Vec2(m_canvasMax.x - m_canvasMin.x, m_canvasMax.y - m_canvasMin.y);
    v.dpi = m_dpi;
    v.FitRect(b, ui::Px(90.0f), 1.25f);
    SetViewState(ViewState{v.pan, v.zoom}, animate);
}

void GraphView::ResetZoom(bool animate)
{
    // 画面中心を保ったまま 100% へ
    Viewport v = m_vp;
    v.origin = Vec2(m_canvasMin.x, m_canvasMin.y);
    v.size = Vec2(m_canvasMax.x - m_canvasMin.x, m_canvasMax.y - m_canvasMin.y);
    v.dpi = m_dpi; v.pan = m_pan; v.zoom = m_zoom;
    v.ZoomAt(Vec2(v.origin.x + v.size.x * 0.5f, v.origin.y + v.size.y * 0.5f), 1.0f);
    SetViewState(ViewState{v.pan, v.zoom}, animate);
}

void GraphView::CenterOn(Vec2 g, bool animate)
{
    const float s = m_zoomTarget * m_dpi;
    const float w = (m_canvasMax.x - m_canvasMin.x), h = (m_canvasMax.y - m_canvasMin.y);
    SetViewState(ViewState{Vec2(g.x - w * 0.5f / s, g.y - h * 0.5f / s), m_zoomTarget}, animate);
}

void GraphView::SettleAnimations()
{
    if (m_framing || m_zoomAnimating)
    {
        if (m_framing) { m_pan = m_panTarget; m_zoom = m_zoomTarget; }
        if (m_zoomAnimating) { Viewport v = m_vp; v.pan = m_pan; v.zoom = m_zoom; v.ZoomAt(m_zoomAnchor, m_zoomTarget); m_pan = v.pan; m_zoom = v.zoom; }
        m_framing = false; m_zoomAnimating = false;
    }
    for (auto& kv : m_anim)
    {
        const bool sel = m_sel.nodes.count(kv.first) != 0;
        kv.second.hover.Snap(kv.first == m_hoverNode ? 1.0f : 0.0f);
        kv.second.sel.Snap(sel ? 1.0f : 0.0f);
        kv.second.lift.Snap(0.0f);
    }
    for (auto& kv : m_pinAnim) kv.second.Snap(0.0f);
    m_wireHover.Snap(m_hoverEdgeValid ? 1.0f : 0.0f);
}

void GraphView::UpdateViewAnim(float dt)
{
    if (m_framing)
    {
        if (!m_set.animate) { m_pan = m_panTarget; m_zoom = m_zoomTarget; m_framing = false; return; }
        m_sPanX.Step(m_panTarget.x, dt, 20.0f, 1.0f);
        m_sPanY.Step(m_panTarget.y, dt, 20.0f, 1.0f);
        m_sZoom.Step(m_zoomTarget, dt, 20.0f, 1.0f);
        m_pan = Vec2(m_sPanX.value, m_sPanY.value);
        m_zoom = ClampZoom(m_sZoom.value);
        if (m_sPanX.Settled(m_panTarget.x) && m_sPanY.Settled(m_panTarget.y) && m_sZoom.Settled(m_zoomTarget))
        {
            m_pan = m_panTarget; m_zoom = m_zoomTarget; m_framing = false;
        }
    }
    else if (m_zoomAnimating)
    {
        if (!m_set.animate) m_sZoom.Snap(m_zoomTarget); else m_sZoom.Step(m_zoomTarget, dt, 24.0f, 1.0f);
        Viewport v = m_vp;
        v.pan = m_pan; v.zoom = m_zoom;
        v.ZoomAt(m_zoomAnchor, m_sZoom.value);
        m_pan = v.pan; m_zoom = v.zoom;
        if (m_sZoom.Settled(m_zoomTarget)) m_zoomAnimating = false;
    }
}

// ---------------------------------------------------------------------------
// フレーム
// ---------------------------------------------------------------------------
void GraphView::Draw(const char* strId, ImVec2 size)
{
    if (!m_doc) return;
    f32 cpuMs = 0.0f;
    {
        CpuScopeTimer timer(&cpuMs);
        const ImGuiIO& io = ImGui::GetIO();
        m_dt = std::min(io.DeltaTime, 0.1f);
        m_time += static_cast<double>(m_dt);
        m_dpi = theme::Scale();
        m_tok = MakeGraphTokens();
        if (m_dpi != m_lastLayoutDpi) { m_doc->Layout().Invalidate(); m_lastLayoutDpi = m_dpi; }

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (size.x <= 0.0f) size.x = avail.x;
        if (size.y <= 0.0f) size.y = avail.y;
        size.x = std::max(size.x, 64.0f);
        size.y = std::max(size.y, 64.0f);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(strId, size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        m_canvasMin = p0;
        m_canvasMax = ImVec2(p0.x + size.x, p0.y + size.y);
        m_windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        m_hovered = hovered;

        // 1 行の高さ（今の倍率で。中央寄せに使う）
        {
            const LayoutConfig& c = m_doc->Layout().Config();
            const float scale = m_zoom * m_dpi;
            m_lhLabel = FontBody()->CalcTextSizeA(std::max(1.0f, std::floor(c.textUnits * scale + 0.5f)), FLT_MAX, 0.0f, "Ag").y;
            m_lhTitle = FontBold()->CalcTextSizeA(std::max(1.0f, std::floor(c.titleUnits * scale + 0.5f)), FLT_MAX, 0.0f, "Ag").y;
            m_lhMono  = FontMono()->CalcTextSizeA(std::max(1.0f, std::floor(c.textUnits * scale + 0.5f)), FLT_MAX, 0.0f, "0.").y;
        }

        m_vp.origin = Vec2(m_canvasMin.x, m_canvasMin.y);
        m_vp.size = Vec2(size.x, size.y);
        m_vp.dpi = m_dpi;
        UpdateViewAnim(m_dt);
        m_vp.pan = m_pan;
        m_vp.zoom = m_zoom;

        PruneSelection();
        HandleInput(hovered, active);
        AutoPan(m_dt);
        m_vp.pan = m_pan;
        m_vp.zoom = m_zoom;
        m_vp.Scale();
        Render();
        m_stats.zoom = m_zoom;
    }
    m_stats.cpuMs = cpuMs;

    // ポップアップ（キャンバスの外側の ImGui 窓）
    DrawPalette();
    DrawContextMenus();
    DrawEditPopups();
    m_keyboardActive = m_windowFocused || m_paletteOpen || m_ctxOpen;
}

Vec2 GraphView::SnapPos(Vec2 p) const { return m_set.snap ? SnapVec(p, m_set.snapStep) : p; }

void GraphView::PruneSelection()
{
    if (!m_doc) return;
    IGraphModel& m = m_doc->Model();
    for (auto it = m_sel.nodes.begin(); it != m_sel.nodes.end();)
        it = m.FindNode(*it) ? std::next(it) : m_sel.nodes.erase(it);
    for (auto it = m_sel.comments.begin(); it != m_sel.comments.end();)
        it = m_doc->FindComment(*it) ? std::next(it) : m_sel.comments.erase(it);
}

HitParams GraphView::MakeHitParams() const
{
    const float s = std::max(0.01f, m_vp.Scale());
    HitParams hp;
    hp.pinRadius = std::max(9.0f, 7.0f * m_dpi / s);
    hp.edgeTol = std::max(6.0f, 5.0f * m_dpi / s);
    hp.resizeSize = std::max(14.0f, 12.0f * m_dpi / s);
    hp.commentTitleH = TitleHeightFor(m_zoom);
    hp.slots = m_zoom >= 0.6f;
    hp.z = &m_z;
    return hp;
}

Hit GraphView::HitTestAt(ImVec2 screen)
{
    return m_doc->HitTest(m_vp.ToGraph(Vec2(screen.x, screen.y)), MakeHitParams());
}

void GraphView::ShowBringToFront(const Selection& s)
{
    for (NodeId id : s.nodes) m_z[id] = ++m_zCounter;
}

void GraphView::SelectAll()
{
    m_sel.Clear();
    for (NodeId id : m_doc->Model().Nodes()) m_sel.nodes.insert(id);
    for (const Comment& c : m_doc->Comments()) m_sel.comments.insert(c.id);
    ++m_selRev;
}

void GraphView::SelectClick(NodeId id, bool add, bool toggle)
{
    if (toggle)
    {
        if (!m_sel.nodes.erase(id)) m_sel.nodes.insert(id);
    }
    else if (add) m_sel.nodes.insert(id);
    else if (!m_sel.Has(id)) { m_sel.Clear(); m_sel.nodes.insert(id); }
    ++m_selRev;
}

// ---------------------------------------------------------------------------
// 入力
// ---------------------------------------------------------------------------
bool GraphView::MinimapRect(ImVec2& mn, ImVec2& mx) const
{
    if (!m_set.showMinimap) return false;
    const float w = ui::Px(190.0f), h = ui::Px(124.0f), m = ui::Px(12.0f);
    if (m_canvasMax.x - m_canvasMin.x < w * 2.0f || m_canvasMax.y - m_canvasMin.y < h * 1.6f) return false;
    mx = ImVec2(m_canvasMax.x - m, m_canvasMax.y - m);
    mn = ImVec2(mx.x - w, mx.y - h);
    return true;
}

void GraphView::MinimapCenterOn(ImVec2 mouse)
{
    ImVec2 mn, mx;
    if (!MinimapRect(mn, mx)) return;
    Rect b = m_doc->ContentBounds();
    if (IsEmptyRect(b)) return;
    b = b.Expanded(80.0f);
    Rect vis = m_vp.VisibleGraphRect();
    b.Add(vis);
    const float sc = std::min((mx.x - mn.x) / std::max(1.0f, b.Width()), (mx.y - mn.y) / std::max(1.0f, b.Height()));
    const Vec2 cGraph = b.Center();
    const Vec2 cMap((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    const Vec2 g((mouse.x - cMap.x) / sc + cGraph.x, (mouse.y - cMap.y) / sc + cGraph.y);
    const float s = m_zoom * m_dpi;
    m_pan = Vec2(g.x - (m_canvasMax.x - m_canvasMin.x) * 0.5f / s, g.y - (m_canvasMax.y - m_canvasMin.y) * 0.5f / s);
    m_panTarget = m_pan;
    m_framing = false;
}

void GraphView::BeginLinkFrom(PinRef pin, bool detachExisting)
{
    m_mode = Mode::LinkDrag;
    m_linkDetach = false;
    m_linkHoverAny = false;
    m_linkHoverValid = false;
    if (detachExisting && !pin.output)
    {
        if (const Edge* e = m_doc->Model().FindEdgeTo(pin))
        {
            m_linkDetach = true;
            m_linkDetached = *e;
            m_linkFrom = e->from;
            m_linkFromOutput = true;
            return;
        }
    }
    m_linkFrom = pin;
    m_linkFromOutput = pin.output;
}

void GraphView::FinishLink(bool dropped)
{
    IGraphModel& m = m_doc->Model();
    GraphHistory& h = m_doc->History();
    bool handled = false;
    if (dropped && m_linkHoverAny && m_linkHoverValid)
    {
        const PinRef out = m_linkFromOutput ? m_linkFrom : m_linkHoverPin;
        const PinRef in  = m_linkFromOutput ? m_linkHoverPin : m_linkFrom;
        h.BeginGroup(m_linkDetach ? "ワイヤを付け替え" : "ワイヤを接続");
        if (m_linkDetach && !(in == m_linkDetached.to)) m_doc->Disconnect(m_linkDetached.to);
        m_doc->Connect(out, in);
        h.EndGroup();
        handled = true;
        // 繋いだ先のノードを前面へ
        m_z[in.node] = ++m_zCounter; m_z[out.node] = ++m_zCounter;
    }
    else if (dropped && m_linkHoverAny && !m_linkHoverValid)
    {
        handled = true;   // 不可の接続先に落とした: 何も起きない（元のワイヤは残る）
    }
    else if (dropped && m_linkDetach)
    {
        m_doc->Disconnect(m_linkDetached.to);   // 掴んだワイヤを空き地へ = 切断
        handled = true;
    }
    if (dropped && !handled)
    {
        // 出力 / 入力ピンから空き地へ = その位置に検索パレット（ドラッグ元のピンに合う候補だけ）
        m_palHasPin = true;
        m_palPin = m_linkFrom;
        m_palPinOutput = m_linkFromOutput;
        m_palFilter = PaletteFilter{};
        m_palFilter.hasPin = true;
        m_palFilter.pinIsOutput = m_linkFromOutput;
        PinType t = 0;
        if (const NodeData* n = m.FindNode(m_linkFrom.node))
        {
            if (m_linkFromOutput) t = m.ResolvedPinType(m_linkFrom);
            else if (m_linkFrom.index < n->desc->inputs.size()) t = n->desc->inputs[m_linkFrom.index].type;
        }
        m_palFilter.pinType = t;
        m_palGraphPos = m_linkDropG;
        m_palScreenPos = ImVec2(m_mouse.x, m_mouse.y);
        m_palQuery[0] = '\0';
        m_palCategory.clear();
        m_paletteRequest = true;
    }
    m_linkDetach = false;
    m_linkHoverAny = false;
    m_mode = Mode::Idle;
}

void GraphView::BeginMoveSession(NodeId primary)
{
    m_moveSel = m_sel;
    // コメントを動かすときは、中に丸ごと入っているノードも一緒に動かす
    for (CommentId cid : m_sel.comments)
        for (NodeId n : m_doc->NodesInsideComment(cid)) m_moveSel.nodes.insert(n);
    m_move = m_doc->BeginMove(m_moveSel);
    m_movePrimary = primary;
    if (primary)
    {
        if (const NodeData* n = m_doc->Model().FindNode(primary)) m_movePrimaryStart = n->pos;
    }
    else if (m_pressHit.kind == Hit::CommentTitle)
    {
        if (const Comment* c = m_doc->FindComment(m_pressHit.comment)) m_movePrimaryStart = c->rect.min;
    }
    ShowBringToFront(m_moveSel);
}

void GraphView::UpdateMoveSession()
{
    const Vec2 raw = m_mouseG - m_pressG;
    Vec2 delta = raw;
    const bool snap = m_set.snap != ImGui::GetIO().KeyShift;   // Shift で反転（スナップ ON なら解除 / OFF なら一時的に有効）
    if (snap) delta = SnapVec(m_movePrimaryStart + raw, m_set.snapStep) - m_movePrimaryStart;
    m_moveSnapped = snap;
    m_doc->UpdateMove(m_move, delta);
}

void GraphView::ApplyBoxSelection()
{
    const Vec2 a = m_pressG, b = m_mouseG;
    const Rect r(Vec2(std::min(a.x, b.x), std::min(a.y, b.y)), Vec2(std::max(a.x, b.x), std::max(a.y, b.y)));
    Selection s = m_boxBase;
    std::vector<NodeId> ns;
    m_doc->NodesInRect(r, false, ns);
    for (NodeId n : ns) s.nodes.insert(n);
    std::vector<CommentId> cs;
    m_doc->CommentsInRect(r, true, cs);
    for (CommentId c : cs) s.comments.insert(c);
    if (s.nodes != m_sel.nodes || s.comments != m_sel.comments) { m_sel = s; ++m_selRev; }
}

void GraphView::AutoPan(float dt)
{
    if (m_mode != Mode::MoveItems && m_mode != Mode::LinkDrag && m_mode != Mode::BoxSelect && m_mode != Mode::ResizeComment) return;
    if (m_mode == Mode::MoveItems && !m_dragStarted) return;
    const float edge = ui::Px(kAutoPanEdgePx);
    float dx = 0.0f, dy = 0.0f;
    if (m_mouse.x < m_canvasMin.x + edge) dx = -(m_canvasMin.x + edge - m_mouse.x);
    else if (m_mouse.x > m_canvasMax.x - edge) dx = m_mouse.x - (m_canvasMax.x - edge);
    if (m_mouse.y < m_canvasMin.y + edge) dy = -(m_canvasMin.y + edge - m_mouse.y);
    else if (m_mouse.y > m_canvasMax.y - edge) dy = m_mouse.y - (m_canvasMax.y - edge);
    if (dx == 0.0f && dy == 0.0f) return;
    // 端に近いほど速い（最大 ≒ 700 px/s）。画面 px → グラフ座標
    const float speed = 14.0f / std::max(1.0f, edge) * 60.0f;
    const float s = m_zoom * m_dpi;
    m_pan.x += dx * speed * dt / s * 8.0f;
    m_pan.y += dy * speed * dt / s * 8.0f;
    m_panTarget = m_pan;
    m_framing = false;
    m_zoomAnimating = false;
    m_vp.pan = m_pan;
    m_mouseG = m_vp.ToGraph(Vec2(m_mouse.x, m_mouse.y));
    if (m_mode == Mode::MoveItems) UpdateMoveSession();
    if (m_mode == Mode::BoxSelect) ApplyBoxSelection();
}

void GraphView::OpenSlotEditor(const Hit& hit, ImVec2 screen)
{
    const NodeData* n = m_doc->Model().FindNode(hit.pin.node);
    if (!n || hit.pin.index >= n->desc->inputs.size()) return;
    const PinDesc& pd = n->desc->inputs[hit.pin.index];
    m_editPin = hit.pin;
    m_editComp = hit.slotComp;
    m_editOld = n->values[hit.pin.index];
    m_editScreen = screen;
    if (pd.valueKind == ValueKind::Enum) m_editKind = EditKind::Enum;
    else if (pd.valueKind == ValueKind::Color) m_editKind = EditKind::Color;
    else
    {
        m_editKind = EditKind::Text;
        std::snprintf(m_editBuf, sizeof(m_editBuf), "%.6g", static_cast<double>(m_editOld.f[std::min(hit.slotComp, 3)]));
    }
    m_editRequest = true;
}

void GraphView::OpenContext(CtxKind kind, const Hit& hit, Vec2 g)
{
    m_ctxKind = kind;
    m_ctxHit = hit;
    m_ctxG = g;
    m_ctxRequest = true;
}

void GraphView::StartRename(CommentId id)
{
    const Comment* c = m_doc->FindComment(id);
    if (!c) return;
    m_renameComment = id;
    std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", c->title.c_str());
    m_renameRequest = true;
}

void GraphView::HandleInput(bool hovered, bool active)
{
    (void)active;
    ImGuiIO& io = ImGui::GetIO();
    m_mouse = io.MousePos;
    m_mouseG = m_vp.ToGraph(Vec2(m_mouse.x, m_mouse.y));
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    const bool anyPopup = m_paletteOpen || m_ctxOpen || m_editKind != EditKind::None || m_renameComment != 0;

    // ---- ホイール = カーソル中心ズーム ----
    if (hovered && io.MouseWheel != 0.0f && m_mode == Mode::Idle && !anyPopup)
    {
        const float f = std::pow(1.15f, io.MouseWheel);
        m_zoomTarget = ClampZoom((m_zoomAnimating ? m_zoomTarget : m_zoom) * f);
        m_zoomAnchor = Vec2(m_mouse.x, m_mouse.y);
        if (!m_zoomAnimating) m_sZoom.Snap(m_zoom);
        m_zoomAnimating = true;
        m_framing = false;
        // 直後の当たり判定が新しいズームに合うよう、この場で 1 ステップ進める
        UpdateViewAnim(m_dt);
        m_vp.pan = m_pan; m_vp.zoom = m_zoom;
        m_mouseG = m_vp.ToGraph(Vec2(m_mouse.x, m_mouse.y));
    }

    // ---- ホバー ----
    if ((m_mode == Mode::Idle || m_mode == Mode::LinkDrag) && hovered && !anyPopup)
    {
        m_doc->EnsureIndex();
        m_hover = HitTestAt(m_mouse);
    }
    else if (m_mode == Mode::Idle) m_hover = Hit{};
    m_hoverNode = 0;
    m_hoverPin = PinRef{};
    m_hoverEdgeValid = false;
    m_hoverComment = 0;
    if (m_mode == Mode::Idle || m_mode == Mode::LinkDrag)
    {
        switch (m_hover.kind)
        {
        case Hit::Node: case Hit::Slot: m_hoverNode = m_hover.node; if (m_hover.kind == Hit::Slot) m_hoverPin = m_hover.pin; break;
        case Hit::Pin: m_hoverNode = m_hover.node; m_hoverPin = m_hover.pin; break;
        case Hit::Edge: m_hoverEdge = m_hover.edge; m_hoverEdgeValid = true; break;
        case Hit::CommentTitle: case Hit::CommentResize: case Hit::CommentBody: m_hoverComment = m_hover.comment; break;
        default: break;
        }
    }

    // ---- Idle: 押下の開始 ----
    if (m_mode == Mode::Idle && hovered && !anyPopup)
    {
        const bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        {
            m_mode = Mode::Panning; m_pressButton = 2; m_pressPos = m_mouse; m_rmbMoved = true;
        }
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_mode = Mode::Panning; m_pressButton = 1; m_pressPos = m_mouse; m_rmbMoved = false; m_pressHit = m_hover;
            m_pressG = m_mouseG;
        }
        else if (dbl && ((m_hover.kind == Hit::Edge && m_doc->Model().RerouteTypeId()) || m_hover.kind == Hit::CommentTitle || m_hover.kind == Hit::Slot
                         || m_hover.kind == Hit::None || m_hover.kind == Hit::CommentBody))
        {
            // ダブルクリックが意味を持つ対象だけここで処理する。ノード / ピンなどは普通のクリックとして下へ流す
            //（素早く 2 回押してもドラッグ接続やクリック選択が死なない）。
            const Hit h = m_hover;
            if (h.kind == Hit::Edge && m_doc->Model().RerouteTypeId())
            {
                m_doc->InsertReroute(h.edge, SnapPos(m_mouseG));
            }
            else if (h.kind == Hit::CommentTitle) StartRename(h.comment);
            else if (h.kind == Hit::Slot) OpenSlotEditor(h, m_mouse);
            else if (h.kind == Hit::None || h.kind == Hit::CommentBody)
            {
                OpenPaletteAt(m_mouseG);
                m_palScreenPos = m_mouse;
            }
        }
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_pressPos = m_mouse; m_pressG = m_mouseG; m_pressButton = 0; m_dragStarted = false;
            m_pressHit = m_hover;
            ImVec2 mmn, mmx;
            if (MinimapRect(mmn, mmx) && m_mouse.x >= mmn.x && m_mouse.x <= mmx.x && m_mouse.y >= mmn.y && m_mouse.y <= mmx.y)
            {
                m_mode = Mode::MinimapDrag;
                MinimapCenterOn(m_mouse);
            }
            else if (ImGui::IsKeyDown(ImGuiKey_Space))
            {
                m_mode = Mode::Panning; m_pressButton = 0; m_rmbMoved = true;
            }
            else
            {
                const Hit& h = m_pressHit;
                switch (h.kind)
                {
                case Hit::Pin:
                    if (alt) { m_doc->DisconnectPin(h.pin); }
                    else
                    {
                        const bool connected = !h.pin.output && m_doc->Model().FindEdgeTo(h.pin) != nullptr;
                        BeginLinkFrom(h.pin, connected);
                    }
                    break;
                case Hit::Slot:
                {
                    const NodeData* n = m_doc->Model().FindNode(h.pin.node);
                    const PinDesc& pd = n->desc->inputs[h.pin.index];
                    if (pd.valueKind == ValueKind::Bool)
                    {
                        PinValue v = n->values[h.pin.index];
                        v.f[0] = v.f[0] != 0.0f ? 0.0f : 1.0f;
                        m_doc->SetPinValue(h.pin, v);
                    }
                    else if (pd.valueKind == ValueKind::Enum || pd.valueKind == ValueKind::Color) OpenSlotEditor(h, m_mouse);
                    else
                    {
                        m_slot = SlotDrag{};
                        m_slot.pin = h.pin; m_slot.comp = h.slotComp; m_slot.oldValue = n->values[h.pin.index];
                        m_slot.startValue = m_slot.oldValue.f[std::min(h.slotComp, 3)];
                        m_slot.pressPos = m_mouse;
                        m_mode = Mode::SlotDrag;
                    }
                    break;
                }
                case Hit::Node:
                {
                    const bool wasSel = m_sel.Has(h.node);
                    if (shift || ctrl) SelectClick(h.node, shift, ctrl);
                    else if (!wasSel) SelectClick(h.node, false, false);
                    m_pendingClickSelect = wasSel && !shift && !ctrl && m_sel.nodes.size() + m_sel.comments.size() > 1;
                    m_clickNode = h.node;
                    m_mode = Mode::MoveItems;
                    ShowBringToFront(m_sel);
                    break;
                }
                case Hit::CommentTitle:
                {
                    if (shift || ctrl) { if (!m_sel.comments.erase(h.comment)) m_sel.comments.insert(h.comment); ++m_selRev; }
                    else if (!m_sel.comments.count(h.comment)) { m_sel.Clear(); m_sel.comments.insert(h.comment); ++m_selRev; }
                    m_pendingClickSelect = false;
                    m_clickNode = 0;
                    m_mode = Mode::MoveItems;
                    break;
                }
                case Hit::CommentResize:
                    if (const Comment* c = m_doc->FindComment(h.comment)) m_resizeOld = *c;
                    m_sel.Clear(); m_sel.comments.insert(h.comment); ++m_selRev;
                    m_mode = Mode::ResizeComment;
                    break;
                case Hit::Edge:
                    if (alt) { m_doc->Disconnect(h.edge.to); break; }
                    [[fallthrough]];
                default:
                    if (ctrl && (h.kind == Hit::None || h.kind == Hit::CommentBody || h.kind == Hit::Edge))
                    {
                        m_mode = Mode::CutStroke;
                        m_cutPts.clear(); m_cutPts.push_back(m_mouseG); m_cutMarked.clear();
                    }
                    else
                    {
                        m_boxBase = shift ? m_sel : Selection{};
                        m_mode = Mode::BoxSelect;
                    }
                    break;
                }
            }
        }
    }

    // ---- 各モードの継続・終了 ----
    const bool lDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    switch (m_mode)
    {
    case Mode::Idle: break;

    case Mode::Panning:
    {
        const int btn = m_pressButton;
        const bool down = btn == 2 ? ImGui::IsMouseDown(ImGuiMouseButton_Middle) : btn == 1 ? ImGui::IsMouseDown(ImGuiMouseButton_Right) : lDown;
        if (down)
        {
            if (btn == 1 && !m_rmbMoved && Dist(m_mouse, m_pressPos) > ui::Px(5.0f)) m_rmbMoved = true;
            if (m_rmbMoved)
            {
                const ImVec2 d = io.MouseDelta;
                const float s = m_zoom * m_dpi;
                m_pan.x -= d.x / s; m_pan.y -= d.y / s;
                m_panTarget = m_pan; m_framing = false; m_zoomAnimating = false;
            }
        }
        else
        {
            if (btn == 1 && !m_rmbMoved)
            {
                // 右クリック（動かしていない）: ノード / ワイヤ / コメント / ピン = メニュー、空き地 = 検索パレット
                const Hit h = m_pressHit;
                switch (h.kind)
                {
                case Hit::Node: case Hit::Slot:
                    if (!m_sel.Has(h.node)) { m_sel.Clear(); m_sel.nodes.insert(h.node); ++m_selRev; }
                    OpenContext(CtxKind::Node, h, m_pressG); break;
                case Hit::Pin: OpenContext(CtxKind::Pin, h, m_pressG); break;
                case Hit::Edge: OpenContext(CtxKind::Edge, h, m_pressG); break;
                case Hit::CommentTitle: case Hit::CommentResize:
                    if (!m_sel.comments.count(h.comment)) { m_sel.Clear(); m_sel.comments.insert(h.comment); ++m_selRev; }
                    OpenContext(CtxKind::Comment, h, m_pressG); break;
                default:
                    OpenPaletteAt(m_pressG);
                    m_palScreenPos = m_pressPos;
                    break;
                }
            }
            m_mode = Mode::Idle; m_pressButton = -1;
        }
        break;
    }

    case Mode::BoxSelect:
    {
        if (lDown)
        {
            if (!m_dragStarted && Dist(m_mouse, m_pressPos) > ui::Px(kDragThresholdPx)) m_dragStarted = true;
            if (m_dragStarted) ApplyBoxSelection();
        }
        else
        {
            if (!m_dragStarted && !shift && !ctrl) { if (!m_sel.Empty()) { m_sel.Clear(); ++m_selRev; } }
            m_mode = Mode::Idle;
        }
        break;
    }

    case Mode::MoveItems:
    {
        if (lDown)
        {
            if (!m_dragStarted && Dist(m_mouse, m_pressPos) > ui::Px(kDragThresholdPx))
            {
                m_dragStarted = true;
                BeginMoveSession(m_pressHit.kind == Hit::Node ? m_clickNode : 0);
            }
            if (m_dragStarted) UpdateMoveSession();
        }
        else
        {
            if (m_dragStarted)
            {
                const bool moved = m_doc->CommitMove(m_move, "ノードを移動");
                (void)moved;
            }
            else if (m_pendingClickSelect && m_clickNode)
            {
                m_sel.Clear(); m_sel.nodes.insert(m_clickNode); ++m_selRev;   // 複数選択の 1 つをクリックだけ = その 1 個へ
            }
            m_mode = Mode::Idle; m_dragStarted = false; m_pendingClickSelect = false;
        }
        break;
    }

    case Mode::ResizeComment:
    {
        if (lDown)
        {
            const Comment* c = m_doc->FindComment(m_resizeOld.id);
            if (c)
            {
                Comment n = *c;
                Vec2 mx = m_mouseG;
                if (m_set.snap != io.KeyShift) mx = SnapVec(mx, m_set.snapStep);
                n.rect.max = Vec2(std::max(mx.x, n.rect.min.x + 120.0f), std::max(mx.y, n.rect.min.y + 60.0f));
                m_doc->SetCommentLive(n);
            }
        }
        else
        {
            m_doc->CommitComment(m_resizeOld);
            m_mode = Mode::Idle;
        }
        break;
    }

    case Mode::LinkDrag:
    {
        m_linkDropG = m_mouseG;
        // 接続先の候補: ピン直撃 or ノード本体（互換のある最初のピン）
        m_linkHoverAny = false; m_linkHoverValid = false; m_linkReason.clear();
        const Hit& h = m_hover;
        IGraphModel& m = m_doc->Model();
        auto consider = [&](PinRef cand)
        {
            m_linkHoverAny = true;
            m_linkHoverPin = cand;
            const ConnectCheck chk = m_linkFromOutput ? m.CanConnect(m_linkFrom, cand) : m.CanConnect(cand, m_linkFrom);
            m_linkHoverValid = chk.ok;
            m_linkReason = chk.reason;
        };
        if (h.kind == Hit::Pin && h.pin.output != m_linkFromOutput && h.pin.node != m_linkFrom.node) consider(h.pin);
        else if ((h.kind == Hit::Node || h.kind == Hit::Slot) && h.node != m_linkFrom.node)
        {
            const NodeData* n = m.FindNode(h.node);
            if (n)
            {
                const size_t cnt = m_linkFromOutput ? n->desc->inputs.size() : n->desc->outputs.size();
                PinRef firstAny; bool have = false;
                for (size_t i = 0; i < cnt; ++i)
                {
                    if (m_linkFromOutput && n->desc->inputs[i].propertyOnly) continue;
                    const PinRef cand{h.node, static_cast<uint16_t>(i), !m_linkFromOutput};
                    const ConnectCheck chk = m_linkFromOutput ? m.CanConnect(m_linkFrom, cand) : m.CanConnect(cand, m_linkFrom);
                    if (chk.ok) { consider(cand); have = true; break; }
                    if (!have && !firstAny.Valid()) firstAny = cand;
                }
                if (!have && firstAny.Valid()) consider(firstAny);
            }
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const bool insideCanvas = m_mouse.x >= m_canvasMin.x && m_mouse.x <= m_canvasMax.x && m_mouse.y >= m_canvasMin.y && m_mouse.y <= m_canvasMax.y;
            FinishLink(insideCanvas);
        }
        break;
    }

    case Mode::CutStroke:
    {
        if (lDown)
        {
            {
                const Vec2 last = m_cutPts.back();
                if (std::fabs(m_mouseG.x - last.x) + std::fabs(m_mouseG.y - last.y) > 3.0f / std::max(0.1f, m_zoom))
                {
                    const Rect seg(Vec2(std::min(last.x, m_mouseG.x), std::min(last.y, m_mouseG.y)),
                                   Vec2(std::max(last.x, m_mouseG.x), std::max(last.y, m_mouseG.y)));
                    std::vector<int> es;
                    m_doc->EdgesInRect(seg.Expanded(4.0f), es);
                    const auto& edges = m_doc->Model().Edges();
                    for (int i : es)
                    {
                        const Edge& e = edges[static_cast<size_t>(i)];
                        if (BezierIntersectsSegment(m_doc->EdgeCurve(e), last, m_mouseG))
                        {
                            const uint64_t k = e.to.Key();
                            if (std::find(m_cutMarked.begin(), m_cutMarked.end(), k) == m_cutMarked.end()) m_cutMarked.push_back(k);
                        }
                    }
                    m_cutPts.push_back(m_mouseG);
                }
            }
        }
        else
        {
            if (!m_cutMarked.empty())
            {
                m_doc->History().BeginGroup("ワイヤを切断");
                for (uint64_t k : m_cutMarked)
                {
                    const PinRef in{static_cast<NodeId>(k >> 17), static_cast<uint16_t>((k >> 1) & 0xFFFF), false};
                    m_doc->Disconnect(in);
                }
                m_doc->History().EndGroup();
            }
            m_cutPts.clear(); m_cutMarked.clear();
            m_mode = Mode::Idle;
        }
        break;
    }

    case Mode::SlotDrag:
    {
        if (lDown)
        {
            const float dx = m_mouse.x - m_slot.pressPos.x;
            if (!m_slot.dragging && std::fabs(dx) > ui::Px(3.0f)) m_slot.dragging = true;
            if (m_slot.dragging)
            {
                const NodeData* n = m_doc->Model().FindNode(m_slot.pin.node);
                if (n)
                {
                    const PinDesc& pd = n->desc->inputs[m_slot.pin.index];
                    float speed = pd.dragSpeed;
                    if (io.KeyShift) speed *= 0.1f;
                    if (io.KeyCtrl) speed *= 10.0f;
                    float v = m_slot.startValue + dx / ui::Px(1.0f) * speed;
                    v = std::min(std::max(v, pd.rangeMin), pd.rangeMax);
                    PinValue nv = m_slot.oldValue;
                    nv.f[std::min(m_slot.comp, 3)] = v;
                    m_doc->SetPinValueLive(m_slot.pin, nv);
                }
            }
        }
        else
        {
            if (m_slot.dragging) m_doc->CommitPinValue(m_slot.pin, m_slot.oldValue);
            else
            {
                Hit h; h.kind = Hit::Slot; h.node = m_slot.pin.node; h.pin = m_slot.pin; h.slotComp = m_slot.comp;
                OpenSlotEditor(h, m_slot.pressPos);
            }
            m_mode = Mode::Idle;
        }
        break;
    }

    case Mode::MinimapDrag:
    {
        if (lDown) MinimapCenterOn(m_mouse);
        else m_mode = Mode::Idle;
        break;
    }
    }

    // Esc / 他ボタンで途中の操作を取り消す（ProcessKeys の edit.selectNone も同じ処理）
    (void)ctrl; (void)alt;
}

// ---------------------------------------------------------------------------
// クリップボード
// ---------------------------------------------------------------------------
void GraphView::CopyToClipboard()
{
    if (m_sel.Empty()) return;
    m_clipboard = CopySelectionJson(*m_doc, m_sel);
    ImGui::SetClipboardText(m_clipboard.c_str());
    m_lastAction = "copy";
}

void GraphView::PasteFromClipboard(bool atMouse)
{
    std::string text;
    if (const char* t = ImGui::GetClipboardText()) text = t;
    Rect b;
    if (text.empty() || !ClipboardBounds(*m_doc, text, b))
    {
        text = m_clipboard;
        if (text.empty() || !ClipboardBounds(*m_doc, text, b)) return;
    }
    Vec2 offset(32.0f, 32.0f);
    const bool inside = m_mouse.x >= m_canvasMin.x && m_mouse.x <= m_canvasMax.x && m_mouse.y >= m_canvasMin.y && m_mouse.y <= m_canvasMax.y;
    if (atMouse && inside) offset = SnapPos(m_mouseG) - b.min;
    PasteResult pr;
    if (PasteJson(*m_doc, text, offset, &pr, nullptr))
    {
        m_sel = pr.selection;
        ++m_selRev;
        ShowBringToFront(m_sel);
        m_lastAction = "paste";
    }
}

// ---------------------------------------------------------------------------
// コマンド
// ---------------------------------------------------------------------------
bool GraphView::Execute(const std::string& id)
{
    if (!m_doc) return false;
    IGraphModel& m = m_doc->Model();
    auto align = [&](AlignMode mode)
    {
        std::vector<NodeId> ns(m_sel.nodes.begin(), m_sel.nodes.end());
        m_doc->Align(ns, mode);
        return true;
    };
    if (id == "edit.undo")   { m_doc->History().Undo(*m_doc); PruneSelection(); return true; }
    if (id == "edit.redo")   { m_doc->History().Redo(*m_doc); PruneSelection(); return true; }
    if (id == "edit.copy")   { CopyToClipboard(); return true; }
    if (id == "edit.paste")  { PasteFromClipboard(true); return true; }
    if (id == "edit.duplicate")
    {
        if (m_sel.Empty()) return false;
        PasteResult pr;
        if (PasteJson(*m_doc, CopySelectionJson(*m_doc, m_sel), Vec2(32.0f, 32.0f), &pr, nullptr))
        {
            m_sel = pr.selection; ++m_selRev; ShowBringToFront(m_sel);
        }
        return true;
    }
    if (id == "edit.delete")
    {
        if (m_sel.Empty()) return false;
        m_doc->RemoveItems(m_sel);
        m_sel.Clear(); ++m_selRev;
        return true;
    }
    if (id == "edit.focus")  { FrameSelection(true); return true; }
    if (id == "edit.selectNone")
    {
        // 優先: 操作の取り消し → ポップアップ → 選択解除
        if (m_mode == Mode::MoveItems && m_dragStarted) { m_doc->CancelMove(m_move); m_mode = Mode::Idle; m_dragStarted = false; return true; }
        if (m_mode == Mode::LinkDrag) { m_mode = Mode::Idle; m_linkDetach = false; return true; }
        if (m_mode == Mode::BoxSelect || m_mode == Mode::CutStroke) { m_mode = Mode::Idle; m_cutPts.clear(); m_cutMarked.clear(); return true; }
        if (m_mode == Mode::SlotDrag) { m_doc->SetPinValueLive(m_slot.pin, m_slot.oldValue); m_mode = Mode::Idle; return true; }
        if (!m_sel.Empty()) { m_sel.Clear(); ++m_selRev; return true; }
        return false;
    }
    if (id == "graph.selectAll") { SelectAll(); return true; }
    if (id == "graph.frameAll")  { FrameAll(true); return true; }
    if (id == "graph.resetZoom") { ResetZoom(true); return true; }
    if (id == "graph.palette")
    {
        m_palHasPin = false;
        m_palFilter = PaletteFilter{};
        OpenPaletteAt(m_mouseG);
        m_palScreenPos = m_mouse;
        return true;
    }
    if (id == "graph.comment")
    {
        CommentId cid = 0;
        if (!m_sel.nodes.empty())
        {
            std::vector<NodeId> ns(m_sel.nodes.begin(), m_sel.nodes.end());
            cid = m_doc->AddCommentAround(ns, "コメント", 0);
        }
        else
        {
            const Vec2 p = SnapPos(m_mouseG);
            cid = m_doc->AddComment(Rect(p, p + Vec2(320.0f, 200.0f)), "コメント", 0);
        }
        if (cid) { m_sel.Clear(); m_sel.comments.insert(cid); ++m_selRev; StartRename(cid); }
        return cid != 0;
    }
    if (id == "graph.selectUpstream" || id == "graph.selectDownstream")
    {
        const bool up = id == "graph.selectUpstream";
        std::vector<NodeId> stack(m_sel.nodes.begin(), m_sel.nodes.end()), tmp;
        std::set<NodeId> seen(m_sel.nodes.begin(), m_sel.nodes.end());
        while (!stack.empty())
        {
            const NodeId n = stack.back(); stack.pop_back();
            tmp.clear();
            if (up) m.Upstream(n, tmp); else m.Downstream(n, tmp);
            for (NodeId x : tmp) if (seen.insert(x).second) stack.push_back(x);
        }
        m_sel.nodes = seen; ++m_selRev;
        return true;
    }
    if (id == "graph.alignLeft")    return align(AlignMode::Left);
    if (id == "graph.alignRight")   return align(AlignMode::Right);
    if (id == "graph.alignTop")     return align(AlignMode::Top);
    if (id == "graph.alignBottom")  return align(AlignMode::Bottom);
    if (id == "graph.alignCenterH") return align(AlignMode::CenterH);
    if (id == "graph.alignCenterV") return align(AlignMode::CenterV);
    if (id == "graph.distributeH")  return align(AlignMode::DistributeH);
    if (id == "graph.distributeV")  return align(AlignMode::DistributeV);
    if (id == "graph.toggleSnap")    { m_set.snap = !m_set.snap; return true; }
    if (id == "graph.toggleMinimap") { m_set.showMinimap = !m_set.showMinimap; return true; }
    if (id == "graph.toggleGrid")    { m_set.showGrid = !m_set.showGrid; return true; }
    if (id == "graph.toggleFlow")
    {
        m_set.flow = m_set.flow == GraphViewSettings::Flow::Off ? GraphViewSettings::Flow::Highlighted : GraphViewSettings::Flow::Off;
        return true;
    }
    if (id == "graph.disconnectSelected")
    {
        m_doc->History().BeginGroup("ワイヤを切断");
        std::vector<PinRef> ins;
        for (const Edge& e : m.Edges()) if (m_sel.Has(e.from.node) || m_sel.Has(e.to.node)) ins.push_back(e.to);
        for (const PinRef& p : ins) m_doc->Disconnect(p);
        m_doc->History().EndGroup();
        return !ins.empty();
    }
    return false;
}

void GraphView::ProcessKeys()
{
    if (!m_doc || !m_windowFocused) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (m_paletteOpen || m_editKind != EditKind::None || m_renameComment != 0) return;   // ポップアップ側が処理する

    for (const char* id : kKeyCommands)
    {
        const cmd::Def* d = cmd::FindCommand(id);
        if (!d) continue;
        if (ChordPressed(d->chord ? d->chord : "") || ChordPressed(d->chord2 ? d->chord2 : ""))
        {
            Execute(id);
            return;
        }
    }
    // 矢印 = 選択を 1 グリッド動かす（Shift で 4 倍）
    if (!io.KeyCtrl && !io.KeyAlt && !m_sel.Empty() && m_mode == Mode::Idle)
    {
        const float step = m_set.snapStep * (io.KeyShift ? 4.0f : 1.0f);
        Vec2 d;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))  d.x -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) d.x += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))    d.y -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))  d.y += step;
        if (d != Vec2(0, 0)) { m_doc->MoveItems(m_sel, d); return; }
    }
    // UE 風ワンキー: ノード型の hotkey を押すと、マウス位置（キャンバス外なら中央）に置く
    if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift && m_mode == Mode::Idle)
    {
        for (const NodeTypeDesc& t : m_doc->Model().NodeTypes())
        {
            if (!t.hotkey) continue;
            const ImGuiKey k = KeyFromName(std::string(1, t.hotkey));
            if (k == ImGuiKey_None || !ImGui::IsKeyPressed(k, false)) continue;
            const bool inside = m_mouse.x >= m_canvasMin.x && m_mouse.x <= m_canvasMax.x && m_mouse.y >= m_canvasMin.y && m_mouse.y <= m_canvasMax.y;
            Vec2 p = inside ? m_mouseG : m_vp.ToGraph(Vec2((m_canvasMin.x + m_canvasMax.x) * 0.5f, (m_canvasMin.y + m_canvasMax.y) * 0.5f));
            p = SnapPos(p);
            const NodeId n = m_doc->AddNode(t.id, p);
            if (n) { m_sel.Clear(); m_sel.nodes.insert(n); ++m_selRev; ShowBringToFront(m_sel); }
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// テストフック
// ---------------------------------------------------------------------------
ImVec2 GraphView::ScreenPinPos(PinRef pin) const
{
    const Vec2 p = m_vp.ToScreen(m_doc->PinPos(pin));
    return ImVec2(p.x, p.y);
}

ImVec2 GraphView::ScreenNodeRect(NodeId id, ImVec2* outMax) const
{
    const Rect r = m_vp.ToScreen(m_doc->NodeRect(id));
    if (outMax) *outMax = ImVec2(r.max.x, r.max.y);
    return ImVec2(r.min.x, r.min.y);
}

} // namespace dx12e::ng
