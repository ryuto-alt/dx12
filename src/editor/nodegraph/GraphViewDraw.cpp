// ノードグラフ ImGui ビュー: 描画（グリッド・コメント・ワイヤ・ノード・オーバーレイ・ミニマップ）。
// 色は GraphTokens（theme:: 由来）だけ。寸法は GraphLayout の論理単位 × (zoom * dpi)。

#include "editor/nodegraph/GraphView.h"

#include "editor/UiWidgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dx12e::ng
{

namespace
{
inline ImVec2 V(Vec2 v) { return ImVec2(v.x, v.y); }
inline ImVec2 V(ImVec2 v) { return v; }

ImU32 AlphaMul(ImU32 c, float k)
{
    const int a = static_cast<int>((c >> 24) & 0xFF);
    const int na = std::min(255, std::max(0, static_cast<int>(static_cast<float>(a) * k + 0.5f)));
    return (c & 0x00FFFFFFu) | (static_cast<ImU32>(na) << 24);
}

ImU32 Mix(ImU32 a, ImU32 b, float t)
{
    auto ch = [&](int shift)
    {
        const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ImU32>(std::min(255.0f, std::max(0.0f, x + (y - x) * t + 0.5f)));
    };
    return (ch(0)) | (ch(8) << 8) | (ch(16) << 16) | (ch(24) << 24);
}

void FmtFloat(char* buf, size_t n, float v, int decimals = 3)
{
    if (std::fabs(v) >= 1000.0f) { std::snprintf(buf, n, "%.0f", static_cast<double>(v)); return; }
    std::snprintf(buf, n, "%.*f", decimals, static_cast<double>(v));
    char* dot = std::strchr(buf, '.');
    if (dot)
    {
        char* end = buf + std::strlen(buf) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (end == dot) *end = '\0';
    }
    if (std::strcmp(buf, "-0") == 0) std::snprintf(buf, n, "0");
}

float Hash01(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return static_cast<float>(x & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}
} // namespace

void GraphView::AnimStep(Spring& sp, float target, float freq, float damping)
{
    if (!m_set.animate) sp.Snap(target);
    else sp.Step(target, m_dt, freq, damping);
}

void GraphView::SetNodeState(NodeId id, NodeState s, const std::string& message)
{
    if (s == NodeState::Normal) m_nodeStates.erase(id);
    else m_nodeStates[id] = NodeStateInfo{s, message};
}

// ---------------------------------------------------------------------------
// 部品
// ---------------------------------------------------------------------------
void GraphView::DrawText(ImDrawList* dl, ImFont* font, float units, ImVec2 pos, ImU32 col, const char* text, const ImVec4* clip) const
{
    const float px = std::floor(units * m_vp.Scale() + 0.5f);
    if (px < 5.0f || !font) return;
    dl->AddText(font, px, pos, col, text, nullptr, 0.0f, clip);
}

void GraphView::DrawGlow(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding, ImU32 col, float strength, float radiusPx) const
{
    if (strength <= 0.01f || radiusPx < 1.0f) return;
    constexpr int kSteps = 6;
    for (int i = kSteps; i >= 1; --i)
    {
        const float d = radiusPx * static_cast<float>(i) / static_cast<float>(kSteps);
        const float f = 1.0f - static_cast<float>(i - 1) / static_cast<float>(kSteps);
        dl->AddRectFilled(ImVec2(mn.x - d, mn.y - d), ImVec2(mx.x + d, mx.y + d), AlphaMul(col, std::min(1.0f, strength * 0.10f * f)), rounding + d);
    }
}

void GraphView::DrawPinShape(ImDrawList* dl, ImVec2 c, float r, PinShape shape, bool filled, ImU32 col, ImU32 inner, float alpha) const
{
    const ImU32 cc = AlphaMul(col, alpha), ci = AlphaMul(inner, alpha);
    const float th = std::max(1.0f, r * 0.30f);
    const int seg = r < 4.0f ? 10 : 16;
    auto ring = [&]()
    {
        if (filled) dl->AddCircleFilled(c, r, cc, seg);
        else { dl->AddCircleFilled(c, r, ci, seg); dl->AddCircle(c, r - th * 0.5f, cc, seg, th); }
    };
    switch (shape)
    {
    case PinShape::Circle: ring(); break;
    case PinShape::CircleLine1:
    case PinShape::CircleLine2:
    {
        ring();
        const ImU32 lc = filled ? ci : cc;
        const float lw = std::max(1.0f, r * 0.24f);
        if (shape == PinShape::CircleLine1) dl->AddLine(ImVec2(c.x - r * 0.5f, c.y), ImVec2(c.x + r * 0.5f, c.y), lc, lw);
        else
        {
            dl->AddLine(ImVec2(c.x - r * 0.5f, c.y - r * 0.32f), ImVec2(c.x + r * 0.5f, c.y - r * 0.32f), lc, lw);
            dl->AddLine(ImVec2(c.x - r * 0.5f, c.y + r * 0.32f), ImVec2(c.x + r * 0.5f, c.y + r * 0.32f), lc, lw);
        }
        break;
    }
    case PinShape::Diamond:
    {
        const float q = r * 1.22f;
        const ImVec2 p[4] = {ImVec2(c.x, c.y - q), ImVec2(c.x + q, c.y), ImVec2(c.x, c.y + q), ImVec2(c.x - q, c.y)};
        if (filled) dl->AddConvexPolyFilled(p, 4, cc);
        else { dl->AddConvexPolyFilled(p, 4, ci); dl->AddPolyline(p, 4, cc, ImDrawFlags_Closed, th); }
        break;
    }
    case PinShape::RoundedSquare:
    {
        const float h = r * 0.95f;
        dl->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), filled ? cc : ci, r * 0.38f);
        if (!filled) dl->AddRect(ImVec2(c.x - h + th * 0.5f, c.y - h + th * 0.5f), ImVec2(c.x + h - th * 0.5f, c.y + h - th * 0.5f), cc, r * 0.3f, 0, th);
        break;
    }
    case PinShape::Square:
    {
        const float h = r * 0.82f;
        dl->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), filled ? cc : ci, 1.0f);
        if (!filled) dl->AddRect(ImVec2(c.x - h + th * 0.5f, c.y - h + th * 0.5f), ImVec2(c.x + h - th * 0.5f, c.y + h - th * 0.5f), cc, 0.0f, 0, th);
        break;
    }
    case PinShape::Pentagon:
        dl->AddNgonFilled(c, r * 1.15f, filled ? cc : ci, 5);
        if (!filled) dl->AddNgon(c, r * 1.15f - th * 0.5f, cc, 5, th);
        break;
    case PinShape::Triangle:
    {
        const float q = r * 1.2f;
        dl->AddTriangleFilled(ImVec2(c.x - q * 0.8f, c.y - q), ImVec2(c.x + q, c.y), ImVec2(c.x - q * 0.8f, c.y + q), filled ? cc : ci);
        if (!filled) dl->AddTriangle(ImVec2(c.x - q * 0.8f, c.y - q), ImVec2(c.x + q, c.y), ImVec2(c.x - q * 0.8f, c.y + q), cc, th);
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// フレーム全体
// ---------------------------------------------------------------------------
void GraphView::UpdateFlowSet()
{
    IGraphModel& m = m_doc->Model();
    const uint64_t key = (m_selRev * 0x9E3779B97F4A7C15ull) ^ m.Revision();
    if (key == m_flowKey) return;
    m_flowKey = key;
    m_upSet.clear();
    m_downSet.clear();
    if (m_sel.nodes.empty()) return;
    std::vector<NodeId> stack, tmp;
    for (int dir = 0; dir < 2; ++dir)
    {
        std::unordered_set<NodeId>& set = dir == 0 ? m_upSet : m_downSet;
        stack.assign(m_sel.nodes.begin(), m_sel.nodes.end());
        for (NodeId n : stack) set.insert(n);
        while (!stack.empty())
        {
            const NodeId n = stack.back();
            stack.pop_back();
            tmp.clear();
            if (dir == 0) m.Upstream(n, tmp); else m.Downstream(n, tmp);
            for (NodeId x : tmp) if (set.insert(x).second) stack.push_back(x);
        }
    }
}

void GraphView::Render()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int vtx0 = dl->VtxBuffer.Size;
    IGraphModel& model = m_doc->Model();
    m_stats.nodes = static_cast<int>(model.Nodes().size());
    m_stats.edges = static_cast<int>(model.Edges().size());
    m_stationary = m_hovered && ImGui::IsMouseHoveringRect(m_canvasMin, m_canvasMax) && ImGui::GetIO().MouseDelta.x == 0.0f && ImGui::GetIO().MouseDelta.y == 0.0f;

    dl->PushClipRect(m_canvasMin, m_canvasMax, true);
    m_split.Split(dl, 4);
    m_split.SetCurrentChannel(dl, 0);
    // 背景（上下でわずかに暗くして周辺を沈める）
    dl->AddRectFilledMultiColor(m_canvasMin, m_canvasMax, m_tok.canvasBgEdge, m_tok.canvasBgEdge, m_tok.canvasBg, m_tok.canvasBg);
    dl->AddRectFilledMultiColor(ImVec2(m_canvasMin.x, m_canvasMin.y + (m_canvasMax.y - m_canvasMin.y) * 0.35f), m_canvasMax,
                                m_tok.canvasBg, m_tok.canvasBg, m_tok.canvasBgEdge, m_tok.canvasBgEdge);
    if (m_set.showGrid) DrawGrid(dl);

    // 見えているものの列挙（空間分割）
    m_doc->EnsureIndex();
    const Rect vis = m_vp.VisibleGraphRect().Expanded(30.0f);
    std::vector<NodeId> nodes;
    m_doc->NodesInRect(vis, false, nodes);
    m_visNodes = std::move(nodes);
    std::sort(m_visNodes.begin(), m_visNodes.end(), [&](NodeId a, NodeId b)
    {
        auto za = m_z.find(a), zb = m_z.find(b);
        const uint32_t va = za == m_z.end() ? 0 : za->second, vb = zb == m_z.end() ? 0 : zb->second;
        if (va != vb) return va < vb;
        return a < b;
    });
    m_doc->EdgesInRect(vis, m_visEdges);
    if (m_connRev != model.Revision())
    {
        m_connKeys.clear();
        for (const Edge& e : model.Edges()) { m_connKeys.push_back(e.from.Key()); m_connKeys.push_back(e.to.Key()); }
        std::sort(m_connKeys.begin(), m_connKeys.end());
        m_connRev = model.Revision();
    }
    UpdateFlowSet();
    // 接続ドラッグ中のドラッグ元の型（互換ピンの強調に使う）
    if (m_mode == Mode::LinkDrag)
    {
        if (const NodeData* n = model.FindNode(m_linkFrom.node))
        {
            if (m_linkFromOutput) m_linkSrcType = model.ResolvedPinType(m_linkFrom);
            else if (m_linkFrom.index < n->desc->inputs.size()) m_linkSrcType = n->desc->inputs[m_linkFrom.index].type;
        }
    }

    DrawComments(dl);
    m_split.SetCurrentChannel(dl, 1);
    DrawEdges(dl);
    m_split.SetCurrentChannel(dl, 2);
    DrawNodes(dl);
    m_split.SetCurrentChannel(dl, 3);
    DrawOverlays(dl);
    DrawMinimap(dl);
    m_split.Merge(dl);
    dl->PopClipRect();
    m_stats.drawnPrims = dl->VtxBuffer.Size - vtx0;

    // 使われなくなったアニメ状態の掃除
    if (m_anim.size() > model.Nodes().size() + 64)
        for (auto it = m_anim.begin(); it != m_anim.end();) it = model.FindNode(it->first) ? std::next(it) : m_anim.erase(it);
    if (m_z.size() > model.Nodes().size() + 64)
        for (auto it = m_z.begin(); it != m_z.end();) it = model.FindNode(it->first) ? std::next(it) : m_z.erase(it);
}

