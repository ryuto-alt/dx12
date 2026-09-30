#include "editor/nodegraph/GraphLayout.h"

#include <algorithm>

namespace dx12e::ng
{

float SlotWidth(ValueKind k)
{
    switch (k)
    {
    case ValueKind::Float: return 60.0f;
    case ValueKind::Color: return 60.0f;
    case ValueKind::Bool:  return 18.0f;
    case ValueKind::Enum:  return 88.0f;
    case ValueKind::Vec2:  return 2.0f * 38.0f;
    case ValueKind::Vec3:  return 3.0f * 38.0f;
    case ValueKind::Vec4:  return 4.0f * 38.0f;
    default: return 0.0f;
    }
}

Rect SlotComponentRect(const Rect& slot, ValueKind k, int comp)
{
    if (k == ValueKind::Vec2 || k == ValueKind::Vec3 || k == ValueKind::Vec4)
    {
        const int n = ValueComponentCount(k);
        const float w = slot.Width() / static_cast<float>(n);
        return Rect({slot.min.x + w * static_cast<float>(comp), slot.min.y}, {slot.min.x + w * static_cast<float>(comp + 1), slot.max.y});
    }
    return slot;
}

float GraphLayout::ApproxWidth(const std::string& s, bool title)
{
    // 1 バイトあたり ASCII ≒ 0.54em、日本語（3 バイト）≒ 1em → バイト数ベースで近似。
    const float em = title ? 13.5f : 12.5f;
    float w = 0.0f;
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { w += em * 0.54f; i += 1; }
        else if ((c >> 5) == 0x6) { w += em; i += 2; }
        else if ((c >> 4) == 0xE) { w += em; i += 3; }
        else { w += em; i += 4; }
    }
    return w;
}

GraphLayout::GraphLayout(TextMeasureFn measure, LayoutConfig cfg)
    : m_measure(measure ? std::move(measure) : TextMeasureFn(&GraphLayout::ApproxWidth)), m_cfg(cfg)
{
}

const TypeLayout& GraphLayout::For(const NodeTypeDesc& t)
{
    auto it = m_cache.find(&t);
    if (it != m_cache.end()) return it->second;

    TypeLayout L;
    const LayoutConfig& c = m_cfg;
    if (t.isReroute)
    {
        L.reroute = true;
        L.size = {c.rerouteSize, c.rerouteSize};
        L.inY.assign(std::max<size_t>(1, t.inputs.size()), c.rerouteSize * 0.5f);
        L.outY.assign(std::max<size_t>(1, t.outputs.size()), c.rerouteSize * 0.5f);
        L.inSlot.assign(L.inY.size(), Rect());
        return m_cache.emplace(&t, std::move(L)).first->second;
    }

    L.headerH = c.headerH;
    const size_t rows = std::max(t.inputs.size(), t.outputs.size());

    float maxInLabel = 0.0f, maxOutLabel = 0.0f, maxSlot = 0.0f;
    bool anySlot = false;
    for (const PinDesc& p : t.inputs)
    {
        maxInLabel = std::max(maxInLabel, m_measure(p.name, false));
        const float sw = SlotWidth(p.valueKind);
        if (sw > 0.0f) { anySlot = true; maxSlot = std::max(maxSlot, sw); }
    }
    for (const PinDesc& p : t.outputs) maxOutLabel = std::max(maxOutLabel, m_measure(p.name, false));

    const float leftW = maxInLabel + (anySlot ? c.slotGap + maxSlot : 0.0f);
    const float bodyW = c.padX + leftW + ((!t.outputs.empty() && !t.inputs.empty()) ? c.colGap : 0.0f) + maxOutLabel + c.padX;
    const float titleW = c.padX * 2.0f + m_measure(t.title, true) + (t.icon.empty() ? 0.0f : 20.0f);
    float w = std::max(c.minW, std::max(bodyW, titleW));
    w = std::ceil(w / 8.0f) * 8.0f;   // 8 単位に丸める（見た目が揃い、グリッドスナップと相性が良い）

    L.labelH = t.hasLabel ? c.labelH : 0.0f;
    float y = c.headerH + L.labelH;
    for (size_t i = 0; i < rows; ++i)
    {
        const float cy = y + c.rowH * (static_cast<float>(i) + 0.5f);
        if (i < t.inputs.size()) L.inY.push_back(cy);
        if (i < t.outputs.size()) L.outY.push_back(cy);
    }
    L.inLabelX = c.padX;
    L.slotColX = c.padX + maxInLabel + c.slotGap;
    L.outLabelRight = w - c.padX;
    for (size_t i = 0; i < t.inputs.size(); ++i)
    {
        const PinDesc& p = t.inputs[i];
        const float sw = SlotWidth(p.valueKind);
        if (sw <= 0.0f) { L.inSlot.emplace_back(); continue; }
        const float x0 = L.slotColX;   // 値欄はラベル列の右に揃える（プロパティ行も同じ列）
        L.inSlot.push_back(Rect({x0, L.inY[i] - c.slotH * 0.5f}, {x0 + sw, L.inY[i] + c.slotH * 0.5f}));
    }
    float h = c.headerH + L.labelH + c.rowH * static_cast<float>(rows) + c.padBottom;
    if (rows == 0) h = c.headerH + L.labelH + c.padBottom + 6.0f;
    if (t.hasPreview)
    {
        L.preview = Rect({c.padX, h - c.padBottom + 2.0f}, {w - c.padX, h - c.padBottom + 2.0f + c.previewH});
        h += c.previewH + 4.0f;
    }
    L.size = {w, h};
    return m_cache.emplace(&t, std::move(L)).first->second;
}

Rect GraphLayout::NodeRect(const NodeData& n)
{
    const TypeLayout& L = For(*n.desc);
    return Rect::FromPosSize(n.pos, L.size);
}

Vec2 GraphLayout::PinPos(const NodeData& n, PinRef pin)
{
    const TypeLayout& L = For(*n.desc);
    const std::vector<float>& ys = pin.output ? L.outY : L.inY;
    const float y = pin.index < ys.size() ? ys[pin.index] : L.size.y * 0.5f;
    return {pin.output ? n.pos.x + L.size.x : n.pos.x, n.pos.y + y};
}

Rect GraphLayout::SlotRect(const NodeData& n, int inputIndex)
{
    const TypeLayout& L = For(*n.desc);
    if (inputIndex < 0 || static_cast<size_t>(inputIndex) >= L.inSlot.size()) return Rect();
    return L.inSlot[static_cast<size_t>(inputIndex)].Translated(n.pos);
}

} // namespace dx12e::ng
