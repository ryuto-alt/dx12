#include "UiEditorPanel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "editor/EditorContext.h"
#include "editor/UiEditUtil.h"
#include "editor/UndoSystem.h"
#include "editor/panels/AssetBrowserPanel.h"
#include "ui/UISystem.h"

namespace dx12e
{

namespace
{
    // ---- 移動 / リサイズドラッグ（SceneViewPanel の UI 編集と同じ規約）----
    constexpr int kDragNone = -1;
    constexpr int kDragMove = 0;
    constexpr int kEdgeL = 1, kEdgeR = 2, kEdgeT = 4, kEdgeB = 8;

    constexpr f32 kHandleHalf    = 4.0f;   // リサイズハンドル矩形の半径（描画）
    constexpr f32 kHandleHitHalf = 6.0f;   // ハンドルの当たり判定半径

    // アンカーハンドル（三角）の寸法・当たり判定
    constexpr f32 kAnchorTriLen  = 12.0f;  // 先端までの長さ
    constexpr f32 kAnchorTriHalf = 5.0f;   // 底辺の半分
    constexpr f32 kAnchorHitR    = 9.0f;   // 当たり判定半径
    constexpr f32 kAnchorSnapPx  = 6.0f;   // 0 / 0.5 / 1 へのスナップ（スクリーン px）

    // 配色（テーマの Accent 0x4c8dff に合わせる。SceneView の UI 編集と同色）
    constexpr ImU32 kAccentCol    = IM_COL32(76, 141, 255, 255);
    constexpr ImU32 kHandleCol    = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kHandleHotCol = IM_COL32(255, 204, 26, 255);
    constexpr ImU32 kHoverCol     = IM_COL32(200, 200, 200, 160);
    constexpr ImU32 kAnchorCol    = IM_COL32(255, 255, 255, 210);
    constexpr ImU32 kAnchorHotCol = IM_COL32(255, 204, 26, 255);
    constexpr ImU32 kScreenBorder = IM_COL32(110, 110, 120, 255);
    constexpr ImU32 kRegionBg     = IM_COL32(24, 24, 27, 255);
    constexpr ImU32 kScreenBgA    = IM_COL32(38, 38, 42, 255);   // 市松の明
    constexpr ImU32 kScreenBgB    = IM_COL32(32, 32, 36, 255);   // 市松の暗
    constexpr ImU32 kGridCol      = IM_COL32(255, 255, 255, 14);

    // 画面サイズプリセット（w=0: Canvas 基準解像度、w<0: カスタム入力）
    struct ScreenSizePreset { const char* label; int w, h; };
    constexpr ScreenSizePreset kScreenSizes[] = {
        {"Canvas 基準",            0,    0},
        {"1920 x 1080 (FHD)",   1920, 1080},
        {"1280 x 720 (HD)",     1280,  720},
        {"2560 x 1440 (QHD)",   2560, 1440},
        {"3840 x 2160 (4K)",    3840, 2160},
        {"1080 x 1920 (縦)",    1080, 1920},
        {"カスタム",              -1,   -1},
    };

    // 8 ハンドル（四隅 + 四辺中点）。配列順 = 当たり判定の優先順（角が先）。
    struct UiHandle { f32 x, y; int edges; ImGuiMouseCursor cursor; };
    void MakeHandles(const UiResolvedRect& rr, UiHandle out[8])
    {
        const f32 midX = (rr.min.x + rr.max.x) * 0.5f;
        const f32 midY = (rr.min.y + rr.max.y) * 0.5f;
        out[0] = {rr.min.x, rr.min.y, kEdgeL | kEdgeT, ImGuiMouseCursor_ResizeNWSE};
        out[1] = {rr.max.x, rr.min.y, kEdgeR | kEdgeT, ImGuiMouseCursor_ResizeNESW};
        out[2] = {rr.min.x, rr.max.y, kEdgeL | kEdgeB, ImGuiMouseCursor_ResizeNESW};
        out[3] = {rr.max.x, rr.max.y, kEdgeR | kEdgeB, ImGuiMouseCursor_ResizeNWSE};
        out[4] = {midX,     rr.min.y, kEdgeT,          ImGuiMouseCursor_ResizeNS};
        out[5] = {midX,     rr.max.y, kEdgeB,          ImGuiMouseCursor_ResizeNS};
        out[6] = {rr.min.x, midY,     kEdgeL,          ImGuiMouseCursor_ResizeEW};
        out[7] = {rr.max.x, midY,     kEdgeR,          ImGuiMouseCursor_ResizeEW};
    }

    // 選択要素の「親矩形」をスクリーン座標で解決する。
    // 親チェーンを遡り、最初の UIRect 持ちの解決済み矩形、または属するキャンバスの矩形
    // （canvasOrigin + 基準解像度 * canvasScale）を返す。解決できなければ false。
    bool ResolveParentScreenRect(entt::registry& reg, const std::vector<UiResolvedRect>& rects,
                                 const UiResolvedRect& sel, ImVec2& outMin, ImVec2& outMax)
    {
        entt::entity cur = sel.e;
        for (int guard = 0; guard < 64; ++guard)   // 循環親子の保険
        {
            const auto* t = reg.try_get<Transform>(cur);
            const entt::entity p = t ? t->parent : entt::null;
            if (p == entt::null || !reg.valid(p))
                return false;
            if (reg.all_of<UICanvas>(p))
            {
                const auto& cv = reg.get<UICanvas>(p);
                outMin = sel.canvasOrigin;
                outMax = ImVec2(sel.canvasOrigin.x + (std::max)(1.0f, cv.refWidth)  * sel.canvasScale,
                                sel.canvasOrigin.y + (std::max)(1.0f, cv.refHeight) * sel.canvasScale);
                return true;
            }
            if (reg.all_of<UIRect>(p))
            {
                for (const auto& rr : rects)
                    if (rr.e == p) { outMin = rr.min; outMax = rr.max; return true; }
                return false;   // 親が非表示等で今フレーム解決されていない
            }
            cur = p;   // UIRect を持たない中間ノードは素通し（レイアウト解決と同じ規約）
        }
        return false;
    }

    // アンカー値のスナップ（0 / 0.5 / 1）。threshold は正規化値。
    f32 SnapAnchor(f32 v, f32 threshold)
    {
        for (f32 s : {0.0f, 0.5f, 1.0f})
            if (std::fabs(v - s) < threshold) return s;
        return v;
    }

    // ---- スマートガイド（Figma 風の整列吸着）----
    // 移動中の要素の 3 点（min / 中央 / max）を、他要素と親キャンバスの同じ 3 点へ吸着させる。
    // 判定はスクリーン座標で行う = ズーム率によらず「見た目で 7px 近づいたら吸く」になる。
    constexpr f32 kSmartSnapPx = 7.0f;

    struct SnapResult
    {
        f32  delta = 0.0f;   // 補正量（スクリーン px）
        bool hit   = false;
        f32  line  = 0.0f;   // 吸着した相手の座標（ガイド線を引く位置）
    };

    // mine: 自分側の 3 点 / cand: 相手側の候補座標。最も近い組み合わせ 1 つを返す。
    SnapResult SnapToCandidates(const f32 mine[3], const std::vector<f32>& cand)
    {
        SnapResult best;
        f32 bestDist = kSmartSnapPx;
        for (f32 c : cand)
        {
            for (int i = 0; i < 3; ++i)
            {
                const f32 d = c - mine[i];
                if (std::fabs(d) >= bestDist) continue;
                bestDist  = std::fabs(d);
                best.delta = d;
                best.hit   = true;
                best.line  = c;
            }
        }
        return best;
    }

    const char* EntityName(entt::registry& reg, entt::entity e)
    {
        if (const auto* tag = reg.try_get<NameTag>(e))
            return tag->name.c_str();
        return "(no name)";
    }