// ---------------------------------------------------------------------------
// グリッド
// ---------------------------------------------------------------------------
void GraphView::DrawGrid(ImDrawList* dl)
{
    const float scale = m_vp.Scale();
    const Rect vis = m_vp.VisibleGraphRect();
    const float minor = m_set.snapStep > 0.0f ? m_set.snapStep : 16.0f;
    const float major = minor * 8.0f;
    auto lines = [&](float step, ImU32 col, float alphaK, float thick)
    {
        const float px = step * scale;
        if (px < 5.0f) return;
        const float a = std::min(1.0f, (px - 5.0f) / 9.0f) * alphaK;
        const ImU32 c = AlphaMul(col, a);
        const float x0 = std::floor(vis.min.x / step) * step, y0 = std::floor(vis.min.y / step) * step;
        for (float x = x0; x <= vis.max.x; x += step)
        {
            const float sx = std::floor(m_vp.ToScreen(Vec2(x, 0)).x) + 0.5f;
            dl->AddLine(ImVec2(sx, m_canvasMin.y), ImVec2(sx, m_canvasMax.y), c, thick);
        }
        for (float y = y0; y <= vis.max.y; y += step)
        {
            const float sy = std::floor(m_vp.ToScreen(Vec2(0, y)).y) + 0.5f;
            dl->AddLine(ImVec2(m_canvasMin.x, sy), ImVec2(m_canvasMax.x, sy), c, thick);
        }
    };
    lines(minor, m_tok.gridMinor, 1.0f, 1.0f);
    lines(major, m_tok.gridMajor, 1.0f, 1.0f);
    // 原点の軸
    const Vec2 o = m_vp.ToScreen(Vec2(0, 0));
    if (o.x >= m_canvasMin.x && o.x <= m_canvasMax.x) dl->AddLine(ImVec2(std::floor(o.x) + 0.5f, m_canvasMin.y), ImVec2(std::floor(o.x) + 0.5f, m_canvasMax.y), m_tok.gridAxis, 1.0f);
    if (o.y >= m_canvasMin.y && o.y <= m_canvasMax.y) dl->AddLine(ImVec2(m_canvasMin.x, std::floor(o.y) + 0.5f), ImVec2(m_canvasMax.x, std::floor(o.y) + 0.5f), m_tok.gridAxis, 1.0f);
}

