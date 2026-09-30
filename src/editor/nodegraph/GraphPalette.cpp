#include "editor/nodegraph/GraphPalette.h"

#include "editor/FuzzyMatch.h"

#include <algorithm>

namespace dx12e::ng
{

std::vector<std::string> PaletteCategories(const IGraphModel& model)
{
    std::vector<std::string> cats;
    for (const NodeTypeDesc& t : model.NodeTypes())
        if (std::find(cats.begin(), cats.end(), t.category) == cats.end()) cats.push_back(t.category);
    return cats;
}

namespace
{
// ドラッグ元ピンに繋がる新ノード側のピン添字（無ければ -1）。型が完全一致するピンを優先する。
int AutoPinFor(const IGraphModel& m, const NodeTypeDesc& t, const PaletteFilter& f)
{
    if (!f.hasPin) return -1;
    int any = -1;
    if (f.pinIsOutput)
    {
        for (size_t i = 0; i < t.inputs.size(); ++i)
        {
            if (t.inputs[i].propertyOnly) continue;
            if (!m.CanConnectTypes(f.pinType, t.inputs[i].type)) continue;
            if (t.inputs[i].type == f.pinType) return static_cast<int>(i);
            if (any < 0) any = static_cast<int>(i);
        }
    }
    else
    {
        for (size_t i = 0; i < t.outputs.size(); ++i)
        {
            if (!m.CanConnectTypes(t.outputs[i].type, f.pinType)) continue;
            if (t.outputs[i].type == f.pinType) return static_cast<int>(i);
            if (any < 0) any = static_cast<int>(i);
        }
    }
    return any;
}
} // namespace

std::vector<PaletteItem> SearchPalette(const IGraphModel& model, const PaletteFilter& f, size_t maxResults)
{
    std::vector<PaletteItem> out;
    const std::vector<NodeTypeDesc>& types = model.NodeTypes();
    const std::vector<std::string> cats = PaletteCategories(model);
    auto catRank = [&](const std::string& c)
    {
        return static_cast<int>(std::find(cats.begin(), cats.end(), c) - cats.begin());
    };

    for (size_t i = 0; i < types.size(); ++i)
    {
        const NodeTypeDesc& t = types[i];
        if (!f.category.empty() && t.category != f.category) continue;
        PaletteItem it;
        it.typeIndex = static_cast<int>(i);
        if (f.hasPin)
        {
            it.autoPin = AutoPinFor(model, t, f);
            if (it.autoPin < 0) continue;
        }
        if (!f.query.empty())
        {
            std::vector<uint32_t> hl;
            const int st = fuzzy::Score(f.query, t.title, &hl);
            const int sk = fuzzy::Score(f.query, t.category + " " + t.keywords + " " + t.id);
            if (st < 0 && sk < 0) continue;
            it.score = std::max(st >= 0 ? st * 2 + 30 : -1, sk >= 0 ? sk : -1);
            if (st >= 0) it.titleHighlight = std::move(hl);
        }
        out.push_back(std::move(it));
    }

    std::stable_sort(out.begin(), out.end(), [&](const PaletteItem& a, const PaletteItem& b)
    {
        if (a.score != b.score) return a.score > b.score;
        const NodeTypeDesc& ta = types[static_cast<size_t>(a.typeIndex)];
        const NodeTypeDesc& tb = types[static_cast<size_t>(b.typeIndex)];
        const int ca = catRank(ta.category), cb = catRank(tb.category);
        if (ca != cb) return ca < cb;
        return ta.title < tb.title;
    });
    if (out.size() > maxResults) out.resize(maxResults);
    return out;
}

} // namespace dx12e::ng