    // 名前ラベル（矩形の左上の上に、視認用の影つきで描く）
    void DrawNameLabel(ImDrawList* dl, const ImVec2& rectMin, const char* text, ImU32 col)
    {
        const ImVec2 pos(rectMin.x, rectMin.y - ImGui::GetTextLineHeight() - 3.0f);
        dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 200), text);
        dl->AddText(pos, col, text);
    }

    // ---- 階層ツリー用ヘルパー ----

    // ツリー右端に薄く出す種別ラベル
    const char* UiKindLabel(entt::registry& reg, entt::entity e)
    {
        if (reg.all_of<UICanvas>(e)) return "Canvas";
        if (reg.all_of<UIButton>(e)) return "Button";
        if (reg.all_of<UISlider>(e)) return "Slider";
        if (reg.all_of<UIToggle>(e)) return "Toggle";
        if (reg.all_of<UIScrollView>(e)) return "Scroll";
        if (reg.all_of<UILayout>(e)) return "Layout";
        if (reg.all_of<UIImage>(e))  return "Image";
        if (reg.all_of<UIText>(e))   return "Text";
        if (reg.all_of<UIRect>(e))   return "Rect";
        return "";
    }

    // node が ancestor の子孫か（自分自身は含まない。循環親子の保険つき）
    bool IsDescendantOf(entt::registry& reg, entt::entity node, entt::entity ancestor)
    {
        entt::entity cur = node;
        for (int guard = 0; guard < 64; ++guard)
        {
            const auto* t = reg.try_get<Transform>(cur);
            if (!t || t->parent == entt::null || !reg.valid(t->parent)) return false;
            if (t->parent == ancestor) return true;
            cur = t->parent;
        }
        return false;
    }

    // parent の子を UIRect::order 昇順（stable = 同値・UIRect 無しは registry 走査順）で返す。
    // UISystem::ResolveAndDrawCanvases の兄弟順と同じ規約。
    std::vector<entt::entity> SortedChildrenOf(entt::registry& reg, entt::entity parent)
    {
        std::vector<entt::entity> list;
        for (auto [e, t] : reg.view<const Transform>().each())
            if (t.parent == parent) list.push_back(e);
        std::stable_sort(list.begin(), list.end(),
                         [&reg](entt::entity a, entt::entity b)
                         {
                             const auto* ra = reg.try_get<UIRect>(a);
                             const auto* rb = reg.try_get<UIRect>(b);
                             return (ra ? ra->order : 0) < (rb ? rb->order : 0);
                         });
        return list;
    }

    // 親チェーンを遡って属する UICanvas を返す（e 自身がキャンバスならそれ。無ければ null）
    entt::entity FindCanvasOf(entt::registry& reg, entt::entity e)
    {
        entt::entity cur = e;
        for (int guard = 0; guard < 64; ++guard)
        {
            if (!reg.valid(cur)) return entt::null;
            if (reg.all_of<UICanvas>(cur)) return cur;
            const auto* t = reg.try_get<Transform>(cur);
            if (!t || t->parent == entt::null) return entt::null;
            cur = t->parent;
        }
        return entt::null;
    }

    // 見た目の矩形を保ったまま親を差し替える（アンカードラッグと同じ offset 再計算）。
    // 別キャンバスへの移動・矩形が解決できない（非表示等）場合は offset を触らず親差し替えのみ。
    void ReparentPreservingRect(entt::registry& reg, entt::entity e, entt::entity newParent)
    {
        auto* tr = reg.try_get<Transform>(e);
        if (!tr) return;
        auto* rect = reg.try_get<UIRect>(e);

        // キャンバス空間の値はビューポート非依存なので、解決用のビューポートは何でもよい
        std::vector<UiResolvedRect> rects;
        if (rect && FindCanvasOf(reg, e) != entt::null
            && FindCanvasOf(reg, e) == FindCanvasOf(reg, newParent))
            UISystem::ResolveRects(reg, 0.0f, 0.0f, 1920.0f, 1080.0f, rects);

        const UiResolvedRect* er = nullptr;
        for (const auto& rr : rects)
            if (rr.e == e) { er = &rr; break; }

        tr->parent = newParent;
        if (!er) return;

        // 新しい親矩形（rects は付け替え前の解決だが、親チェーンは付け替え後を遡る）
        ImVec2 pMinS, pMaxS;
        if (!ResolveParentScreenRect(reg, rects, *er, pMinS, pMaxS)) return;

        const f32 cs = (er->canvasScale > 1e-6f) ? er->canvasScale : 1.0f;
        const ImVec2 rMinC((er->min.x - er->canvasOrigin.x) / cs, (er->min.y - er->canvasOrigin.y) / cs);
        const ImVec2 rMaxC((er->max.x - er->canvasOrigin.x) / cs, (er->max.y - er->canvasOrigin.y) / cs);
        const ImVec2 pMinC((pMinS.x - er->canvasOrigin.x) / cs, (pMinS.y - er->canvasOrigin.y) / cs);
        const ImVec2 pMaxC((pMaxS.x - er->canvasOrigin.x) / cs, (pMaxS.y - er->canvasOrigin.y) / cs);
        const f32 pw = (std::max)(1e-3f, pMaxC.x - pMinC.x);
        const f32 ph = (std::max)(1e-3f, pMaxC.y - pMinC.y);

        // 解決式: rectMin = parentMin + parentSize * anchorMin + offsetMin（アンカードラッグと同じ）
        rect->offsetMin.x = rMinC.x - (pMinC.x + pw * rect->anchorMin.x);
        rect->offsetMin.y = rMinC.y - (pMinC.y + ph * rect->anchorMin.y);
        rect->offsetMax.x = rMaxC.x - (pMinC.x + pw * rect->anchorMax.x);
        rect->offsetMax.y = rMaxC.y - (pMinC.y + ph * rect->anchorMax.y);
    }

    // dropped を newParent の子へ移し（既に子ならその場で）、insertIdx の位置へ並べ替えて
    // 兄弟全体の UIRect::order を 0..n で振り直す。insertIdx < 0 = 末尾。
    void ApplyUiDrop(entt::registry& reg, entt::entity dropped, entt::entity newParent, int insertIdx)
    {
        auto* tr = reg.try_get<Transform>(dropped);
        if (!tr || !reg.valid(newParent)) return;

        if (tr->parent != newParent)
            ReparentPreservingRect(reg, dropped, newParent);

        std::vector<entt::entity> list = SortedChildrenOf(reg, newParent);
        list.erase(std::remove(list.begin(), list.end(), dropped), list.end());
        if (insertIdx < 0 || insertIdx > static_cast<int>(list.size()))
            insertIdx = static_cast<int>(list.size());
        list.insert(list.begin() + insertIdx, dropped);
        int i = 0;
        for (entt::entity c : list)
            if (auto* r = reg.try_get<UIRect>(c)) r->order = i++;
    }
}

// ===== 階層ツリー（左ペイン）=====