// ---------------------------------------------------------------------------
// コメント
// ---------------------------------------------------------------------------
void GraphView::DrawComments(ImDrawList* dl)
{
    const float scale = m_vp.Scale();
    const Rect vis = m_vp.VisibleGraphRect();
    const float titleH = TitleHeightFor(m_zoom);
    const float R = std::max(2.0f, 6.0f * scale);
    const float b1 = std::max(1.0f, ui::PxF(1.0f));
    for (const Comment& c : m_doc->Comments())
    {
        if (!c.rect.Overlaps(vis)) continue;
        const int ci = std::min(std::max(c.color, 0), kCommentColors - 1);
        const Rect rs = m_vp.ToScreen(c.rect);
        const ImVec2 mn(rs.min.x, rs.min.y), mx(rs.max.x, rs.max.y);
        const bool sel = m_sel.comments.count(c.id) != 0;
        const bool hov = m_hoverComment == c.id;
        const float th = std::min(titleH * scale, (mx.y - mn.y) * 0.5f);

        dl->AddRectFilled(mn, mx, m_tok.commentFill[ci], R);
        dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + th), m_tok.commentTitle[ci], R, ImDrawFlags_RoundCornersTop);
        dl->AddRect(mn, mx, sel ? m_tok.accentHover : AlphaMul(m_tok.commentBorder[ci], hov ? 1.0f : 0.8f), R, 0, sel ? b1 * 1.6f : b1);
        if (sel && m_set.glow) DrawGlow(dl, mn, mx, R, m_tok.accent, m_tok.glow * 0.7f, ui::PxF(10.0f));

        // 見出し（クリップして描く）
        const float unitsTitle = titleH * 0.5f;
        const ImVec4 clip(mn.x, mn.y, mx.x - b1 * 2.0f, mn.y + th);
        const float px = std::floor(unitsTitle * scale + 0.5f);
        if (px >= 5.0f && !c.title.empty())
        {
            const float lh = FontBold()->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y;
            dl->AddText(FontBold(), px, ImVec2(mn.x + 10.0f * scale, mn.y + (th - lh) * 0.5f), m_tok.text, c.title.c_str(), nullptr, 0.0f, &clip);
        }
        // リサイズハンドル（右下の斜線 3 本）
        if (sel || hov)
        {
            const ImU32 hc = AlphaMul(m_tok.accentHover, sel ? 0.9f : 0.6f);
            for (int i = 1; i <= 3; ++i)
            {
                const float d = static_cast<float>(i) * 4.0f * std::max(0.6f, scale);
                dl->AddLine(ImVec2(mx.x - 3.0f - d, mx.y - 3.0f), ImVec2(mx.x - 3.0f, mx.y - 3.0f - d), hc, b1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ワイヤ
// ---------------------------------------------------------------------------
void GraphView::DrawEdges(ImDrawList* dl)
{
    IGraphModel& model = m_doc->Model();
    const auto& edges = model.Edges();
    const float scale = m_vp.Scale();
    const bool hasSel = !m_sel.nodes.empty();
    const float baseW = std::max(1.0f, 2.0f * m_dpi * std::min(1.5f, std::max(0.55f, m_zoom)));
    const bool glowOn = m_set.glow && m_tok.glow > 0.0f && m_zoom >= 0.3f;
    const float now = Now();

    // ワイヤのホバー強調（連続）
    const bool wireHovered = m_hoverEdgeValid && m_mode == Mode::Idle;
    if (wireHovered) m_wireHoverEdge = m_hoverEdge;
    AnimStep(m_wireHover, wireHovered ? 1.0f : 0.0f, 24.0f, 1.0f);

    int drawn = 0;
    for (int idx : m_visEdges)
    {
        const Edge& e = edges[static_cast<size_t>(idx)];
        if (m_linkDetach && e == m_linkDetached) continue;
        const Bezier c = m_doc->EdgeCurve(e);
        const ImVec2 p0 = V(m_vp.ToScreen(c.p0)), p1 = V(m_vp.ToScreen(c.p1)), p2 = V(m_vp.ToScreen(c.p2)), p3 = V(m_vp.ToScreen(c.p3));

        const PinType rt = model.ResolvedPinType(e.from);
        ImU32 col = m_tok.pinType[std::min(std::max(model.PinTypeInfo(rt).colorSlot, 0), kPinTypeSlots - 1)];
        const bool hl = hasSel && (m_upSet.count(e.to.node) || m_downSet.count(e.from.node));
        const bool hov = wireHovered && e == m_wireHoverEdge;
        const bool cut = !m_cutMarked.empty() && std::find(m_cutMarked.begin(), m_cutMarked.end(), e.to.Key()) != m_cutMarked.end();
        float alpha = hasSel ? (hl ? 1.0f : 0.28f) : 0.82f;
        float w = baseW * (hl ? 1.3f : 1.0f);
        if (hov) { alpha = 1.0f; w = baseW * (1.0f + 0.5f * m_wireHover.value); }
        if (cut) { col = m_tok.bad; alpha = 0.9f; }

        const float len = std::sqrt((p3.x - p0.x) * (p3.x - p0.x) + (p3.y - p0.y) * (p3.y - p0.y));
        const int segs = std::min(40, std::max(6, static_cast<int>(len / (m_zoom < 0.4f ? 60.0f : 26.0f)) + 4));
        if (glowOn && (hl || hov) && !cut)
        {
            const float gk = m_tok.glow * (hov ? m_wireHover.value : 1.0f);
            dl->AddBezierCubic(p0, p1, p2, p3, AlphaMul(col, 0.070f * gk * 2.2f), w * 5.2f, segs);
            dl->AddBezierCubic(p0, p1, p2, p3, AlphaMul(col, 0.115f * gk * 2.0f), w * 3.2f, segs);
            dl->AddBezierCubic(p0, p1, p2, p3, AlphaMul(col, 0.22f * gk * 1.6f), w * 1.9f, segs);
        }
        dl->AddBezierCubic(p0, p1, p2, p3, AlphaMul(col, alpha), w, segs);
        ++drawn;

        // 流れるハイライト（選択の上流 / 下流と、ホバー中のワイヤだけ。設定で無効化）
        const bool flow = (m_set.flow == GraphViewSettings::Flow::Highlighted && (hl || hov)) || (m_set.flow == GraphViewSettings::Flow::All && m_visEdges.size() < 200);
        if (flow && !cut && m_zoom >= 0.3f)
        {
            const uint32_t h = e.from.node * 2654435761u + e.to.node * 40503u + e.to.index;
            const float speed = 0.34f;
            for (int k = 0; k < 2; ++k)
            {
                const float t0 = std::fmod(now * speed + Hash01(h + static_cast<uint32_t>(k) * 977u) + static_cast<float>(k) * 0.5f, 1.0f);
                for (int j = 0; j < 5; ++j)
                {
                    const float t = t0 - static_cast<float>(j) * 0.014f;
                    if (t <= 0.0f || t >= 1.0f) continue;
                    const Vec2 g = c.At(t);
                    const ImVec2 sp = V(m_vp.ToScreen(g));
                    const float f = 1.0f - static_cast<float>(j) / 5.0f;
                    dl->AddCircleFilled(sp, w * (0.55f + 0.7f * f), AlphaMul(Mix(col, 0xFFFFFFFFu, 0.55f), 0.85f * f), 8);
                }
            }
        }
    }
    m_stats.drawnEdges = drawn;
    (void)scale;
}

// ---------------------------------------------------------------------------
// 値欄
// ---------------------------------------------------------------------------
void GraphView::DrawSlot(ImDrawList* dl, const NodeData& n, int pinIndex, const Rect& slotG, float scale, float alpha, bool hoverSlot, int hoverComp)
{
    const PinDesc& pd = n.desc->inputs[static_cast<size_t>(pinIndex)];
    const PinValue& v = n.values[static_cast<size_t>(pinIndex)];
    const Rect rs = m_vp.ToScreen(slotG);
    const ImVec2 mn(rs.min.x, rs.min.y), mx(rs.max.x, rs.max.y);
    const float R = std::max(2.0f, 3.0f * scale);
    const float b1 = std::max(1.0f, ui::PxF(1.0f));
    const LayoutConfig& cfg = m_doc->Layout().Config();
    const bool active = m_mode == Mode::SlotDrag && m_slot.pin.node == n.id && m_slot.pin.index == pinIndex;

    auto textCentered = [&](const char* txt, ImVec2 a, ImVec2 b, ImU32 col)
    {
        const float px = std::floor(cfg.textUnits * scale + 0.5f);
        if (px < 5.0f) return;
        const ImVec2 ts = FontMono()->CalcTextSizeA(px, FLT_MAX, 0.0f, txt);
        const ImVec4 clip(a.x, a.y, b.x, b.y);
        dl->AddText(FontMono(), px, ImVec2(std::floor((a.x + b.x - ts.x) * 0.5f + 0.5f), std::floor((a.y + b.y - ts.y) * 0.5f + 0.5f)), col, txt, nullptr, 0.0f, &clip);
    };

    switch (pd.valueKind)
    {
    case ValueKind::Color:
    {
        // 色見本（明るい側は枠で締める）
        const ImU32 c = ImGui::ColorConvertFloat4ToU32(ImVec4(v.f[0], v.f[1], v.f[2], 1.0f));
        dl->AddRectFilled(mn, mx, AlphaMul(c, alpha), R);
        dl->AddRect(mn, mx, AlphaMul(hoverSlot ? m_tok.slotBorderHover : m_tok.slotBorder, alpha), R, 0, b1);
        // 明暗の縁取り（暗い色は内側に細い明線、明るい色は外側が締まる）
        dl->AddRect(ImVec2(mn.x + b1, mn.y + b1), ImVec2(mx.x - b1, mx.y - b1), AlphaMul(IM_COL32(255, 255, 255, 26), alpha), std::max(1.0f, R - b1), 0, b1);
        break;
    }
    case ValueKind::Bool:
    {
        const bool on = v.f[0] != 0.0f;
        dl->AddRectFilled(mn, mx, AlphaMul(on ? m_tok.accent : m_tok.slotBg, alpha), R);
        dl->AddRect(mn, mx, AlphaMul(on ? m_tok.accentHover : (hoverSlot ? m_tok.slotBorderHover : m_tok.slotBorder), alpha), R, 0, b1);
        if (on)
        {
            const float w = mx.x - mn.x, h = mx.y - mn.y;
            const ImVec2 a(mn.x + w * 0.24f, mn.y + h * 0.52f), b(mn.x + w * 0.44f, mn.y + h * 0.72f), c(mn.x + w * 0.78f, mn.y + h * 0.30f);
            const ImU32 ck = AlphaMul(IM_COL32(255, 255, 255, 235), alpha);
            dl->AddLine(a, b, ck, std::max(1.4f, scale * 1.6f));
            dl->AddLine(b, c, ck, std::max(1.4f, scale * 1.6f));
        }
        break;
    }
    case ValueKind::Enum:
    {
        dl->AddRectFilled(mn, mx, AlphaMul(m_tok.slotBg, alpha), R);
        dl->AddRect(mn, mx, AlphaMul(hoverSlot ? m_tok.slotBorderHover : m_tok.slotBorder, alpha), R, 0, b1);
        const int idx = std::min(std::max(static_cast<int>(v.f[0]), 0), std::max(0, static_cast<int>(pd.enumOptions.size()) - 1));
        const std::string label = pd.enumOptions.empty() ? std::string("-") : pd.enumOptions[static_cast<size_t>(idx)];
        const float pad = 5.0f * scale;
        const float px = std::floor(cfg.textUnits * scale + 0.5f);
        if (px >= 5.0f)
        {
            const float lh = FontBody()->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y;
            const ImVec4 clip(mn.x, mn.y, mx.x - 14.0f * scale, mx.y);
            dl->AddText(FontBody(), px, ImVec2(mn.x + pad, (mn.y + mx.y - lh) * 0.5f), AlphaMul(m_tok.slotText, alpha), label.c_str(), nullptr, 0.0f, &clip);
        }
        // ▾
        const ImVec2 cc(mx.x - 9.0f * scale, (mn.y + mx.y) * 0.5f);
        const float q = 2.6f * scale;
        dl->AddTriangleFilled(ImVec2(cc.x - q, cc.y - q * 0.5f), ImVec2(cc.x + q, cc.y - q * 0.5f), ImVec2(cc.x, cc.y + q * 0.7f), AlphaMul(m_tok.textDim, alpha));
        break;
    }
    default:   // Float / Vec2 / Vec3 / Vec4
    {
        const int nc = ValueComponentCount(pd.valueKind);
        const int n1 = std::max(1, std::min(nc, 4));
        for (int c = 0; c < n1; ++c)
        {
            const Rect cr = SlotComponentRect(slotG, pd.valueKind, c);
            const Rect crs = m_vp.ToScreen(cr);
            const ImVec2 a(crs.min.x + (c > 0 ? 1.0f : 0.0f), crs.min.y), b(crs.max.x - (c + 1 < n1 ? 1.0f : 0.0f), crs.max.y);
            const bool ch = hoverSlot && hoverComp == c;
            const ImDrawFlags fl = n1 == 1 ? 0 : (c == 0 ? ImDrawFlags_RoundCornersLeft : (c + 1 == n1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone));
            dl->AddRectFilled(a, b, AlphaMul(m_tok.slotBg, alpha), R, fl);
            // 範囲つきスカラーは塗りバー（スライダー風）
            if (pd.valueKind == ValueKind::Float && pd.rangeMax - pd.rangeMin < 1000.0f && pd.rangeMax > pd.rangeMin)
            {
                const float t = std::min(1.0f, std::max(0.0f, (v.f[0] - pd.rangeMin) / (pd.rangeMax - pd.rangeMin)));
                if (t > 0.0f) dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * t, b.y), AlphaMul(m_tok.accent, 0.30f * alpha), R, fl);
            }
            if (ch || (active && m_slot.comp == c)) dl->AddRectFilled(a, b, AlphaMul(m_tok.accent, 0.10f * alpha), R, fl);
            char buf[24];
            FmtFloat(buf, sizeof(buf), v.f[c], n1 == 1 ? 3 : 2);
            textCentered(buf, a, b, AlphaMul(m_tok.slotText, alpha));
        }
        const ImU32 bc = active ? m_tok.accent : (hoverSlot ? m_tok.slotBorderHover : m_tok.slotBorder);
        dl->AddRect(mn, mx, AlphaMul(bc, alpha), R, 0, b1);
        for (int c = 1; c < n1; ++c)
        {
            const float x = m_vp.ToScreen(Vec2(SlotComponentRect(slotG, pd.valueKind, c).min.x, 0)).x;
            dl->AddLine(ImVec2(x, mn.y + 2.0f), ImVec2(x, mx.y - 2.0f), AlphaMul(m_tok.slotBorder, alpha), 1.0f);
        }
        break;
    }
    }
}

void GraphView::DrawPreview(ImDrawList* dl, const NodeData& n, const Rect& rg, float scale, ImU32 base)
{
    const Rect rs = m_vp.ToScreen(rg);
    const ImVec2 mn(rs.min.x, rs.min.y), mx(rs.max.x, rs.max.y);
    const float R = std::max(2.0f, 4.0f * scale);
    dl->AddRectFilled(mn, mx, m_tok.nodeInner, R);
    if (m_nodePreview)
    {
        dl->PushClipRect(mn, mx, true);
        const bool drew = m_nodePreview(n.id, dl, mn, mx);
        dl->PopClipRect();
        if (drew) { dl->AddRect(mn, mx, m_tok.nodeBorder, R, 0, std::max(1.0f, ui::PxF(1.0f))); return; }
    }
    // 予約領域のプレースホルダー: ノード ID から決まる決定的な模様（チェッカー + 斜めグラデ）
    const float h = Hash01(n.id * 7919u);
    const ImU32 c0 = Mix(m_tok.nodeInner, base, 0.20f + 0.25f * h), c1 = Mix(m_tok.nodeInner, base, 0.55f);
    const ImVec2 in0(mn.x + 2.0f, mn.y + 2.0f), in1(mx.x - 2.0f, mx.y - 2.0f);
    dl->PushClipRect(in0, in1, true);
    dl->AddRectFilledMultiColor(in0, in1, c0, c1, c0, c1);
    const int cols = 8, rows = 4;
    const float cw = (in1.x - in0.x) / static_cast<float>(cols), ch = (in1.y - in0.y) / static_cast<float>(rows);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x)
            if (((x + y) & 1) == 0)
                dl->AddRectFilled(ImVec2(in0.x + cw * static_cast<float>(x), in0.y + ch * static_cast<float>(y)),
                                  ImVec2(in0.x + cw * static_cast<float>(x + 1), in0.y + ch * static_cast<float>(y + 1)), IM_COL32(255, 255, 255, 14));
    dl->PopClipRect();
    dl->AddRect(mn, mx, m_tok.nodeBorder, R, 0, std::max(1.0f, ui::PxF(1.0f)));
}

