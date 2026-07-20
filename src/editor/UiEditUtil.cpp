#include "UiEditUtil.h"

#include <algorithm>
#include <vector>

#include "ecs/Components.h"

namespace dx12e::uiedit
{

namespace
{
    // hit から親へ遡り、UIRect 持ちの選択候補チェーンを「最外殻 → hit」の順で返す。
    // キャンバス（UICanvas 持ち）は含めない。UIRect を持たない中間ノードは素通し。
    std::vector<entt::entity> BuildSelectionChain(entt::registry& reg, entt::entity hit)
    {
        std::vector<entt::entity> chain;   // hit → 祖先 の順で積んで最後に反転
        entt::entity cur = hit;
        for (int guard = 0; guard < 64 && cur != entt::null && reg.valid(cur); ++guard)
        {
            if (reg.all_of<UICanvas>(cur))
                break;
            if (reg.all_of<UIRect>(cur))
                chain.push_back(cur);
            const auto* t = reg.try_get<Transform>(cur);
            cur = t ? t->parent : entt::null;
        }
        std::reverse(chain.begin(), chain.end());
        return chain;
    }
}

void DrawResolvedOutline(ImDrawList* dl, const UiResolvedRect& rr, ImU32 col, float thickness)
{
    if (rr.hasXform)
    {
        const ImVec2 a = rr.xform.Apply(rr.min.x, rr.min.y);
        const ImVec2 b = rr.xform.Apply(rr.max.x, rr.min.y);
        const ImVec2 c = rr.xform.Apply(rr.max.x, rr.max.y);
        const ImVec2 d = rr.xform.Apply(rr.min.x, rr.max.y);
        dl->AddQuad(a, b, c, d, col, thickness);
    }
    else
    {
        dl->AddRect(rr.min, rr.max, col, 0.0f, 0, thickness);
    }
}

bool ResolvedRectContains(const UiResolvedRect& rr, const ImVec2& p)
{
    ImVec2 q = p;
    if (rr.hasXform)
        q = rr.xform.Inverted().Apply(p.x, p.y);
    return q.x >= rr.min.x && q.x < rr.max.x && q.y >= rr.min.y && q.y < rr.max.y;
}

entt::entity ResolveClickTarget(entt::registry& reg, entt::entity hit,
                                entt::entity currentSelection, bool doubleClick)
{
    if (hit == entt::null || !reg.valid(hit) || !reg.all_of<UIRect>(hit))
        return entt::null;

    const std::vector<entt::entity> chain = BuildSelectionChain(reg, hit);
    if (chain.empty())
        return hit;   // 理屈上起きない（hit 自身が UIRect 持ち）が保険

    if (doubleClick)
    {
        // 深掘り: 現在の選択がチェーン上なら 1 段 hit 側へ。経路上に無ければ hit を直接選択。
        for (size_t i = 0; i + 1 < chain.size(); ++i)
            if (chain[i] == currentSelection)
                return chain[i + 1];
        return hit;
    }

    // シングルクリック: 深掘り済みの選択（hit の祖先 or hit 自身）は維持し、
    // それ以外は最外殻のウィジェット（ボタン本体など）を選ぶ。
    for (entt::entity e : chain)
        if (e == currentSelection)
            return currentSelection;
    return chain.front();
}

bool ResolveUiParentRectPx(entt::registry& reg, entt::entity e,
                           DirectX::XMFLOAT2& outMin, DirectX::XMFLOAT2& outMax)
{
    // e の親 → UICanvas の直前 までの祖先を集める（e 自身とキャンバスは含めない）
    std::vector<entt::entity> chain;
    entt::entity canvas = entt::null;
    entt::entity cur = e;
    for (int guard = 0; guard < 64; ++guard)   // 循環親子の保険
    {
        const auto* t = reg.try_get<Transform>(cur);
        entt::entity parent = t ? t->parent : entt::null;
        if (parent == entt::null || !reg.valid(parent)) break;
        if (reg.all_of<UICanvas>(parent)) { canvas = parent; break; }
        chain.push_back(parent);
        cur = parent;
    }
    if (canvas == entt::null) return false;

    const auto& cv = reg.get<UICanvas>(canvas);
    DirectX::XMFLOAT2 mn{0.0f, 0.0f};
    DirectX::XMFLOAT2 mx{(std::max)(1.0f, cv.refWidth), (std::max)(1.0f, cv.refHeight)};

    // キャンバス直下 → e の親 の順に解決（UIRect を持たない中間ノードは親矩形を素通し）
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        const auto* r = reg.try_get<UIRect>(*it);
        if (!r) continue;
        const float pw = mx.x - mn.x;
        const float ph = mx.y - mn.y;
        const DirectX::XMFLOAT2 nmn{mn.x + pw * r->anchorMin.x + r->offsetMin.x,
                                    mn.y + ph * r->anchorMin.y + r->offsetMin.y};
        const DirectX::XMFLOAT2 nmx{mn.x + pw * r->anchorMax.x + r->offsetMax.x,
                                    mn.y + ph * r->anchorMax.y + r->offsetMax.y};
        mn = nmn;
        mx = nmx;
    }
    outMin = mn;
    outMax = mx;
    return true;
}

bool ApplyUiPlacement(entt::registry& reg, entt::entity e, int ax, int ay)
{
    if (!reg.valid(e)) return false;
    auto* r = reg.try_get<UIRect>(e);
    if (!r) return false;

    DirectX::XMFLOAT2 pmin{}, pmax{};
    if (!ResolveUiParentRectPx(reg, e, pmin, pmax)) return false;

    // 現在の解決済みサイズ（キャンバス空間 px）を維持する
    const float pw = pmax.x - pmin.x;
    const float ph = pmax.y - pmin.y;
    const float w = (std::max)(1.0f, (pw * r->anchorMax.x + r->offsetMax.x)
                                   - (pw * r->anchorMin.x + r->offsetMin.x));
    const float h = (std::max)(1.0f, (ph * r->anchorMax.y + r->offsetMax.y)
                                   - (ph * r->anchorMin.y + r->offsetMin.y));

    const float av = std::clamp(ax, 0, 2) * 0.5f;   // 0 / 0.5 / 1
    const float bv = std::clamp(ay, 0, 2) * 0.5f;

    r->anchorMin = {av, bv};
    r->anchorMax = {av, bv};
    // 端はフラッシュ・中央は中央寄せ: アンカー点の av 割合だけ矩形が左（上）に出る配分
    r->offsetMin.x = -w * av;
    r->offsetMax.x =  w * (1.0f - av);
    r->offsetMin.y = -h * bv;
    r->offsetMax.y =  h * (1.0f - bv);
    return true;
}

} // namespace dx12e::uiedit