void UiEditorPanel::DrawUiTreeNode(entt::registry& reg, EditorContext& ctx, entt::entity e,
                                   const UiChildrenMap& children)
{
    if (!reg.valid(e)) return;
    const auto it = children.find(e);
    const bool leaf     = (it == children.end() || it->second.empty());
    const bool isCanvas = reg.all_of<UICanvas>(e);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_DefaultOpen;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf;
    if (ctx.IsSelected(e)) flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(static_cast<int>(static_cast<u32>(e)));
    const bool open = ImGui::TreeNodeEx(EntityName(reg, e), flags);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
    {
        if (ImGui::GetIO().KeyCtrl) ctx.ToggleSelection(e);
        else                        ctx.Select(e);
    }

    // ドラッグ元（キャンバスは sortOrder で並ぶので対象外。UIRect 持ちの要素のみ）
    if (!isCanvas && reg.all_of<UIRect>(e)
        && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers))
    {
        ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &e, sizeof(entt::entity));
        ImGui::TextUnformatted(EntityName(reg, e));
        ImGui::EndDragDropSource();
    }

    // ドロップ先: 要素の上下 1/4 = その前/後へ挿入（兄弟並べ替え）、中央 = 子にする（親変更）。
    // キャンバスは「子にする」のみ。プレビュー（挿入線/枠）を描き、確定はツリー走査後に適用。
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                "HIERARCHY_ENTITY",
                ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
        {
            const entt::entity dropped = *static_cast<const entt::entity*>(payload->Data);
            const bool valid = reg.valid(dropped) && dropped != e
                            && reg.all_of<UIRect>(dropped)          // UI 要素のみ受ける
                            && !IsDescendantOf(reg, e, dropped);    // 自分の子孫へは落とせない
            if (valid)
            {
                const ImVec2 rmin = ImGui::GetItemRectMin();
                const ImVec2 rmax = ImGui::GetItemRectMax();
                const f32 h  = rmax.y - rmin.y;
                const f32 my = ImGui::GetIO().MousePos.y;
                int zone = 1;   // 0=前へ挿入, 1=子にする, 2=後へ挿入
                if (!isCanvas)
                {
                    if      (my < rmin.y + h * 0.25f) zone = 0;
                    else if (my > rmax.y - h * 0.25f) zone = 2;
                }

                ImDrawList* fdl = ImGui::GetWindowDrawList();
                if (zone == 1)
                    fdl->AddRect(rmin, rmax, kAccentCol);
                else
                {
                    const f32 y = (zone == 0) ? rmin.y : rmax.y;
                    fdl->AddLine(ImVec2(rmin.x, y), ImVec2(rmax.x, y), kAccentCol, 2.0f);
                }

                if (payload->IsDelivery())
                {
                    if (zone == 1)
                    {
                        m_dropEntity = dropped; m_dropParent = e; m_dropIndex = -1;
                    }
                    else if (const auto* t = reg.try_get<Transform>(e);
                             t && t->parent != entt::null && reg.valid(t->parent))
                    {
                        // 兄弟リスト内（dropped 除外後）の e の位置を求めて前/後へ
                        int idx = 0;
                        for (entt::entity sib : SortedChildrenOf(reg, t->parent))
                        {
                            if (sib == dropped) continue;
                            if (sib == e) break;
                            ++idx;
                        }
                        m_dropEntity = dropped;
                        m_dropParent = t->parent;
                        m_dropIndex  = idx + (zone == 2 ? 1 : 0);
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 種別を薄く添える
    if (const char* kind = UiKindLabel(reg, e); kind[0])
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", kind);
    }

    if (open)
    {
        if (it != children.end())
            for (entt::entity c : it->second)
                DrawUiTreeNode(reg, ctx, c, children);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void UiEditorPanel::DrawHierarchyPane(entt::registry& reg, EditorContext& ctx)
{
    ImGui::TextDisabled("階層  (上=奥 / 下=手前)");
    ImGui::SetItemTooltip("ドラッグ&ドロップ:\n"
                          "  要素の中央へ = その子にする（親変更）\n"
                          "  要素の上/下端へ = その前/後へ挿入（描画順の並べ替え）");
    ImGui::Separator();

    // キャンバス一覧（sortOrder 昇順 = UISystem の描画順と同じ）
    struct Entry { entt::entity e; int order; };
    std::vector<Entry> canvases;
    for (auto [e, cv] : reg.view<const UICanvas>().each())
        canvases.push_back({e, cv.sortOrder});
    std::stable_sort(canvases.begin(), canvases.end(),
                     [](const Entry& a, const Entry& b) { return a.order < b.order; });
    if (canvases.empty())
    {
        ImGui::TextDisabled("UICanvas がありません");
        return;
    }

    // 兄弟順つき子リストのスナップショット（UISystem::ResolveAndDrawCanvases と同じ規約）。
    // D&D 確定はツリー走査後（m_dropEntity）に適用 = 走査中に registry の親子を書き換えない。
    UiChildrenMap children;
    for (auto [e, t] : reg.view<const Transform>().each())
        if (t.parent != entt::null && reg.valid(t.parent))
            children[t.parent].push_back(e);
    for (auto& [parent, list] : children)
        std::stable_sort(list.begin(), list.end(),
                         [&reg](entt::entity a, entt::entity b)
                         {
                             const auto* ra = reg.try_get<UIRect>(a);
                             const auto* rb = reg.try_get<UIRect>(b);
                             return (ra ? ra->order : 0) < (rb ? rb->order : 0);
                         });

    m_dropEntity = entt::null;
    for (const Entry& c : canvases)
        DrawUiTreeNode(reg, ctx, c.e, children);

    if (m_dropEntity != entt::null)
    {
        // D&D（親変更 + 兄弟並べ替え）は Transform::parent と、新旧の兄弟全部の
        // UIRect::order/offset を書き換える。どれが変わるか事前には分からないので、
        // 関係しうるエンティティ（動かす要素 + 新旧の親の子全部）を丸ごとスナップショットし、
        // 実際に変わったものだけ CompositeCommand へ束ねる = Ctrl+Z 1 回で完全に戻る。
        std::vector<entt::entity> touched{m_dropEntity};
        const auto* movedTr = reg.try_get<Transform>(m_dropEntity);
        for (entt::entity parent : {movedTr ? movedTr->parent : entt::null, m_dropParent})
        {
            if (parent == entt::null || !reg.valid(parent)) continue;
            for (entt::entity c : SortedChildrenOf(reg, parent))
                if (std::find(touched.begin(), touched.end(), c) == touched.end())
                    touched.push_back(c);
        }

        std::vector<std::pair<entt::entity, UIRect>> rectBefore;
        for (entt::entity e : touched)
            if (const auto* r = reg.try_get<UIRect>(e)) rectBefore.emplace_back(e, *r);
        const Transform trBefore = movedTr ? *movedTr : Transform{};
        const bool hadTransform = (movedTr != nullptr);

        ApplyUiDrop(reg, m_dropEntity, m_dropParent, m_dropIndex);
        m_dropEntity = entt::null;

        auto composite = std::make_unique<CompositeCommand>("UI 階層の変更");
        if (hadTransform)
        {
            const auto* trAfter = reg.try_get<Transform>(touched[0]);
            if (trAfter && trAfter->parent != trBefore.parent)
                composite->Add(std::make_unique<ComponentEditCommand<Transform>>(
                    &reg, touched[0], trBefore, *trAfter, "UI Reparent"));
        }
        for (const auto& [e, before] : rectBefore)
        {
            const auto* after = reg.try_get<UIRect>(e);
            if (!after || std::memcmp(&before, after, sizeof(UIRect)) == 0) continue;
            composite->Add(std::make_unique<ComponentEditCommand<UIRect>>(
                &reg, e, before, *after, "UI Rect"));
        }
        if (!composite->Empty()) ctx.undoSystem.PushCommand(std::move(composite));
    }
}

void UiEditorPanel::AlignSelection(entt::registry& reg, EditorContext& ctx, int mode)
{
    // 対象は「UIRect を持つ選択要素」だけ。並べ替えは解決済みのスクリーン矩形ではなく
    // UIRect の offset（= キャンバス空間）で行う。アンカーが違う要素が混ざっていても
    // offset を直接動かす限り「見た目の移動量」と一致する。
    struct Item { entt::entity e; UIRect before; f32 minv, maxv; };
    std::vector<Item> items;
    for (entt::entity e : ctx.selectedEntities)
    {
        if (!reg.valid(e) || !reg.all_of<UIRect>(e)) continue;
        const UIRect& r = reg.get<UIRect>(e);
        const bool horiz = (mode <= 2) || mode == 6;
        items.push_back({e, r,
                         horiz ? r.offsetMin.x : r.offsetMin.y,
                         horiz ? r.offsetMax.x : r.offsetMax.y});
    }
    if (items.size() < 2) return;

    // 全体の外接範囲（整列の基準）
    f32 lo = FLT_MAX, hi = -FLT_MAX;
    for (const auto& it : items) { lo = (std::min)(lo, it.minv); hi = (std::max)(hi, it.maxv); }

    auto moveBy = [&](Item& it, f32 d)
    {
        auto& r = reg.get<UIRect>(it.e);
        if (mode <= 2 || mode == 6) { r.offsetMin.x += d; r.offsetMax.x += d; }
        else                        { r.offsetMin.y += d; r.offsetMax.y += d; }
    };

    if (mode == 6 || mode == 7)
    {
        // 等間隔分布: 両端を固定したまま、間の要素の「隙間」を均す。
        // 中心を等分するのではなく隙間を等分するのが Figma / Illustrator の既定挙動。
        std::sort(items.begin(), items.end(),
                  [](const Item& a, const Item& b) { return a.minv < b.minv; });
        f32 totalSize = 0.0f;
        for (const auto& it : items) totalSize += (it.maxv - it.minv);
        const f32 gap = ((hi - lo) - totalSize) / static_cast<f32>(items.size() - 1);

        f32 cursor = lo;
        for (auto& it : items)
        {
            moveBy(it, cursor - it.minv);
            cursor += (it.maxv - it.minv) + gap;
        }
    }
    else
    {
        const int kind = mode % 3;   // 0=手前側 1=中央 2=奥側
        const f32 center = (lo + hi) * 0.5f;
        for (auto& it : items)
        {
            f32 d = 0.0f;
            if (kind == 0)      d = lo - it.minv;
            else if (kind == 1) d = center - (it.minv + it.maxv) * 0.5f;
            else                d = hi - it.maxv;
            moveBy(it, d);
        }
    }

    for (const auto& it : items)
    {
        const UIRect& after = reg.get<UIRect>(it.e);
        if (std::memcmp(&it.before, &after, sizeof(UIRect)) == 0) continue;
        ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<UIRect>>(
            &reg, it.e, it.before, after, "UI Align"));
    }
}

void UiEditorPanel::DrawPrefabPalette(const std::string& assetsDir)
{
    namespace fs = std::filesystem;

    ImGui::TextDisabled("UI プレハブ");
    ImGui::SameLine();
    if (ImGui::SmallButton("更新")) m_prefabListLoaded = false;
    ImGui::SetItemTooltip("assets/prefabs/ui/ の一覧を読み直す");

    if (!m_prefabListLoaded)
    {
        m_prefabPaths.clear();
        std::error_code ec;
        const fs::path dir = fs::path(assetsDir) / "prefabs" / "ui";
        if (fs::exists(dir, ec))
        {
            for (auto& entry : fs::directory_iterator(dir, ec))
            {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() != ".prefab") continue;
                m_prefabPaths.push_back(entry.path().string());
            }
            std::sort(m_prefabPaths.begin(), m_prefabPaths.end());
        }
        m_prefabListLoaded = true;
    }

    if (m_prefabPaths.empty())
    {
        ImGui::TextDisabled("(空)\n要素を右クリック →\n「プレハブにする」で作る");
        return;
    }

    if (ImGui::BeginChild("##UiPrefabList", ImVec2(0.0f, 0.0f)))
    {
        for (const auto& path : m_prefabPaths)
        {
            const std::string name = fs::path(path).stem().string();
            ImGui::Selectable(name.c_str());
            // キャンバスへドラッグして配置できるよう、アセットブラウザと同じ payload を出す
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                ImGui::SetDragDropPayload(AssetBrowserPanel::kDragDropPayloadType,
                                          path.c_str(), path.size() + 1);
                ImGui::Text("%s", name.c_str());
                ImGui::TextDisabled("キャンバスへドロップ");
                ImGui::EndDragDropSource();
            }
            // ダブルクリックはキャンバス中央へ配置（ドラッグせずに置きたい時用）
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_pendingPrefabDrop = path;
                m_pendingPrefabDropPos = ImVec2(-1.0f, -1.0f);   // 画面外 = 中央配置へフォールバック
            }
        }
    }
    ImGui::EndChild();
}