// ---------------------------------------------------------------------------
// ノード
// ---------------------------------------------------------------------------
void GraphView::DrawNodes(ImDrawList* dl)
{
    IGraphModel& model = m_doc->Model();
    GraphLayout& layout = m_doc->Layout();
    const LayoutConfig& cfg = layout.Config();
    const float scale = m_vp.Scale();
    const float zoom = m_zoom;
    const bool lodRect = zoom < 0.25f;
    const bool lodNoLabels = zoom < 0.4f;
    const bool lodNoSlots = zoom < 0.6f;
    const float R = std::max(2.0f, 7.0f * scale);
    const float b1 = std::max(1.0f, ui::PxF(1.0f));
    const float now = Now();
    const bool linkActive = m_mode == Mode::LinkDrag;
    const bool movingActive = m_mode == Mode::MoveItems && m_dragStarted;
    int drawn = 0;
    int anchors = 0;

    // 仮想入力の imgui_find が引ける名前つき要素（ノード数が少ないときだけ。大きいグラフでは出さない）
    const bool emitAnchors = m_anchor && static_cast<int>(m_visNodes.size()) <= m_anchorMaxNodes;

    // ピンのホバー（連続）: ホバー中のピンは 1 へ、他は 0 へ
    if (m_hoverPin.Valid()) m_pinAnim[m_hoverPin.Key()];
    for (auto it = m_pinAnim.begin(); it != m_pinAnim.end();)
    {
        const bool target = m_hoverPin.Valid() && it->first == m_hoverPin.Key();
        AnimStep(it->second, target ? 1.0f : 0.0f, 28.0f, 0.75f);
        if (!target && it->second.value < 0.002f) it = m_pinAnim.erase(it); else ++it;
    }

    for (NodeId id : m_visNodes)
    {
        const NodeData* n = model.FindNode(id);
        if (!n || !n->desc) continue;
        const NodeTypeDesc& t = *n->desc;
        const TypeLayout& L = layout.For(t);
        const Rect rg = Rect::FromPosSize(n->pos, L.size);
        const Rect rs = m_vp.ToScreen(rg);
        const ImVec2 mn(rs.min.x, rs.min.y), mx(rs.max.x, rs.max.y);
        if (mx.x < m_canvasMin.x || mn.x > m_canvasMax.x || mx.y < m_canvasMin.y || mn.y > m_canvasMax.y) continue;
        ++drawn;

        const bool selected = m_sel.Has(id);
        NodeAnim& an = m_anim[id];
        AnimStep(an.hover, (id == m_hoverNode && m_mode != Mode::BoxSelect) ? 1.0f : 0.0f, 26.0f, 1.0f);
        AnimStep(an.sel, selected ? 1.0f : 0.0f, 30.0f, 0.62f);
        AnimStep(an.lift, (movingActive && m_moveSel.Has(id)) ? 1.0f : 0.0f, 24.0f, 0.8f);
        const float hv = an.hover.value, sv = an.sel.value, lf = an.lift.value;

        NodeState state = NodeState::Normal;
        const std::string* stateMsg = nullptr;
        if (auto st = m_nodeStates.find(id); st != m_nodeStates.end()) { state = st->second.state; stateMsg = &st->second.message; }
        const float dim = state == NodeState::Dimmed ? 0.45f : 1.0f;

        const int cs = std::min(std::max(t.categorySlot, 0), kCategorySlots - 1);
        const ImU32 catCol = m_tok.headerCat[cs];
        const ImU32 catBright = m_tok.headerCatBright[cs];

        if (emitAnchors)
        {
            ++anchors;
            m_anchor("node", "ng:node:" + std::to_string(id) + ":" + t.id, mn, mx);
        }

        // ---- リルート点 ----
        if (t.isReroute)
        {
            const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
            const float r = (mx.x - mn.x) * 0.5f * 0.62f;
            const PinType rt = model.ResolvedPinType(PinRef{id, 0, true});
            const ImU32 col = m_tok.pinType[std::min(std::max(model.PinTypeInfo(rt).colorSlot, 0), kPinTypeSlots - 1)];
            if (m_set.glow && m_tok.glow > 0.0f && sv > 0.01f) DrawGlow(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), r, m_tok.accent, m_tok.glow * sv, ui::PxF(9.0f));
            dl->AddCircleFilled(c, r * (1.0f + 0.12f * hv), m_tok.nodeInner, 20);
            dl->AddCircleFilled(c, r * 0.62f * (1.0f + 0.12f * hv), col, 16);
            dl->AddCircle(c, r * (1.0f + 0.12f * hv), Mix(m_tok.nodeBorder, m_tok.accentHover, std::max(sv, hv * 0.5f)), 20, std::max(1.0f, b1 * (1.0f + sv)));
            if (emitAnchors)
            {
                m_anchor("pin", "ng:pin:" + std::to_string(id) + ":in:0", ImVec2(mn.x - 4, c.y - 4), ImVec2(mn.x + 4, c.y + 4));
                m_anchor("pin", "ng:pin:" + std::to_string(id) + ":out:0", ImVec2(mx.x - 4, c.y - 4), ImVec2(mx.x + 4, c.y + 4));
            }
            continue;
        }

        // ---- LOD: 塗り矩形 + 色帯だけ ----
        if (lodRect)
        {
            dl->AddRectFilled(mn, mx, AlphaMul(m_tok.nodeBody, dim), R);
            dl->AddRectFilled(mn, ImVec2(mx.x, std::min(mx.y, mn.y + cfg.headerH * scale)), AlphaMul(catCol, dim), R, ImDrawFlags_RoundCornersTop);
            if (selected) dl->AddRect(mn, mx, m_tok.accentHover, R, 0, std::max(1.5f, b1 * 1.5f));
            continue;
        }

        // ---- 影・グロー ----
        if (zoom >= 0.5f)
        {
            const float off = (2.5f + 5.0f * lf) * scale;
            dl->AddRectFilled(ImVec2(mn.x - scale, mn.y + off * 0.6f), ImVec2(mx.x + scale, mx.y + off), AlphaMul(m_tok.nodeShadow, (0.5f + 0.5f * lf) * dim), R + scale * 2.0f);
        }
        const bool glowOn = m_set.glow && m_tok.glow > 0.0f;
        if (glowOn && sv > 0.01f) DrawGlow(dl, mn, mx, R, m_tok.accent, m_tok.glow * std::min(1.3f, sv), ui::PxF(15.0f) * std::min(1.0f, 0.4f + zoom));
        if (state == NodeState::Error)
        {
            const float pulse = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(now * 4.2f));
            DrawGlow(dl, mn, mx, R, m_tok.bad, (glowOn ? m_tok.glow : 0.5f) * 1.5f * pulse, ui::PxF(12.0f));
        }
        else if (state == NodeState::Warning) DrawGlow(dl, mn, mx, R, m_tok.warn, (glowOn ? m_tok.glow : 0.5f) * 0.9f, ui::PxF(9.0f));

        // ---- 本体・ヘッダ ----
        const ImU32 body = AlphaMul(Mix(m_tok.nodeBody, m_tok.nodeBodyHover, hv * 0.55f), dim);
        dl->AddRectFilled(mn, mx, body, R);
        const float hh = L.headerH * scale;
        ImU32 head = Mix(catCol, catBright, hv * 0.22f + sv * 0.14f);
        head = AlphaMul(head, dim);
        dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + hh), head, R, ImDrawFlags_RoundCornersTop);
        // ヘッダ上端の細いハイライトと下端の区切り
        dl->AddLine(ImVec2(mn.x + R, mn.y + b1 * 0.5f + 0.5f), ImVec2(mx.x - R, mn.y + b1 * 0.5f + 0.5f), AlphaMul(IM_COL32(255, 255, 255, 34), dim), 1.0f);
        dl->AddLine(ImVec2(mn.x, mn.y + hh), ImVec2(mx.x, mn.y + hh), AlphaMul(IM_COL32(0, 0, 0, 70), dim), 1.0f);
        if (state == NodeState::Compiling)
        {
            // ゆっくり流れる光（ヘッダの中だけ）
            const float ph = std::fmod(now * 0.55f, 1.4f) - 0.2f;
            const float x0 = mn.x + (mx.x - mn.x) * ph, wq = (mx.x - mn.x) * 0.22f;
            dl->PushClipRect(mn, ImVec2(mx.x, mn.y + hh), true);
            dl->AddRectFilledMultiColor(ImVec2(x0 - wq, mn.y), ImVec2(x0, mn.y + hh), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 46), IM_COL32(255, 255, 255, 46), IM_COL32(255, 255, 255, 0));
            dl->AddRectFilledMultiColor(ImVec2(x0, mn.y), ImVec2(x0 + wq, mn.y + hh), IM_COL32(255, 255, 255, 46), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 46));
            dl->PopClipRect();
        }
        // ---- 枠 ----
        ImU32 border = Mix(m_tok.nodeBorder, m_tok.nodeBorderHover, hv);
        float bw = b1;
        if (sv > 0.01f) { border = Mix(border, m_tok.accentHover, std::min(1.0f, sv)); bw = b1 * (1.0f + 0.7f * std::min(1.0f, sv)); }
        if (state == NodeState::Error) { border = m_tok.bad; bw = b1 * 1.6f; }
        else if (state == NodeState::Warning) { border = m_tok.warn; bw = b1 * 1.4f; }
        dl->AddRect(mn, mx, AlphaMul(border, dim), R, 0, bw);

        // ---- タイトル ----
        if (zoom >= 0.25f)
        {
            float tx = mn.x + cfg.padX * scale;
            if (!t.icon.empty())
            {
                ui::DrawIconCentered(dl, t.icon.c_str(), ImVec2(tx + 6.0f * scale, mn.y + hh * 0.5f), AlphaMul(m_tok.onHeader, dim), std::floor(14.0f * scale));
                tx += 20.0f * scale;
            }
            const float px = std::floor(cfg.titleUnits * scale + 0.5f);
            if (px >= 5.0f)
            {
                const ImVec4 clip(mn.x, mn.y, mx.x - 4.0f * scale, mn.y + hh);
                dl->AddText(FontBold(), px, ImVec2(tx, std::floor(mn.y + (hh - m_lhTitle) * 0.5f + 0.5f)), AlphaMul(m_tok.text, dim), t.title.c_str(), nullptr, 0.0f, &clip);
            }
        }
        // ---- 名前欄（ヘッダ直下。パラメータ名・テクスチャ名など。NodeTypeDesc::hasLabel）----
        if (L.labelH > 0.0f && !lodNoLabels)
        {
            const float ly0 = mn.y + hh, ly1 = mn.y + (L.headerH + L.labelH) * scale;
            const float px = std::floor(cfg.textUnits * scale + 0.5f);
            if (px >= 5.0f && !n->label.empty())
            {
                const float lh = FontBody()->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y;
                const ImVec4 clip(mn.x, ly0, mx.x - cfg.padX * 0.5f * scale, ly1);
                dl->AddText(FontBody(), px, ImVec2(std::floor(mn.x + cfg.padX * scale + 0.5f), std::floor((ly0 + ly1 - lh) * 0.5f + 0.5f)),
                            AlphaMul(m_tok.textMid, dim), n->label.c_str(), nullptr, 0.0f, &clip);
            }
            dl->AddLine(ImVec2(mn.x + cfg.padX * scale * 0.5f, ly1), ImVec2(mx.x - cfg.padX * scale * 0.5f, ly1), AlphaMul(m_tok.nodeBorder, 0.55f * dim), 1.0f);
        }
        // エラーバッジ
        if (state == NodeState::Error || state == NodeState::Warning)
        {
            const ImVec2 bc(mx.x - 11.0f * scale, mn.y + hh * 0.5f);
            const float br = 6.5f * scale;
            const ImU32 bcol = state == NodeState::Error ? m_tok.bad : m_tok.warn;
            dl->AddCircleFilled(bc, br, bcol, 14);
            const float px = std::floor(9.5f * scale);
            if (px >= 5.0f) dl->AddText(FontBold(), px, ImVec2(bc.x - px * 0.24f, bc.y - m_lhTitle * 0.5f * (px / std::max(1.0f, std::floor(cfg.titleUnits * scale)))), IM_COL32(20, 20, 24, 240), "!");
        }

        // ---- ピン・ラベル・値欄 ----
        const float pr = cfg.pinR * scale;
        const auto isConnected = [&](PinRef p) { return std::binary_search(m_connKeys.begin(), m_connKeys.end(), p.Key()); };
        const auto drawPins = [&](bool output)
        {
            const std::vector<PinDesc>& pins = output ? t.outputs : t.inputs;
            const std::vector<float>& ys = output ? L.outY : L.inY;
            for (size_t i = 0; i < pins.size(); ++i)
            {
                const PinDesc& pd = pins[i];
                const PinRef pin{id, static_cast<uint16_t>(i), output};
                const float cy = mn.y + ys[i] * scale;
                const ImVec2 c(output ? mx.x : mn.x, cy);
                const bool propOnly = !output && pd.propertyOnly;
                const bool connected = !propOnly && isConnected(pin);

                // 型（多相は上流から推論した型）
                PinType rt = pd.type;
                if (!propOnly) rt = model.ResolvedPinType(pin);
                const PinTypeDesc& ti = model.PinTypeInfo(rt);
                const ImU32 tcol = m_tok.pinType[std::min(std::max(ti.colorSlot, 0), kPinTypeSlots - 1)];

                // 互換ピンの強調（接続ドラッグ中）
                float alpha = dim;
                float pulse = 0.0f;
                bool compat = false;
                if (linkActive && !propOnly)
                {
                    const bool oppositeSide = (output != m_linkFromOutput) && id != m_linkFrom.node;
                    if (oppositeSide)
                    {
                        compat = m_linkFromOutput ? model.CanConnectTypes(m_linkSrcType, pd.type) : model.CanConnectTypes(pd.type, m_linkSrcType);
                        if (compat) pulse = 0.5f + 0.5f * std::sin(now * 6.0f);
                    }
                    if (!compat && !(pin == m_linkFrom)) alpha *= 0.32f;
                }
                float pa = 0.0f;
                if (auto it = m_pinAnim.find(pin.Key()); it != m_pinAnim.end()) pa = it->second.value;

                if (!propOnly && !lodNoLabels)
                {
                    // ラベル前の下地（ピンが浮いて見えないように）
                }
                if (!propOnly)
                {
                    const float rr = pr * (1.0f + 0.32f * pa + 0.22f * pulse);
                    if (compat && glowOn) dl->AddCircleFilled(c, rr * 2.4f, AlphaMul(tcol, 0.10f + 0.10f * pulse), 20);
                    if (pa > 0.01f && glowOn) dl->AddCircleFilled(c, rr * 2.0f, AlphaMul(tcol, 0.16f * pa * m_tok.glow), 20);
                    DrawPinShape(dl, c, rr, ti.shape, connected || (compat && pulse > 0.6f), tcol, m_tok.nodeInner, alpha);
                    if (emitAnchors)
                        m_anchor("pin", "ng:pin:" + std::to_string(id) + (output ? ":out:" : ":in:") + std::to_string(i), ImVec2(c.x - pr, c.y - pr), ImVec2(c.x + pr, c.y + pr));
                }

                if (lodNoLabels) continue;
                const float px = std::floor(cfg.textUnits * scale + 0.5f);
                if (px < 5.0f || pd.name.empty()) continue;
                const ImU32 lcol = AlphaMul((pa > 0.05f || (compat && pulse > 0.5f)) ? m_tok.text : m_tok.textMid, alpha);
                const float ly = std::floor(cy - m_lhLabel * 0.5f + 0.5f);
                if (output)
                {
                    const float w = FontBody()->CalcTextSizeA(px, FLT_MAX, 0.0f, pd.name.c_str()).x;
                    dl->AddText(FontBody(), px, ImVec2(std::floor(mn.x + L.outLabelRight * scale - w + 0.5f), ly), lcol, pd.name.c_str());
                }
                else
                {
                    const float lx = mn.x + L.inLabelX * scale;
                    dl->AddText(FontBody(), px, ImVec2(std::floor(lx + 0.5f), ly), lcol, pd.name.c_str());
                }
            }
        };
        drawPins(false);
        drawPins(true);

        if (!lodNoSlots)
        {
            for (size_t i = 0; i < t.inputs.size(); ++i)
            {
                const PinDesc& pd = t.inputs[i];
                if (pd.valueKind == ValueKind::None) continue;
                if (!pd.propertyOnly && isConnected(PinRef{id, static_cast<uint16_t>(i), false})) continue;
                const Rect sg = layout.SlotRect(*n, static_cast<int>(i));
                if (sg.Width() <= 0.0f) continue;
                const bool hs = m_hover.kind == Hit::Slot && m_hover.pin.node == id && m_hover.pin.index == i && (m_mode == Mode::Idle);
                DrawSlot(dl, *n, static_cast<int>(i), sg, scale, dim, hs, hs ? m_hover.slotComp : 0);
                if (emitAnchors)
                {
                    const Rect ss = m_vp.ToScreen(sg);
                    m_anchor("slot", "ng:slot:" + std::to_string(id) + ":" + std::to_string(i), ImVec2(ss.min.x, ss.min.y), ImVec2(ss.max.x, ss.max.y));
                }
            }
            if (t.hasPreview && L.preview.Width() > 0.0f && m_set.showPreview)
                DrawPreview(dl, *n, L.preview.Translated(n->pos), scale, catCol);
        }
    }
    m_stats.drawnNodes = drawn;
    (void)anchors;
}

// ---------------------------------------------------------------------------
// オーバーレイ（矩形選択・接続プレビュー・切断ストローク・ツールチップ）
// ---------------------------------------------------------------------------
void GraphView::DrawOverlays(ImDrawList* dl)
{
    IGraphModel& model = m_doc->Model();
    const float scale = m_vp.Scale();
    const float b1 = std::max(1.0f, ui::PxF(1.0f));
    const bool glowOn = m_set.glow && m_tok.glow > 0.0f;

    // 矩形選択
    if (m_mode == Mode::BoxSelect && m_dragStarted)
    {
        const ImVec2 a = V(m_vp.ToScreen(m_pressG)), b = m_mouse;
        const ImVec2 mn(std::min(a.x, b.x), std::min(a.y, b.y)), mx(std::max(a.x, b.x), std::max(a.y, b.y));
        dl->AddRectFilled(mn, mx, AlphaMul(m_tok.accent, 0.10f), 3.0f);
        dl->AddRect(mn, mx, AlphaMul(m_tok.accentHover, 0.85f), 3.0f, 0, b1);
    }

    // 接続プレビュー（点線）
    if (m_mode == Mode::LinkDrag)
    {
        const Vec2 pinG = m_doc->PinPos(m_linkFrom);
        Vec2 endG = m_mouseG;
        if (m_linkHoverAny) endG = m_doc->PinPos(m_linkHoverPin);
        const Bezier c = m_linkFromOutput ? MakeWire(pinG, endG) : MakeWire(endG, pinG);
        const PinType rt = m_linkSrcType;
        ImU32 col = m_tok.pinType[std::min(std::max(model.PinTypeInfo(rt).colorSlot, 0), kPinTypeSlots - 1)];
        if (m_linkHoverAny && !m_linkHoverValid) col = m_tok.bad;
        const float w = std::max(1.0f, 2.0f * m_dpi * std::min(1.5f, std::max(0.55f, m_zoom)));
        constexpr int kN = 56;
        ImVec2 pts[kN + 1];
        for (int i = 0; i <= kN; ++i) pts[i] = V(m_vp.ToScreen(c.At(static_cast<float>(i) / static_cast<float>(kN))));
        if (glowOn && !(m_linkHoverAny && !m_linkHoverValid))
            for (int i = 0; i < kN; ++i) if (i % 4 != 3) dl->AddLine(pts[i], pts[i + 1], AlphaMul(col, 0.14f * m_tok.glow), w * 3.2f);
        for (int i = 0; i < kN; ++i) if (i % 4 != 3) dl->AddLine(pts[i], pts[i + 1], AlphaMul(col, 0.95f), w);
        const ImVec2 end = m_linkFromOutput ? pts[kN] : pts[0];
        dl->AddCircleFilled(end, 4.0f * scale + 1.0f, AlphaMul(col, 0.95f), 12);
        if (m_linkHoverAny && !m_linkHoverValid)
        {
            const float q = 4.5f * scale + 2.0f;
            dl->AddLine(ImVec2(end.x - q, end.y - q), ImVec2(end.x + q, end.y + q), m_tok.bad, b1 * 2.0f);
            dl->AddLine(ImVec2(end.x - q, end.y + q), ImVec2(end.x + q, end.y - q), m_tok.bad, b1 * 2.0f);
            if (!m_linkReason.empty()) ImGui::SetTooltip("%s", m_linkReason.c_str());
        }
    }

    // ワイヤ切断のストローク
    if (m_mode == Mode::CutStroke && m_cutPts.size() >= 1)
    {
        std::vector<ImVec2> pts;
        for (const Vec2& g : m_cutPts) pts.push_back(V(m_vp.ToScreen(g)));
        pts.push_back(m_mouse);
        for (size_t i = 0; i + 1 < pts.size(); ++i) if (i % 3 != 2) dl->AddLine(pts[i], pts[i + 1], AlphaMul(m_tok.bad, 0.95f), b1 * 1.6f);
    }

    // ワンキー握り中（hotkeyHoldClick）: カーソルの横に「何を置くか」を出す
    if (m_hkType && m_hovered)
    {
        const std::string label = m_hkType->title + "  ・  クリックで配置";
        const char key[2] = {m_hkType->hotkey, 0};
        const float pad = ui::PxF(8.0f), kw = ui::PxF(20.0f);
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        const ImVec2 p(m_mouse.x + ui::PxF(18.0f), m_mouse.y + ui::PxF(20.0f));
        const ImVec2 q(p.x + kw + pad * 2.0f + ts.x, p.y + ts.y + pad);
        dl->AddRectFilled(ImVec2(p.x, p.y + 2.0f), ImVec2(q.x, q.y + 3.0f), AlphaMul(m_tok.nodeShadow, 0.8f), ui::PxF(6.0f));
        dl->AddRectFilled(p, q, AlphaMul(m_tok.popupBg, 0.96f), ui::PxF(6.0f));
        dl->AddRect(p, q, AlphaMul(m_tok.accentHover, 0.9f), ui::PxF(6.0f), 0, b1);
        const ImVec2 kp(p.x + pad * 0.5f, p.y + pad * 0.5f);
        dl->AddRectFilled(kp, ImVec2(kp.x + kw, q.y - pad * 0.5f), m_tok.accent, ui::PxF(4.0f));
        const ImVec2 ks = ImGui::CalcTextSize(key);
        dl->AddText(ImVec2(kp.x + (kw - ks.x) * 0.5f, p.y + pad * 0.5f + (ts.y - ks.y) * 0.5f), IM_COL32(6, 16, 31, 255), key);
        dl->AddText(ImVec2(kp.x + kw + pad, p.y + pad * 0.5f), m_tok.text, label.c_str());
    }

    // ツールチップ（ピン: 名前と型 / ノードの状態メッセージ）
    if (m_hovered && m_stationary && m_mode == Mode::Idle && !m_paletteOpen && !m_ctxOpen)
    {
        if (m_hover.kind == Hit::Pin)
        {
            const NodeData* n = model.FindNode(m_hover.pin.node);
            if (n && n->desc)
            {
                const auto& pins = m_hover.pin.output ? n->desc->outputs : n->desc->inputs;
                if (m_hover.pin.index < pins.size())
                {
                    const PinDesc& pd = pins[m_hover.pin.index];
                    const PinType rt = model.ResolvedPinType(m_hover.pin);
                    ImGui::SetTooltip("%s  (%s)%s%s%s", pd.name.empty() ? "(pin)" : pd.name.c_str(), model.PinTypeInfo(rt).name.c_str(),
                                      pd.desc.empty() ? "" : "\n", pd.desc.c_str(),
                                      (!m_hover.pin.output && model.FindEdgeTo(m_hover.pin)) ? "\nAlt+クリック: 切断" : "");
                }
            }
        }
        else if (m_hover.kind == Hit::Node || m_hover.kind == Hit::Slot)
        {
            auto st = m_nodeStates.find(m_hover.node);
            if (st != m_nodeStates.end() && !st->second.message.empty()) ImGui::SetTooltip("%s", st->second.message.c_str());
            else if (const NodeData* n = model.FindNode(m_hover.node); n && n->desc && !n->desc->description.empty()) ImGui::SetTooltip("%s", n->desc->description.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// ミニマップ
// ---------------------------------------------------------------------------
void GraphView::DrawMinimap(ImDrawList* dl)
{
    ImVec2 mn, mx;
    if (!MinimapRect(mn, mx)) return;
    IGraphModel& model = m_doc->Model();
    const float R = ui::PxF(6.0f);
    const float b1 = std::max(1.0f, ui::PxF(1.0f));
    dl->AddRectFilled(ImVec2(mn.x + 1, mn.y + 3), ImVec2(mx.x + 1, mx.y + 4), AlphaMul(m_tok.nodeShadow, 0.7f), R);
    dl->AddRectFilled(mn, mx, AlphaMul(m_tok.popupBg, 0.90f), R);

    Rect b = m_doc->ContentBounds();
    const Rect vis = m_vp.VisibleGraphRect();
    if (IsEmptyRect(b)) b = vis; else b.Add(vis);
    b = b.Expanded(60.0f);
    const float pad = ui::PxF(6.0f);
    const ImVec2 a(mn.x + pad, mn.y + pad), z(mx.x - pad, mx.y - pad);
    const float sc = std::min((z.x - a.x) / std::max(1.0f, b.Width()), (z.y - a.y) / std::max(1.0f, b.Height()));
    const Vec2 cG = b.Center();
    const ImVec2 cM((a.x + z.x) * 0.5f, (a.y + z.y) * 0.5f);
    auto toMap = [&](Vec2 g) { return ImVec2(cM.x + (g.x - cG.x) * sc, cM.y + (g.y - cG.y) * sc); };

    dl->PushClipRect(mn, mx, true);
    for (const Comment& c : m_doc->Comments())
    {
        const ImVec2 p0 = toMap(c.rect.min), p1 = toMap(c.rect.max);
        const int ci = std::min(std::max(c.color, 0), kCommentColors - 1);
        dl->AddRectFilled(p0, p1, AlphaMul(m_tok.commentBorder[ci], 0.22f), 2.0f);
        dl->AddRect(p0, p1, AlphaMul(m_tok.commentBorder[ci], 0.7f), 2.0f, 0, 1.0f);
    }
    for (NodeId id : model.Nodes())
    {
        const NodeData* n = model.FindNode(id);
        if (!n || !n->desc) continue;
        const Rect r = Rect::FromPosSize(n->pos, m_doc->Layout().For(*n->desc).size);
        const ImVec2 p0 = toMap(r.min), p1 = toMap(r.max);
        const ImU32 col = m_tok.headerCatBright[std::min(std::max(n->desc->categorySlot, 0), kCategorySlots - 1)];
        const bool sel = m_sel.Has(id);
        dl->AddRectFilled(p0, ImVec2(std::max(p1.x, p0.x + 2.0f), std::max(p1.y, p0.y + 1.5f)), sel ? m_tok.accentHover : AlphaMul(col, 0.85f));
    }
    // ビューポート枠
    const ImVec2 v0 = toMap(vis.min), v1 = toMap(vis.max);
    dl->AddRectFilled(v0, v1, AlphaMul(m_tok.accent, 0.08f), 2.0f);
    dl->AddRect(v0, v1, AlphaMul(m_tok.accentHover, 0.9f), 2.0f, 0, b1 * 1.2f);
    dl->PopClipRect();
    dl->AddRect(mn, mx, m_tok.popupBorder, R, 0, b1);
}

} // namespace dx12e::ng