void UiEditorPanel::RenderWindow(entt::registry& reg, EditorContext& ctx, const std::string& assetsDir,
                                 ResourceManager* resources, DescriptorHeap* srvHeap,
                                 ID3D12GraphicsCommandList* cmdList)
{
    if (!ctx.showUiEditor)
    {
        m_dragEdges = kDragNone;
        m_anchorDrag = -1;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(1040.0f, 720.0f), ImGuiCond_FirstUseEver);
    // NoDocking: 右下タブ領域(狭い)へ誤ってドッキングされて手狭になるのを防ぐ。常にフローティング。
    if (!ImGui::Begin("UIエディタ###UiEditorPanelFloating", &ctx.showUiEditor,
                      ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }
    // 3D ビューポートの上に浮かぶことがあるので、ホバー中はシーンカメラが同時に反応しないよう
    // EditorContext 経由で伝える（VfxEditor と同じ 2 値ラッチ）。
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                               | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem
                               | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        ctx.floatingToolWindowHoveredThisFrame = true;

    ImGuiIO& io = ImGui::GetIO();

    const bool selHasRectTb = ctx.selectedEntity != entt::null
        && reg.valid(ctx.selectedEntity) && reg.all_of<UIRect>(ctx.selectedEntity);

    // ===== ツールバー（パレット + 配置 + 画面サイズ + ズーム）=====
    const auto spawnMarker = [&ctx](const char* marker) {
        PendingSpawnRequest req;
        req.modelPath = marker;
        ctx.pendingSpawns.push_back(req);
    };

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("追加:");
    ImGui::SameLine();
    if (ImGui::Button("+ Canvas")) spawnMarker("__ui_canvas__");
    ImGui::SetItemTooltip("UICanvas を追加（UI のルート。基準解像度を持つ）");
    ImGui::SameLine();
    if (ImGui::Button("+ 画像")) spawnMarker("__ui_image__");
    ImGui::SetItemTooltip("UIImage を追加（選択中の UI 要素の子として。無選択なら Canvas 直下）");
    ImGui::SameLine();
    if (ImGui::Button("+ テキスト")) spawnMarker("__ui_text__");
    ImGui::SetItemTooltip("UIText を追加");
    ImGui::SameLine();
    if (ImGui::Button("+ ボタン")) spawnMarker("__ui_button__");
    ImGui::SetItemTooltip("UIButton を追加（背景画像 + ラベル付き）");
    ImGui::SameLine();
    if (ImGui::Button("+ スライダー")) spawnMarker("__ui_slider__");
    ImGui::SetItemTooltip("UISlider を追加（音量調整等。値変更で onChangeEvent を emit）");
    ImGui::SameLine();
    if (ImGui::Button("+ トグル")) spawnMarker("__ui_toggle__");
    ImGui::SetItemTooltip("UIToggle を追加（チェックボックス + ラベル付き）");
    ImGui::SameLine();
    if (ImGui::Button("+ スクロール")) spawnMarker("__ui_scrollview__");
    ImGui::SetItemTooltip("UIScrollView を追加（子をクリップ + ホイールスクロール。\n"
                          "階層ツリーで子をぶら下げて使う）");

    ImGui::SameLine(0.0f, 14.0f);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0.0f, 14.0f);

    // ---- 配置テンプレ（9 方位。アンカー + 位置を同時スナップ。Unity の Alt+プリセット相当）----
    ImGui::BeginDisabled(!selHasRectTb);
    if (ImGui::Button("配置 \xe2\x96\xbe"))   // ▾
        ImGui::OpenPopup("##UiEdPlacement");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("選択中の UI 要素を親矩形の 9 方位へスナップ配置\n"
                          "（アンカーも同時に設定 = 解像度が変わっても追従する）");
    if (ImGui::BeginPopup("##UiEdPlacement"))
    {
        static const char* kPlaceLabels[3][3] = {
            {"\xe2\x86\x96", "\xe2\x86\x91", "\xe2\x86\x97"},   // ↖ ↑ ↗
            {"\xe2\x86\x90", "\xe2\x97\x8f", "\xe2\x86\x92"},   // ← ● →
            {"\xe2\x86\x99", "\xe2\x86\x93", "\xe2\x86\x98"},   // ↙ ↓ ↘
        };
        ImGui::TextDisabled("配置（アンカー + 位置）");
        for (int ry = 0; ry < 3; ++ry)
        {
            for (int rx = 0; rx < 3; ++rx)
            {
                if (rx > 0) ImGui::SameLine();
                ImGui::PushID(ry * 3 + rx);
                if (ImGui::Button(kPlaceLabels[ry][rx], ImVec2(30.0f, 30.0f))
                    && selHasRectTb)
                {
                    const UIRect before = reg.get<UIRect>(ctx.selectedEntity);
                    if (uiedit::ApplyUiPlacement(reg, ctx.selectedEntity, rx, ry))
                    {
                        const UIRect& after = reg.get<UIRect>(ctx.selectedEntity);
                        if (std::memcmp(&before, &after, sizeof(UIRect)) != 0)
                            ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<UIRect>>(
                                &reg, ctx.selectedEntity, before, after, "UI Placement"));
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndPopup();
    }

    // ---- 整列 / 分布（マルチ選択専用。Figma / Illustrator の整列パネル相当）----
    {
        const bool multi = ctx.selectedEntities.size() >= 2;
        ImGui::SameLine();
        ImGui::BeginDisabled(!multi);
        if (ImGui::Button("整列 \xe2\x96\xbe"))
            ImGui::OpenPopup("##UiEdAlign");
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(multi
            ? "選択した複数要素を揃える / 等間隔に並べる"
            : "Ctrl+クリックで 2 つ以上選ぶと使える");

        if (ImGui::BeginPopup("##UiEdAlign"))
        {
            // 0..5 = 左/横中央/右/上/縦中央/下、6/7 = 横/縦に等間隔分布
            static const char* kAlignLabels[8] = {
                "左揃え", "左右中央", "右揃え", "上揃え", "上下中央", "下揃え",
                "横に等間隔", "縦に等間隔"};
            for (int mode = 0; mode < 8; ++mode)
            {
                if (mode == 6) ImGui::Separator();
                if (!ImGui::MenuItem(kAlignLabels[mode])) continue;
                AlignSelection(reg, ctx, mode);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    ImGui::SameLine();
    ImGui::Checkbox("ガイド", &m_smartGuides);
    ImGui::SetItemTooltip("ドラッグ中に他の要素・画面の端/中央へ吸着する（Alt 押下で一時無効）");

    ImGui::SameLine(0.0f, 14.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("画面");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("##UiEdScreenSize", kScreenSizes[m_screenSizeIdx].label))
    {
        for (int i = 0; i < IM_ARRAYSIZE(kScreenSizes); ++i)
        {
            if (ImGui::Selectable(kScreenSizes[i].label, i == m_screenSizeIdx))
            {
                m_screenSizeIdx = i;
                m_fitRequested = true;   // サイズが変わったら見失わないよう Fit し直す
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("プレビューする画面解像度。実行時と同じスケーリング\n"
                          "（ScaleToFit のレターボックス等）で表示される");
    if (kScreenSizes[m_screenSizeIdx].w < 0)   // カスタム
    {
        ImGui::SameLine();
        int wh[2] = {m_customW, m_customH};
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::InputInt2("##UiEdCustomWH", wh))
        {
            m_customW = std::clamp(wh[0], 16, 16384);
            m_customH = std::clamp(wh[1], 16, 16384);
        }
    }

    ImGui::SameLine(0.0f, 14.0f);
    if (ImGui::Button("Fit")) m_fitRequested = true;
    ImGui::SetItemTooltip("画面全体が収まるようにズームを合わせる");
    ImGui::SameLine();
    if (ImGui::Button("100%")) { m_zoom = 1.0f; m_pan = ImVec2(0.0f, 0.0f); }
    ImGui::SetItemTooltip("等倍表示（1 キャンバス px = 1 画面 px）");
    ImGui::SameLine();
    ImGui::Checkbox("グリッド", &m_showGrid);
    ImGui::SetItemTooltip("50px（画面座標）間隔のグリッドを表示");
    ImGui::SameLine();
    ImGui::Checkbox("市松", &m_checkerBg);
    ImGui::SetItemTooltip("画面背景を市松模様に（透過 UI の視認用）");

    // ===== 左: 階層ツリー / 右: キャンバス領域（下 1 行はステータスバーに残す）=====
    const f32 statusH = ImGui::GetFrameHeightWithSpacing();

    if (ImGui::BeginChild("##UiEdTree", ImVec2(235.0f, -statusH), ImGuiChildFlags_Borders))
    {
        // 上: 階層 / 下: UI プレハブのパレット。パレットは高さを固定して階層を潰さない。
        const f32 paletteH = 152.0f;
        if (ImGui::BeginChild("##UiEdTreeInner", ImVec2(0.0f, -paletteH)))
            DrawHierarchyPane(reg, ctx);
        ImGui::EndChild();
        ImGui::Separator();
        DrawPrefabPalette(assetsDir);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= statusH;
    const ImVec2 cPos = ImGui::GetCursorScreenPos();

    if (avail.x < 64.0f || avail.y < 64.0f)
    {
        ImGui::End();
        return;
    }

    // 入力キャプチャ（左 = 選択/ドラッグ、右 = パン/メニュー、中 = パン）。
    // InvisibleButton にしておくとクリックが背後の窓や 3D ピッキングへ漏れない。
    ImGui::InvisibleButton("##UiEdCanvas", avail,
                           ImGuiButtonFlags_MouseButtonLeft
                           | ImGuiButtonFlags_MouseButtonRight
                           | ImGuiButtonFlags_MouseButtonMiddle);
    const bool canvasHovered = ImGui::IsItemHovered();

    // ---- 基準解像度（仮想スクリーンのサイズ、キャンバス空間 px）----
    f32 refW = 1920.0f, refH = 1080.0f;
    {
        const auto& preset = kScreenSizes[m_screenSizeIdx];
        if (preset.w > 0)       { refW = (f32)preset.w;  refH = (f32)preset.h; }
        else if (preset.w < 0)  { refW = (f32)m_customW; refH = (f32)m_customH; }
        else
        {
            // Canvas 基準: 最初の UICanvas の基準解像度（無ければ 1920x1080）
            auto cvView = reg.view<UICanvas>();
            if (const entt::entity first = cvView.front(); first != entt::null)
            {
                const auto& cv = cvView.get<UICanvas>(first);
                refW = (std::max)(1.0f, cv.refWidth);
                refH = (std::max)(1.0f, cv.refHeight);
            }
        }
    }

    // ---- Fit / ズーム / パン ----
    if (m_fitRequested)
    {
        constexpr f32 kFitMargin = 24.0f;
        m_zoom = (std::min)((avail.x - kFitMargin * 2.0f) / refW,
                            (avail.y - kFitMargin * 2.0f) / refH);
        m_zoom = std::clamp(m_zoom, 0.05f, 8.0f);
        m_pan = ImVec2(0.0f, 0.0f);
        m_fitRequested = false;
    }

    const ImVec2 regionCenter(cPos.x + avail.x * 0.5f, cPos.y + avail.y * 0.5f);
    const bool editDragging = (m_dragEdges != kDragNone) || (m_anchorDrag >= 0);

    // ホイール = カーソル中心ズーム（要素ドラッグ中は開始時座標が狂うので受けない）
    if (canvasHovered && !editDragging && io.MouseWheel != 0.0f)
    {
        const f32 oldZoom = m_zoom;
        m_zoom = std::clamp(m_zoom * std::pow(1.15f, io.MouseWheel), 0.05f, 8.0f);
        // カーソル位置のキャンバス上の点が動かないようパンを補正:
        //   O' = mouse - (mouse - O) * (zoom'/zoom)
        const ImVec2 oldOrigin(regionCenter.x + m_pan.x - refW * oldZoom * 0.5f,
                               regionCenter.y + m_pan.y - refH * oldZoom * 0.5f);
        const f32 ratio = m_zoom / oldZoom;
        const ImVec2 newOrigin(io.MousePos.x - (io.MousePos.x - oldOrigin.x) * ratio,
                               io.MousePos.y - (io.MousePos.y - oldOrigin.y) * ratio);
        m_pan.x = newOrigin.x - regionCenter.x + refW * m_zoom * 0.5f;
        m_pan.y = newOrigin.y - regionCenter.y + refH * m_zoom * 0.5f;
    }

    // 中 / 右ドラッグ = パン（キャンバス上で押下開始した時だけ。要素ドラッグ中は無効）
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        m_mmbDownOnCanvas = true;
    if (!io.MouseDown[ImGuiMouseButton_Middle])
        m_mmbDownOnCanvas = false;
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        m_rmbDownOnCanvas = true;
        m_rmbDownPos = io.MousePos;
    }
    if (!editDragging
        && ((m_mmbDownOnCanvas && io.MouseDown[ImGuiMouseButton_Middle])
            || (m_rmbDownOnCanvas && io.MouseDown[ImGuiMouseButton_Right])))
    {
        m_pan.x += io.MouseDelta.x;
        m_pan.y += io.MouseDelta.y;
    }

    // ---- 仮想スクリーン矩形（スクリーン座標）----
    const f32 vw = refW * m_zoom;
    const f32 vh = refH * m_zoom;
    const f32 ox = regionCenter.x + m_pan.x - vw * 0.5f;
    const f32 oy = regionCenter.y + m_pan.y - vh * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(cPos, ImVec2(cPos.x + avail.x, cPos.y + avail.y), true);

    // ---- 背景（領域全体 → 仮想スクリーン → グリッド）----
    dl->AddRectFilled(cPos, ImVec2(cPos.x + avail.x, cPos.y + avail.y), kRegionBg);
    {
        // 見えている部分だけ塗る（大きくズームインしても DrawList を膨らませない）
        const f32 sx0 = (std::max)(ox, cPos.x),           sy0 = (std::max)(oy, cPos.y);
        const f32 sx1 = (std::min)(ox + vw, cPos.x + avail.x), sy1 = (std::min)(oy + vh, cPos.y + avail.y);
        if (sx0 < sx1 && sy0 < sy1)
        {
            if (m_checkerBg)
            {
                constexpr f32 kTile = 24.0f;
                const int ix0 = (int)std::floor((sx0 - ox) / kTile);
                const int iy0 = (int)std::floor((sy0 - oy) / kTile);
                const int ix1 = (int)std::ceil((sx1 - ox) / kTile);
                const int iy1 = (int)std::ceil((sy1 - oy) / kTile);
                for (int ty = iy0; ty < iy1; ++ty)
                {
                    for (int tx = ix0; tx < ix1; ++tx)
                    {
                        const ImVec2 a((std::max)(ox + tx * kTile, sx0),       (std::max)(oy + ty * kTile, sy0));
                        const ImVec2 b((std::min)(ox + (tx + 1) * kTile, sx1), (std::min)(oy + (ty + 1) * kTile, sy1));
                        dl->AddRectFilled(a, b, ((tx + ty) & 1) ? kScreenBgB : kScreenBgA);
                    }
                }
            }
            else
            {
                dl->AddRectFilled(ImVec2(sx0, sy0), ImVec2(sx1, sy1), kScreenBgB);
            }
        }

        if (m_showGrid)
        {
            const f32 step = 50.0f * m_zoom;   // 画面座標 50px 間隔
            if (step >= 8.0f && sx0 < sx1 && sy0 < sy1)
            {
                for (f32 x = ox + std::ceil((sx0 - ox) / step) * step; x <= sx1; x += step)
                    dl->AddLine(ImVec2(x, sy0), ImVec2(x, sy1), kGridCol);
                for (f32 y = oy + std::ceil((sy0 - oy) / step) * step; y <= sy1; y += step)
                    dl->AddLine(ImVec2(sx0, y), ImVec2(sx1, y), kGridCol);
            }
        }
    }

    // ---- ゲーム内 UI のプレビュー（SceneView プレビューと同経路）----
    UISystem::RenderPreview(reg, dl, ox, oy, vw, vh, resources, srvHeap, cmdList);

    // ---- スマートガイド ----
    // 編集ブロック（下）はプレビュー描画より後に走るので、UI 要素の見た目は常に 1 フレーム遅れる。
    // ガイドも前フレームの計算結果をここで描くことで、線と要素のズレを無くしている。
    for (const auto& g : m_guides)
    {
        constexpr ImU32 kGuideCol = IM_COL32(255, 70, 160, 220);
        if (g.vertical) dl->AddLine(ImVec2(g.pos, g.a), ImVec2(g.pos, g.b), kGuideCol, 1.0f);
        else            dl->AddLine(ImVec2(g.a, g.pos), ImVec2(g.b, g.pos), kGuideCol, 1.0f);
    }

    // 仮想スクリーンの枠（プレビューの上に描いて輪郭を出す）
    dl->AddRect(ImVec2(ox - 1.0f, oy - 1.0f), ImVec2(ox + vw + 1.0f, oy + vh + 1.0f), kScreenBorder);

    // UICanvas が 1 つも無い場合は中央にヒント
    const bool anyCanvas = (reg.view<UICanvas>().front() != entt::null);
    if (!anyCanvas)
    {
        const char* msg1 = "UICanvas がありません";
        const char* msg2 = "上の「+ Canvas」または右クリックメニューから追加できます";
        const ImVec2 s1 = ImGui::CalcTextSize(msg1);
        const ImVec2 s2 = ImGui::CalcTextSize(msg2);
        dl->AddText(ImVec2(regionCenter.x - s1.x * 0.5f, regionCenter.y - s1.y - 4.0f),
                    IM_COL32(255, 255, 255, 170), msg1);
        dl->AddText(ImVec2(regionCenter.x - s2.x * 0.5f, regionCenter.y + 4.0f),
                    IM_COL32(255, 255, 255, 110), msg2);
    }

    // ===== 編集（解決済み矩形ベース。SceneView の HandleUiEditing と同じ絶対値方式）=====

    // ---- 移動 / リサイズドラッグの継続・終了（開始済みドラッグは領域外でも追従）----
    if (m_dragEdges != kDragNone)
    {
        if (!reg.valid(m_dragEntity) || !reg.all_of<UIRect>(m_dragEntity))
        {
            m_dragEdges = kDragNone;   // ドラッグ中に対象が消えた（削除等）
        }
        else if (!io.MouseDown[ImGuiMouseButton_Left])
        {
            // ドラッグ終了: 変化があれば Undo コマンドを push
            const UIRect& before = m_dragStartRect;
            const UIRect& after  = reg.get<UIRect>(m_dragEntity);
            const bool changed =
                before.offsetMin.x != after.offsetMin.x ||
                before.offsetMin.y != after.offsetMin.y ||
                before.offsetMax.x != after.offsetMax.x ||
                before.offsetMax.y != after.offsetMax.y;
            if (changed)
                ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<UIRect>>(
                    &reg, m_dragEntity, before, after, "UI Rect"));
            // マルチ選択ぶんも 1 要素 1 コマンドで積む。Ctrl+Z 1 回では 1 要素しか戻らへんが、
            // グループ用のコマンド型を足すより既存の仕組みに乗る方が壊れにくい。
            for (const auto& [other, startRect] : m_dragExtra)
            {
                if (!reg.valid(other) || !reg.all_of<UIRect>(other)) continue;
                const UIRect& a2 = reg.get<UIRect>(other);
                if (std::memcmp(&startRect, &a2, sizeof(UIRect)) == 0) continue;
                ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<UIRect>>(
                    &reg, other, startRect, a2, "UI Rect"));
            }
            m_dragExtra.clear();
            m_guides.clear();
            m_dragEdges = kDragNone;
        }
        else
        {
            // 「開始スナップショット + 総移動量」の絶対値方式（増分方式のドリフト防止）
            const f32 scale = (m_dragCanvasScale > 1e-6f) ? m_dragCanvasScale : 1.0f;
            f32 dxS = io.MousePos.x - m_dragStartMouse.x;   // スクリーン px
            f32 dyS = io.MousePos.y - m_dragStartMouse.y;

            // ---- スマートガイド（移動時のみ。Alt 押下で一時無効 = Figma と同じ操作感）----
            m_guides.clear();
            if (m_dragEdges == kDragMove && m_smartGuides && !io.KeyAlt)
            {
                // 候補: 同じキャンバスの他要素（自分と子孫は除く）＋ 仮想スクリーン自体
                std::vector<f32> candX, candY;
                ImVec2 otherMin(FLT_MAX, FLT_MAX), otherMax(-FLT_MAX, -FLT_MAX);
                for (const auto& rr : m_lastRects)
                {
                    if (rr.e == m_dragEntity || rr.hasXform) continue;
                    if (IsDescendantOf(reg, rr.e, m_dragEntity)) continue;
                    candX.push_back(rr.min.x);
                    candX.push_back((rr.min.x + rr.max.x) * 0.5f);
                    candX.push_back(rr.max.x);
                    candY.push_back(rr.min.y);
                    candY.push_back((rr.min.y + rr.max.y) * 0.5f);
                    candY.push_back(rr.max.y);
                    otherMin = ImVec2((std::min)(otherMin.x, rr.min.x), (std::min)(otherMin.y, rr.min.y));
                    otherMax = ImVec2((std::max)(otherMax.x, rr.max.x), (std::max)(otherMax.y, rr.max.y));
                }
                candX.push_back(ox); candX.push_back(ox + vw * 0.5f); candX.push_back(ox + vw);
                candY.push_back(oy); candY.push_back(oy + vh * 0.5f); candY.push_back(oy + vh);
                otherMin = ImVec2((std::min)(otherMin.x, ox), (std::min)(otherMin.y, oy));
                otherMax = ImVec2((std::max)(otherMax.x, ox + vw), (std::max)(otherMax.y, oy + vh));

                const f32 mx[3] = {m_dragStartMin.x + dxS,
                                   (m_dragStartMin.x + m_dragStartMax.x) * 0.5f + dxS,
                                   m_dragStartMax.x + dxS};
                const f32 my[3] = {m_dragStartMin.y + dyS,
                                   (m_dragStartMin.y + m_dragStartMax.y) * 0.5f + dyS,
                                   m_dragStartMax.y + dyS};

                const SnapResult sx = SnapToCandidates(mx, candX);
                const SnapResult sy = SnapToCandidates(my, candY);
                if (sx.hit)
                {
                    dxS += sx.delta;
                    m_guides.push_back({true, sx.line, otherMin.y, otherMax.y});
                }
                if (sy.hit)
                {
                    dyS += sy.delta;
                    m_guides.push_back({false, sy.line, otherMin.x, otherMax.x});
                }
            }

            const f32 dxC = dxS / scale;
            const f32 dyC = dyS / scale;

            auto& rect = reg.get<UIRect>(m_dragEntity);
            rect = m_dragStartRect;

            if (m_dragEdges == kDragMove)
            {
                rect.offsetMin.x += dxC; rect.offsetMax.x += dxC;
                rect.offsetMin.y += dyC; rect.offsetMax.y += dyC;

                // マルチ選択: 掴んだ要素と同じ移動量を他の選択要素へも適用する。
                // 吸着は掴んだ要素の矩形だけで判定する（各要素が別々に吸くと相対配置が崩れるため）。
                for (const auto& [other, startRect] : m_dragExtra)
                {
                    if (!reg.valid(other) || !reg.all_of<UIRect>(other)) continue;
                    auto& r2 = reg.get<UIRect>(other);
                    r2 = startRect;
                    r2.offsetMin.x += dxC; r2.offsetMax.x += dxC;
                    r2.offsetMin.y += dyC; r2.offsetMax.y += dyC;
                }
            }
            else
            {
                // 最小サイズガード: 幅/高さをキャンバス空間 1px 未満・反転にしない
                constexpr f32 kMinSizePx = 1.0f;
                const f32 startW = (m_dragStartMax.x - m_dragStartMin.x) / scale;
                const f32 startH = (m_dragStartMax.y - m_dragStartMin.y) / scale;
                if (m_dragEdges & kEdgeL) rect.offsetMin.x += (std::min)(dxC, startW - kMinSizePx);
                if (m_dragEdges & kEdgeR) rect.offsetMax.x += (std::max)(dxC, kMinSizePx - startW);
                if (m_dragEdges & kEdgeT) rect.offsetMin.y += (std::min)(dyC, startH - kMinSizePx);
                if (m_dragEdges & kEdgeB) rect.offsetMax.y += (std::max)(dyC, kMinSizePx - startH);
            }
        }
    }

    // ---- 解決済み UI 矩形（描画順 = 奥→手前）。ドラッグ反映後に解決 = 今フレームの位置 ----
    std::vector<UiResolvedRect> rects;
    UISystem::ResolveRects(reg, ox, oy, vw, vh, rects);
    // スマートガイドの吸着候補は「前フレームの解決結果」を使う。移動中の要素の矩形は
    // 開始矩形 + 移動量で解析的に求まるので、候補側（動かへん兄弟）は 1 フレーム古くて問題ない。
    m_lastRects = rects;

    const auto findRect = [&rects](entt::entity e) -> const UiResolvedRect* {
        if (e == entt::null) return nullptr;
        for (const auto& rr : rects)
            if (rr.e == e) return &rr;
        return nullptr;
    };

    // ---- アンカーハンドルドラッグの継続・終了 ----
    if (m_anchorDrag >= 0)
    {
        const UiResolvedRect* sr = (reg.valid(m_dragEntity) && reg.all_of<UIRect>(m_dragEntity))
            ? findRect(m_dragEntity) : nullptr;
        if (!sr)
        {
            m_anchorDrag = -1;
        }
        else if (!io.MouseDown[ImGuiMouseButton_Left])
        {
            const UIRect& before = m_anchorStartRect;
            const UIRect& after  = reg.get<UIRect>(m_dragEntity);
            if (std::memcmp(&before, &after, sizeof(UIRect)) != 0)
                ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<UIRect>>(
                    &reg, m_dragEntity, before, after, "UI Anchor"));
            m_anchorDrag = -1;
        }
        else
        {
            // マウス → キャンバス空間 px → 親矩形内の正規化アンカー値
            const f32 cs = (sr->canvasScale > 1e-6f) ? sr->canvasScale : 1.0f;
            const f32 mxC = (io.MousePos.x - sr->canvasOrigin.x) / cs;
            const f32 myC = (io.MousePos.y - sr->canvasOrigin.y) / cs;
            const f32 pw = (std::max)(1e-3f, m_anchorParentMaxC.x - m_anchorParentMinC.x);
            const f32 ph = (std::max)(1e-3f, m_anchorParentMaxC.y - m_anchorParentMinC.y);
            const f32 snapX = kAnchorSnapPx / (pw * cs);
            const f32 snapY = kAnchorSnapPx / (ph * cs);
            f32 nx = SnapAnchor(std::clamp((mxC - m_anchorParentMinC.x) / pw, 0.0f, 1.0f), snapX);
            f32 ny = SnapAnchor(std::clamp((myC - m_anchorParentMinC.y) / ph, 0.0f, 1.0f), snapY);

            auto& rect = reg.get<UIRect>(m_dragEntity);
            rect = m_anchorStartRect;

            if (m_anchorDrag == 4)
            {
                // 中央: 4 つまとめて移動（アンカーの広がりは保つ）
                const f32 spreadX = rect.anchorMax.x - rect.anchorMin.x;
                const f32 spreadY = rect.anchorMax.y - rect.anchorMin.y;
                rect.anchorMin.x = std::clamp(nx - spreadX * 0.5f, 0.0f, 1.0f - spreadX);
                rect.anchorMin.y = std::clamp(ny - spreadY * 0.5f, 0.0f, 1.0f - spreadY);
                rect.anchorMax.x = rect.anchorMin.x + spreadX;
                rect.anchorMax.y = rect.anchorMin.y + spreadY;
            }
            else
            {
                // 三角 1 つ: 対応する成分だけ動かす（min <= max を維持 = ドラッグで分割/合流）
                if (m_anchorDrag & 1) rect.anchorMax.x = (std::max)(nx, rect.anchorMin.x);
                else                  rect.anchorMin.x = (std::min)(nx, rect.anchorMax.x);
                if (m_anchorDrag & 2) rect.anchorMax.y = (std::max)(ny, rect.anchorMin.y);
                else                  rect.anchorMin.y = (std::min)(ny, rect.anchorMax.y);
            }

            // 見た目の矩形（ドラッグ開始時にキャンバス空間で捕捉）を維持するよう offset を再計算。
            // 解決式: rectMin = parentMin + parentSize * anchorMin + offsetMin
            rect.offsetMin.x = m_anchorRectMinC.x - (m_anchorParentMinC.x + pw * rect.anchorMin.x);
            rect.offsetMin.y = m_anchorRectMinC.y - (m_anchorParentMinC.y + ph * rect.anchorMin.y);
            rect.offsetMax.x = m_anchorRectMaxC.x - (m_anchorParentMinC.x + pw * rect.anchorMax.x);
            rect.offsetMax.y = m_anchorRectMaxC.y - (m_anchorParentMinC.y + ph * rect.anchorMax.y);

            // アンカーを動かした分レイアウト解決が変わるので、矩形一覧を引き直す
            UISystem::ResolveRects(reg, ox, oy, vw, vh, rects);
        }
    }

    // ---- 選択中エンティティの解決済み矩形とハンドル ----
    // 回転/スキュー中（hasXform）はリサイズ/アンカーハンドルを出さない（軸平行前提の
    // ドラッグ数学が成立しないため）。移動ドラッグ・矢印ナッジ・Inspector 数値編集は有効。
    const UiResolvedRect* selRect = findRect(ctx.selectedEntity);
    UiHandle handles[8] = {};
    if (selRect && !selRect->hasXform)
        MakeHandles(*selRect, handles);

    // アンカーハンドル位置（親矩形 + アンカー値）。親が解決できない時は非表示。
    ImVec2 parentMinS{}, parentMaxS{};
    bool   hasParentRect = false;
    ImVec2 anchorPosS[4] = {};       // 0..3 = (xMin,yMin)(xMax,yMin)(xMin,yMax)(xMax,yMax)
    ImVec2 anchorCenterS{};
    if (selRect && !selRect->hasXform
        && reg.valid(ctx.selectedEntity) && reg.all_of<UIRect>(ctx.selectedEntity))
    {
        hasParentRect = ResolveParentScreenRect(reg, rects, *selRect, parentMinS, parentMaxS);
        if (hasParentRect)
        {
            const auto& r = reg.get<UIRect>(ctx.selectedEntity);
            const f32 pw = parentMaxS.x - parentMinS.x;
            const f32 ph = parentMaxS.y - parentMinS.y;
            const f32 ax0 = parentMinS.x + pw * r.anchorMin.x;
            const f32 ax1 = parentMinS.x + pw * r.anchorMax.x;
            const f32 ay0 = parentMinS.y + ph * r.anchorMin.y;
            const f32 ay1 = parentMinS.y + ph * r.anchorMax.y;
            anchorPosS[0] = ImVec2(ax0, ay0);
            anchorPosS[1] = ImVec2(ax1, ay0);
            anchorPosS[2] = ImVec2(ax0, ay1);
            anchorPosS[3] = ImVec2(ax1, ay1);
            anchorCenterS = ImVec2((ax0 + ax1) * 0.5f, (ay0 + ay1) * 0.5f);
        }
    }

    // ---- ホバー判定（アンカー > リサイズハンドル > UI 要素）----
    int hoveredAnchor = -1;   // 0..3 = 三角、4 = 中央
    int hoveredHandle = -1;
    const UiResolvedRect* hoveredElem = nullptr;
    if (canvasHovered && !editDragging)
    {
        if (hasParentRect)
        {
            for (int i = 0; i < 4; ++i)
            {
                const f32 dx = io.MousePos.x - anchorPosS[i].x;
                const f32 dy = io.MousePos.y - anchorPosS[i].y;
                if (dx * dx + dy * dy <= kAnchorHitR * kAnchorHitR) { hoveredAnchor = i; break; }
            }
            if (hoveredAnchor < 0)
            {
                const f32 dx = io.MousePos.x - anchorCenterS.x;
                const f32 dy = io.MousePos.y - anchorCenterS.y;
                if (dx * dx + dy * dy <= 6.0f * 6.0f) hoveredAnchor = 4;
            }
        }
        if (hoveredAnchor < 0 && selRect && !selRect->hasXform)
        {
            for (int i = 0; i < 8; ++i)
            {
                if (std::fabs(io.MousePos.x - handles[i].x) <= kHandleHitHalf
                    && std::fabs(io.MousePos.y - handles[i].y) <= kHandleHitHalf)
                {
                    hoveredHandle = i;
                    break;
                }
            }
        }
        if (hoveredAnchor < 0 && hoveredHandle < 0)
        {
            // 後方走査 = 描画順の逆 = 最前面優先（回転/スキュー要素は逆写像で判定）
            for (auto it = rects.rbegin(); it != rects.rend(); ++it)
            {
                if (uiedit::ResolvedRectContains(*it, io.MousePos))
                {
                    hoveredElem = &(*it);
                    break;
                }
            }
        }
    }

    // ---- カーソル形状 ----
    if (m_anchorDrag >= 0 || hoveredAnchor >= 0)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    else if (m_dragEdges > kDragMove)
    {
        for (int i = 0; selRect && i < 8; ++i)
            if (handles[i].edges == m_dragEdges) { ImGui::SetMouseCursor(handles[i].cursor); break; }
    }
    else if (m_dragEdges == kDragMove)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    else if (hoveredHandle >= 0)
        ImGui::SetMouseCursor(handles[hoveredHandle].cursor);
    else if (hoveredElem && hoveredElem->e == ctx.selectedEntity)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

    // ---- 左クリック: アンカー > ハンドル > UI 要素 > 空クリックで選択解除 ----
    if (!editDragging && canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        if (hoveredAnchor >= 0)
        {
            const f32 cs = (selRect->canvasScale > 1e-6f) ? selRect->canvasScale : 1.0f;
            m_dragEntity      = ctx.selectedEntity;
            m_anchorStartRect = reg.get<UIRect>(ctx.selectedEntity);
            m_anchorRectMinC  = ImVec2((selRect->min.x - selRect->canvasOrigin.x) / cs,
                                       (selRect->min.y - selRect->canvasOrigin.y) / cs);
            m_anchorRectMaxC  = ImVec2((selRect->max.x - selRect->canvasOrigin.x) / cs,
                                       (selRect->max.y - selRect->canvasOrigin.y) / cs);
            m_anchorParentMinC = ImVec2((parentMinS.x - selRect->canvasOrigin.x) / cs,
                                        (parentMinS.y - selRect->canvasOrigin.y) / cs);
            m_anchorParentMaxC = ImVec2((parentMaxS.x - selRect->canvasOrigin.x) / cs,
                                        (parentMaxS.y - selRect->canvasOrigin.y) / cs);
            m_anchorDrag = hoveredAnchor;
        }
        else if (hoveredHandle >= 0)
        {
            m_dragEntity      = ctx.selectedEntity;
            m_dragStartRect   = reg.get<UIRect>(ctx.selectedEntity);
            m_dragStartMouse  = io.MousePos;
            m_dragStartMin    = selRect->min;
            m_dragStartMax    = selRect->max;
            m_dragCanvasScale = selRect->canvasScale;
            m_dragEdges       = handles[hoveredHandle].edges;
        }
        else if (hoveredElem)
        {
            // クリック = 最外殻のウィジェット（ボタン本体など）を選択、ダブルクリックで 1 段深掘り
            // （Figma / UMG 方式。ラベル(子 UIText)だけ掴んで「文字だけ動く」のを防ぐ）
            const bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            const entt::entity target =
                uiedit::ResolveClickTarget(reg, hoveredElem->e, ctx.selectedEntity, dbl);
            if (target != entt::null)
            {
                if (io.KeyCtrl)
                {
                    ctx.ToggleSelection(target);
                }
                else
                {
                    // 既に複数選択の一部を掴んだ場合は選択を壊さずグループ移動に入る
                    // （選択し直しになると「まとめて動かす」が永遠にできへん）。
                    const bool groupDrag = ctx.IsSelected(target) && ctx.selectedEntities.size() > 1;
                    if (!groupDrag) ctx.Select(target);

                    // 選択と同時に移動ドラッグ開始（Unity / UMG 同様、掴んでそのまま動かせる）
                    if (const UiResolvedRect* tr = findRect(target))
                    {
                        m_dragEntity      = target;
                        m_dragStartRect   = reg.get<UIRect>(target);
                        m_dragStartMouse  = io.MousePos;
                        m_dragStartMin    = tr->min;
                        m_dragStartMax    = tr->max;
                        m_dragCanvasScale = tr->canvasScale;
                        m_dragEdges       = kDragMove;

                        m_dragExtra.clear();
                        if (groupDrag)
                        {
                            for (entt::entity other : ctx.selectedEntities)
                            {
                                if (other == target || !reg.valid(other)) continue;
                                if (!reg.all_of<UIRect>(other)) continue;
                                // 掴んだ要素の子孫は親と一緒に動くので二重に足さない
                                if (IsDescendantOf(reg, other, target)) continue;
                                m_dragExtra.emplace_back(other, reg.get<UIRect>(other));
                            }
                        }
                    }
                }
            }
            // 選択が変わったので描画用に引き直す
            selRect = findRect(ctx.selectedEntity);
            if (selRect && !selRect->hasXform)
                MakeHandles(*selRect, handles);
            hoveredHandle = -1;
            hasParentRect = false;   // アンカーは次フレームに再計算（1 フレームだけ非表示で十分）
        }
        else if (!io.KeyCtrl)
        {
            ctx.ClearSelection();   // 空クリック = 選択解除（UMG / Unity と同じ）
            selRect = nullptr;
            hasParentRect = false;
        }
    }

    // ---- オーバーレイ描画 ----
    // ホバー中の要素: 「クリックしたら選ばれる対象」（最外殻優先）に薄いアウトライン + 名前
    if (hoveredElem && m_dragEdges == kDragNone)
    {
        const entt::entity hoverTarget =
            uiedit::ResolveClickTarget(reg, hoveredElem->e, ctx.selectedEntity, false);
        if (hoverTarget != entt::null && hoverTarget != ctx.selectedEntity)
        {
            if (const UiResolvedRect* hr = findRect(hoverTarget))
            {
                uiedit::DrawResolvedOutline(dl, *hr, kHoverCol);
                DrawNameLabel(dl, hr->min, EntityName(reg, hoverTarget), kHoverCol);
            }
        }
    }
    // マルチ選択（プライマリ以外）: 細いアウトライン
    for (entt::entity e : ctx.selectedEntities)
    {
        if (e == ctx.selectedEntity) continue;
        if (const UiResolvedRect* rr = findRect(e))
            uiedit::DrawResolvedOutline(dl, *rr, kAccentCol);
    }
    // プライマリ選択: アウトライン + 名前 + 8 ハンドル + アンカーハンドル
    // （回転/スキュー中はハンドル無し = hasParentRect/handles とも上のガードで空）
    if (selRect)
    {
        uiedit::DrawResolvedOutline(dl, *selRect, kAccentCol, 2.0f);
        DrawNameLabel(dl, selRect->min, EntityName(reg, ctx.selectedEntity), kAccentCol);

        if (hasParentRect)
        {
            // アンカー三角（花弁）。中心から外向き。ドラッグ/ホバー中は黄色。
            const f32 inv = 0.70710678f;   // 1/sqrt(2)
            for (int i = 0; i < 4; ++i)
            {
                const f32 sx = (i & 1) ? 1.0f : -1.0f;
                const f32 sy = (i & 2) ? 1.0f : -1.0f;
                const ImVec2 d(sx * inv, sy * inv);
                const ImVec2 perp(-d.y, d.x);
                const ImVec2 tip(anchorPosS[i].x + d.x * kAnchorTriLen,
                                 anchorPosS[i].y + d.y * kAnchorTriLen);
                const ImVec2 b1(anchorPosS[i].x + perp.x * kAnchorTriHalf,
                                anchorPosS[i].y + perp.y * kAnchorTriHalf);
                const ImVec2 b2(anchorPosS[i].x - perp.x * kAnchorTriHalf,
                                anchorPosS[i].y - perp.y * kAnchorTriHalf);
                const bool hot = (m_anchorDrag == i) || (m_anchorDrag < 0 && hoveredAnchor == i);
                dl->AddTriangleFilled(tip, b1, b2, hot ? kAnchorHotCol : kAnchorCol);
                dl->AddTriangle(tip, b1, b2, IM_COL32(0, 0, 0, 160));
            }
            // 中央ハンドル（4 つまとめて移動）
            const bool hotC = (m_anchorDrag == 4) || (m_anchorDrag < 0 && hoveredAnchor == 4);
            dl->AddCircleFilled(anchorCenterS, 3.5f, hotC ? kAnchorHotCol : kAnchorCol);
            dl->AddCircle(anchorCenterS, 3.5f, IM_COL32(0, 0, 0, 160));
        }

        for (int i = 0; !selRect->hasXform && i < 8; ++i)
        {
            const bool hot = (m_dragEdges > kDragMove)
                ? (handles[i].edges == m_dragEdges)
                : (i == hoveredHandle);
            const ImVec2 hmin(handles[i].x - kHandleHalf, handles[i].y - kHandleHalf);
            const ImVec2 hmax(handles[i].x + kHandleHalf, handles[i].y + kHandleHalf);
            dl->AddRectFilled(hmin, hmax, hot ? kHandleHotCol : kHandleCol);
            dl->AddRect(hmin, hmax, kAccentCol);
        }
    }

    // 操作ヒント（左下）
    dl->AddText(ImVec2(cPos.x + 8.0f, cPos.y + avail.y - ImGui::GetTextLineHeight() - 6.0f),
                IM_COL32(255, 255, 255, 90),
                "ホイール: ズーム   中/右ドラッグ: パン   クリック: 選択   矢印: 移動(Shift=10px)   Del: 削除   Ctrl+D: 複製");

    dl->PopClipRect();

    // ===== キーボード（窓フォーカス中のみ。テキスト入力中は無効）=====
    const bool selHasRect = ctx.selectedEntity != entt::null
        && reg.valid(ctx.selectedEntity) && reg.all_of<UIRect>(ctx.selectedEntity);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput)
    {
        // 矢印キーナッジ。押している間は 1 セッションとして蓄積し、離した時に 1 コマンドで Undo 登録
        if (selHasRect && m_dragEdges == kDragNone && m_anchorDrag < 0)
        {
            const f32 step = io.KeyShift ? 10.0f : 1.0f;
            f32 nx = 0.0f, ny = 0.0f;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  nx -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nx += step;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    ny -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  ny += step;
            if (nx != 0.0f || ny != 0.0f)
            {
                if (!m_nudging || m_nudgeEntity != ctx.selectedEntity)
                {
                    m_nudging       = true;
                    m_nudgeEntity   = ctx.selectedEntity;
                    m_nudgeStartRect = reg.get<UIRect>(ctx.selectedEntity);
                }
                auto& rect = reg.get<UIRect>(ctx.selectedEntity);
                rect.offsetMin.x += nx; rect.offsetMax.x += nx;
                rect.offsetMin.y += ny; rect.offsetMax.y += ny;
            }
        }

        // Del = 削除（UIRect 持ちの選択のみ。フレーム境界の共通削除経路へ）
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && selHasRect)
        {
            for (entt::entity e : ctx.selectedEntities)
                if (reg.valid(e) && reg.all_of<UIRect>(e))
                    ctx.pendingDeletions.push_back(e);
        }

        // Ctrl+D = 複製（フレーム境界の共通複製経路へ）
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && selHasRect)
        {
            for (entt::entity e : ctx.selectedEntities)
                if (reg.valid(e) && reg.all_of<UIRect>(e))
                    ctx.pendingDuplications.push_back(e);
        }
    }

    // ナッジセッション終了（矢印キーが全部離れたら Undo コマンドを 1 つ push）
    if (m_nudging
        && !ImGui::IsKeyDown(ImGuiKey_LeftArrow) && !ImGui::IsKeyDown(ImGuiKey_RightArrow)
        && !ImGui::IsKeyDown(ImGuiKey_UpArrow)   && !ImGui::IsKeyDown(ImGuiKey_DownArrow))
    {
        if (reg.valid(m_nudgeEntity) && reg.all_of<UIRect>(m_nudgeEntity))
        {
            const UIRect& after = reg.get<UIRect>(m_nudgeEntity);
            if (std::memcmp(&m_nudgeStartRect, &after, sizeof(UIRect)) != 0)
                ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<UIRect>>(
                    &reg, m_nudgeEntity, m_nudgeStartRect, after, "UI Rect"));
        }
        m_nudging = false;
        m_nudgeEntity = entt::null;
    }

    // ===== 右クリックメニュー（ドラッグでない右クリックだけ。パンと区別）=====
    if (m_rmbDownOnCanvas && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
    {
        m_rmbDownOnCanvas = false;
        const f32 dx = io.MousePos.x - m_rmbDownPos.x;
        const f32 dy = io.MousePos.y - m_rmbDownPos.y;
        if (dx * dx + dy * dy < 4.0f * 4.0f && canvasHovered)
            ImGui::OpenPopup("##UiEdContext");
    }
    if (ImGui::BeginPopup("##UiEdContext"))
    {
        ImGui::TextDisabled("UI を追加");
        ImGui::Separator();
        if (ImGui::MenuItem("Canvas"))   spawnMarker("__ui_canvas__");
        if (ImGui::MenuItem("画像 Image"))   spawnMarker("__ui_image__");
        if (ImGui::MenuItem("テキスト Text")) spawnMarker("__ui_text__");
        if (ImGui::MenuItem("ボタン Button")) spawnMarker("__ui_button__");
        if (ImGui::MenuItem("スライダー Slider")) spawnMarker("__ui_slider__");
        if (ImGui::MenuItem("トグル Toggle")) spawnMarker("__ui_toggle__");
        if (ImGui::MenuItem("スクロールビュー ScrollView")) spawnMarker("__ui_scrollview__");
        if (selHasRect)
        {
            ImGui::Separator();
            if (ImGui::MenuItem("複製 (Ctrl+D)"))
                ctx.pendingDuplications.push_back(ctx.selectedEntity);
            if (ImGui::MenuItem("削除 (Del)"))
                ctx.pendingDeletions.push_back(ctx.selectedEntity);
            ImGui::Separator();
            // 選択サブツリーを assets/prefabs/ui/ へ書き出して、その場でインスタンス化する
            // （UI パーツを何十個も並べる用途では、これが無いと同じ物を作り直す羽目になる）
            if (ImGui::MenuItem("プレハブにする"))
                ctx.pendingCreatePrefab = ctx.selectedEntity;
            if (reg.all_of<PrefabLink>(ctx.selectedEntity))
            {
                if (ImGui::MenuItem("プレハブに適用 Apply"))
                    ctx.pendingPrefabApply.push_back(ctx.selectedEntity);
                if (ImGui::MenuItem("プレハブから戻す Revert"))
                    ctx.pendingPrefabRevert.push_back(ctx.selectedEntity);
            }
        }
        ImGui::EndPopup();
    }

    // ===== アセットブラウザからのテクスチャ D&D（最前面の UIImage へ割当）=====
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
        {
            const char* droppedPath = static_cast<const char*>(payload->Data);
            namespace fs = std::filesystem;
            std::string ext = fs::path(droppedPath).extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Texture)
            {
                // ドロップ位置の最前面 UIImage を探す
                const UiResolvedRect* target = nullptr;
                for (auto it = rects.rbegin(); it != rects.rend(); ++it)
                {
                    if (io.MousePos.x >= it->min.x && io.MousePos.x < it->max.x
                        && io.MousePos.y >= it->min.y && io.MousePos.y < it->max.y
                        && reg.all_of<UIImage>(it->e))
                    {
                        target = &(*it);
                        break;
                    }
                }
                if (target)
                {
                    // 絶対パス → assets 相対（Inspector のテクスチャスロット D&D と同じ変換）
                    std::string abs  = fs::path(droppedPath).lexically_normal().string();
                    std::string base = fs::path(assetsDir).lexically_normal().string();
                    std::replace(abs.begin(),  abs.end(),  '\\', '/');
                    std::replace(base.begin(), base.end(), '\\', '/');

                    auto& img = reg.get<UIImage>(target->e);
                    const UIImage before = img;
                    img.texturePath = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
                    ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<UIImage>>(
                        &reg, target->e, before, img, "UI Texture"));
                    ctx.Select(target->e);
                }
            }
            else if (AssetBrowserPanel::ClassifyExtension(ext)
                     == AssetBrowserPanel::AssetType::Prefab)
            {
                // UI プレハブをドロップ位置へ配置する。実際の生成はフレーム境界（pendingSpawns）
                // なので、ここでは「どのキャンバスの子として、どのキャンバス座標に置くか」だけ渡す。
                m_pendingPrefabDrop = droppedPath;
                m_pendingPrefabDropPos = io.MousePos;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // ===== ドロップされた UI プレハブの配置予約 =====
    // 生成後に位置を合わせたいので、生成要求と「置きたいキャンバス座標」を一緒に積む。
    if (!m_pendingPrefabDrop.empty())
    {
        // ドロップ位置を含む最前面のキャンバスを親にする（無ければキャンバス自動生成に任せる）
        entt::entity parent = entt::null;
        f32 canvasX = 0.0f, canvasY = 0.0f;
        for (auto it = rects.rbegin(); it != rects.rend(); ++it)
        {
            if (m_pendingPrefabDropPos.x < it->min.x || m_pendingPrefabDropPos.x >= it->max.x
                || m_pendingPrefabDropPos.y < it->min.y || m_pendingPrefabDropPos.y >= it->max.y)
                continue;
            parent = it->e;
            const f32 cs = (it->canvasScale > 1e-6f) ? it->canvasScale : 1.0f;
            canvasX = (m_pendingPrefabDropPos.x - it->canvasOrigin.x) / cs;
            canvasY = (m_pendingPrefabDropPos.y - it->canvasOrigin.y) / cs;
            break;
        }
        if (parent == entt::null)
        {
            // 要素の上ではなくキャンバスの余白に落とされた場合: 最初のキャンバスへ入れる
            auto canvasView = reg.view<const UICanvas>();
            if (canvasView.begin() != canvasView.end()) parent = *canvasView.begin();
            canvasX = refW * 0.5f;
            canvasY = refH * 0.5f;
        }

        PendingSpawnRequest req;
        req.modelPath = m_pendingPrefabDrop;
        req.parent    = parent;
        req.uiCanvasPos   = {canvasX, canvasY};
        req.placeInUiCanvas = true;
        ctx.pendingSpawns.push_back(req);

        m_pendingPrefabDrop.clear();
    }

    // ===== ステータスバー =====
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("ズーム %.0f%%   画面 %.0f x %.0f", m_zoom * 100.0, refW, refH);
        if (selRect)
        {
            const f32 cs = (selRect->canvasScale > 1e-6f) ? selRect->canvasScale : 1.0f;
            const f32 rx = (selRect->min.x - selRect->canvasOrigin.x) / cs;
            const f32 ry = (selRect->min.y - selRect->canvasOrigin.y) / cs;
            const f32 rw = (selRect->max.x - selRect->min.x) / cs;
            const f32 rh = (selRect->max.y - selRect->min.y) / cs;
            ImGui::SameLine(0.0f, 20.0f);
            ImGui::Text("選択: %s", EntityName(reg, ctx.selectedEntity));
            ImGui::SameLine();
            ImGui::TextDisabled("(%.0f, %.0f)  %.0f x %.0f px", rx, ry, rw, rh);
        }
    }

    ImGui::End();
}

} // namespace dx12e
