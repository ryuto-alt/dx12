#include "ui/UISystem.h"

#include <algorithm>
#include <cfloat>
#include <unordered_map>

#include <imgui.h>

#include "core/PathResolver.h"
#include "ecs/Components.h"
#include "engine/core/EventBus.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/Texture.h"
#include "resource/ResourceManager.h"

namespace dx12e
{
namespace
{

// 解決済み矩形（min/max）。キャンバス空間（基準解像度ピクセル）とスクリーン空間の両方で使う。
struct UiRectPx
{
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    float Width()  const { return maxX - minX; }
    float Height() const { return maxY - minY; }
    bool Contains(float x, float y) const
    {
        return x >= minX && y >= minY && x < maxX && y < maxY;
    }
};

// レイキャスト対象 1 件（エンティティ + スクリーン空間の解決済み矩形）。描画順に積む。
struct UiHitEntry
{
    entt::entity e = entt::null;
    UiRectPx     rect;
};

// キャンバス→スクリーンの変換と、このフレームの入力スナップショット（走査中は不変）。
struct UiDrawContext
{
    entt::registry* reg = nullptr;
    const std::unordered_map<entt::entity, std::vector<entt::entity>>* children = nullptr;
    ImDrawList* dl = nullptr;

    // キャンバス空間 → スクリーン: screen = origin + canvasPx * scale
    float originX = 0.0f, originY = 0.0f, scale = 1.0f;

    UiRectPx viewport;             // ゲームビューポート矩形（スクリーンpx）。ホバー判定はこの内側のみ
    ImVec2 mousePos{0.0f, 0.0f};
    bool windowHovered = false;    // ##GameUI が最前面でホバーされているか（エディタパネル越しの誤反応防止）
    bool mouseDown     = false;
    bool mouseClicked  = false;
    bool mouseReleased = false;

    ResourceManager*           resources = nullptr;
    DescriptorHeap*            srvHeap   = nullptr;
    ID3D12GraphicsCommandList* cmdList   = nullptr;

    // トラバース中に集めるレイキャスト情報（描画順 = 奥→手前。入力の解決は走査完了後）
    std::vector<UiHitEntry>* buttonRects = nullptr;  // interactable な UIButton 全件
    std::vector<UiHitEntry>* blockers    = nullptr;  // クリックを遮る要素（ボタン + raycastBlock 画像）
};

// UIRect の解決式（Components.h のコメントと同一）:
//   rectMin = parentMin + parentSize*anchorMin + offsetMin
//   rectMax = parentMin + parentSize*anchorMax + offsetMax
UiRectPx ResolveUiRect(const UIRect& r, const UiRectPx& parent)
{
    UiRectPx out;
    out.minX = parent.minX + parent.Width()  * r.anchorMin.x + r.offsetMin.x;
    out.minY = parent.minY + parent.Height() * r.anchorMin.y + r.offsetMin.y;
    out.maxX = parent.minX + parent.Width()  * r.anchorMax.x + r.offsetMax.x;
    out.maxY = parent.minY + parent.Height() * r.anchorMax.y + r.offsetMax.y;
    return out;
}

UiRectPx ToScreen(const UiRectPx& c, const UiDrawContext& ctx)
{
    return {ctx.originX + c.minX * ctx.scale, ctx.originY + c.minY * ctx.scale,
            ctx.originX + c.maxX * ctx.scale, ctx.originY + c.maxY * ctx.scale};
}

ImU32 ToImCol(const DirectX::XMFLOAT4& c)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w));
}

// 9-slice: sliceBorder（左,上,右,下、元テクスチャpx）で 3x3 に分割して描く。
// 角はキャンバススケール倍の固定サイズ、辺と中央は引き伸ばし。
// 矩形が境界の合計より小さい場合は境界を等比で縮めて破綻を防ぐ。
// UV 側も境界合計が uvMin..uvMax の切り出しサブ矩形を超える場合は等比で縮めてからクランプする。
void AddImage9Slice(ImDrawList* dl, ImTextureID texId,
                    const ImVec2& pMin, const ImVec2& pMax,
                    const ImVec2& uvMin, const ImVec2& uvMax,
                    float texW, float texH,
                    const DirectX::XMFLOAT4& border, float scale, ImU32 col)
{
    // 画面側の境界幅(px)
    float dstL = border.x * scale, dstT = border.y * scale;
    float dstR = border.z * scale, dstB = border.w * scale;
    const float rw = pMax.x - pMin.x;
    const float rh = pMax.y - pMin.y;
    if (dstL + dstR > rw && dstL + dstR > 0.0f)
    {
        const float k = rw / (dstL + dstR);
        dstL *= k; dstR *= k;
    }
    if (dstT + dstB > rh && dstT + dstB > 0.0f)
    {
        const float k = rh / (dstT + dstB);
        dstT *= k; dstB *= k;
    }

    // UV 側の境界幅（境界は元テクスチャのピクセル指定）
    float duL = (texW > 0.0f) ? border.x / texW : 0.0f;
    float duR = (texW > 0.0f) ? border.z / texW : 0.0f;
    float dvT = (texH > 0.0f) ? border.y / texH : 0.0f;
    float dvB = (texH > 0.0f) ? border.w / texH : 0.0f;
    // 境界合計がサブ矩形を超える場合は dst 側と対称に等比で縮める。これにより
    // uvMin.x+duL <= uvMax.x-duR（縦も同様）が保証され、中央セルの UV 反転は起きない。
    const float uvW = uvMax.x - uvMin.x;
    const float uvH = uvMax.y - uvMin.y;
    if (duL + duR > uvW && duL + duR > 0.0f)
    {
        const float k = uvW / (duL + duR);
        duL *= k; duR *= k;
    }
    if (dvT + dvB > uvH && dvT + dvB > 0.0f)
    {
        const float k = uvH / (dvT + dvB);
        dvT *= k; dvB *= k;
    }

    const float xs[4] = {pMin.x, pMin.x + dstL, pMax.x - dstR, pMax.x};
    const float ys[4] = {pMin.y, pMin.y + dstT, pMax.y - dstB, pMax.y};
    const float us[4] = {uvMin.x, std::min(uvMin.x + duL, uvMax.x),
                         std::max(uvMax.x - duR, uvMin.x), uvMax.x};
    const float vs[4] = {uvMin.y, std::min(uvMin.y + dvT, uvMax.y),
                         std::max(uvMax.y - dvB, uvMin.y), uvMax.y};

    for (int row = 0; row < 3; ++row)
    {
        for (int c = 0; c < 3; ++c)
        {
            const ImVec2 a(xs[c], ys[row]);
            const ImVec2 b(xs[c + 1], ys[row + 1]);
            if (b.x - a.x < 0.5f || b.y - a.y < 0.5f) continue;   // 潰れたセルはスキップ
            dl->AddImage(texId, a, b,
                         ImVec2(us[c], vs[row]), ImVec2(us[c + 1], vs[row + 1]), col);
        }
    }
}

// 1 要素の描画とレイキャスト収集（入力の解決はトラバース完了後）。rect はキャンバス空間の解決済み矩形。
void DrawUiElement(entt::entity e, const UiRectPx& rect, UiDrawContext& ctx)
{
    auto& reg = *ctx.reg;
    const UiRectPx s = ToScreen(rect, ctx);

    // --- UIButton: レイキャスト収集とティント色の確定 ---
    // 入力（ホバー/押下/クリック）の解決は全キャンバスのトラバース完了後に行うため、
    // ティントは「前フレームで確定した _hovered/_pressed」を使う（今フレームの最前面が
    // まだ未確定のため。1 フレーム遅延は既存のクリック配送と同じ思想）。
    DirectX::XMFLOAT4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    auto* btn = reg.try_get<UIButton>(e);
    if (btn)
    {
        if (btn->interactable)
        {
            tint = btn->_pressed ? btn->pressedColor
                 : (btn->_hovered ? btn->hoverColor : btn->normalColor);
            if (ctx.buttonRects)
                ctx.buttonRects->push_back({e, s});
        }
        else
        {
            btn->_hovered = false;
            btn->_pressed = false;
            tint = btn->normalColor;
        }
    }

    // クリックを遮る要素 = interactable な UIButton ＋ raycastBlock=true の UIImage。
    // UIText は遮らない（ボタンのラベルが親ボタンのクリックを妨げないように）。
    if (ctx.blockers)
    {
        const auto* imgBlock = reg.try_get<UIImage>(e);
        if ((btn && btn->interactable) || (imgBlock && imgBlock->raycastBlock))
            ctx.blockers->push_back({e, s});
    }

    // --- UIImage: テクスチャ（9-slice 対応）または単色矩形。ボタンの状態色を乗算 ---
    if (const auto* img = reg.try_get<UIImage>(e))
    {
        const DirectX::XMFLOAT4 col{img->color.x * tint.x, img->color.y * tint.y,
                                    img->color.z * tint.z, img->color.w * tint.w};
        const ImU32 ucol = ToImCol(col);
        const ImVec2 pMin(s.minX, s.minY);
        const ImVec2 pMax(s.maxX, s.maxY);
        bool drawn = false;
        if (!img->texturePath.empty() && ctx.resources && ctx.srvHeap && ctx.cmdList)
        {
            // エディタアイコンと同じ経路: SRV index → GPU ハンドル(u64) = ImTextureID。
            // GetOrLoadTexture はパスキーでキャッシュされるため毎フレーム呼んでも安価。
            const std::string abs = PathResolver::AssetsDir() + img->texturePath;
            Texture* tex = ctx.resources->GetOrLoadTexture(
                PathResolver::Utf8ToWide(abs), ctx.cmdList, true);
            if (tex)
            {
                const ImTextureID texId = static_cast<ImTextureID>(
                    ctx.srvHeap->GetGpuHandle(tex->GetSrvIndex()).ptr);
                const ImVec2 uv0(img->uvMin.x, img->uvMin.y);
                const ImVec2 uv1(img->uvMax.x, img->uvMax.y);
                const bool nineSlice = img->sliceBorder.x > 0.0f || img->sliceBorder.y > 0.0f
                                    || img->sliceBorder.z > 0.0f || img->sliceBorder.w > 0.0f;
                if (nineSlice)
                    AddImage9Slice(ctx.dl, texId, pMin, pMax, uv0, uv1,
                                   static_cast<float>(tex->GetWidth()),
                                   static_cast<float>(tex->GetHeight()),
                                   img->sliceBorder, ctx.scale, ucol);
                else
                    ctx.dl->AddImage(texId, pMin, pMax, uv0, uv1, ucol);
                drawn = true;
            }
        }
        // texturePath 空 or ロード失敗 → 単色矩形（角丸対応）。失敗時も要素が見えるようにする
        if (!drawn)
            ctx.dl->AddRectFilled(pMin, pMax, ucol, img->cornerRadius * ctx.scale);
    }

    // --- UIText: 整列 + 折り返し（ImGui 共有フォントのスケール描画。ボタンティントは掛けない）---
    if (const auto* txt = reg.try_get<UIText>(e))
    {
        if (!txt->text.empty() && txt->color.w > 0.0f)
        {
            ImFont* font = ImGui::GetFont();
            const float fontSize = std::max(1.0f, txt->fontSize * ctx.scale);
            const float wrapW = txt->wrap ? std::max(1.0f, s.Width()) : 0.0f;
            const ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, wrapW, txt->text.c_str());
            // ブロック単位の整列（CalcTextSizeA は折り返し後の全体サイズを返す）
            float tx = s.minX;
            if (txt->alignH == 1)      tx = s.minX + (s.Width() - ts.x) * 0.5f;
            else if (txt->alignH == 2) tx = s.maxX - ts.x;
            float ty = s.minY;
            if (txt->alignV == 1)      ty = s.minY + (s.Height() - ts.y) * 0.5f;
            else if (txt->alignV == 2) ty = s.maxY - ts.y;
            ctx.dl->AddText(font, fontSize, ImVec2(tx, ty), ToImCol(txt->color),
                            txt->text.c_str(), nullptr, wrapW);
        }
    }
}

// 親→子の DFS。UIRect を持つノードは矩形を解決して描画し、持たないノードは親矩形を素通しする。
// visible=false の UIRect はサブツリーごとスキップ。兄弟順は children 構築時の走査順
// （HierarchyPanel と同じ view<Transform> の順序）。
void DrawUiSubtree(entt::entity e, const UiRectPx& parentRect, UiDrawContext& ctx)
{
    UiRectPx current = parentRect;
    if (const auto* rect = ctx.reg->try_get<UIRect>(e))
    {
        if (!rect->visible) return;
        current = ResolveUiRect(*rect, parentRect);
        DrawUiElement(e, current, ctx);
    }
    auto it = ctx.children->find(e);
    if (it == ctx.children->end()) return;
    for (entt::entity child : it->second)
        DrawUiSubtree(child, current, ctx);
}

} // namespace

void UISystem::RenderAndUpdateInput(entt::registry& reg, ImDrawList* dl,
                                    float ox, float oy, float vw, float vh,
                                    ResourceManager* resources, DescriptorHeap* srvHeap,
                                    ID3D12GraphicsCommandList* cmdList)
{
    if (!dl || vw <= 0.0f || vh <= 0.0f)
        return;

    // キャンバス収集（sortOrder 昇順、同値は走査順を維持）
    struct CanvasEntry
    {
        entt::entity    e;
        const UICanvas* canvas;
    };
    std::vector<CanvasEntry> canvases;
    for (auto [e, canvas] : reg.view<const UICanvas>().each())
    {
        if (canvas.visible)
            canvases.push_back({e, &canvas});
    }
    if (canvases.empty())
        return;
    std::stable_sort(canvases.begin(), canvases.end(),
                     [](const CanvasEntry& a, const CanvasEntry& b)
                     { return a.canvas->sortOrder < b.canvas->sortOrder; });

    // 子リストを 1 パスで構築（HierarchyPanel と同じ view<Transform> 走査順 = 兄弟順）
    std::unordered_map<entt::entity, std::vector<entt::entity>> children;
    for (auto [e, t] : reg.view<const Transform>().each())
    {
        if (t.parent != entt::null && reg.valid(t.parent))
            children[t.parent].push_back(e);
    }

    // このフレームの入力スナップショット（##GameUI ウィンドウ内で呼ばれる前提）
    std::vector<UiHitEntry> buttonRects;   // interactable な UIButton（描画順）
    std::vector<UiHitEntry> blockers;      // クリックを遮る要素（描画順。後ろほど手前）
    UiDrawContext ctx;
    ctx.reg           = &reg;
    ctx.children      = &children;
    ctx.dl            = dl;
    ctx.viewport      = {ox, oy, ox + vw, oy + vh};
    ctx.mousePos      = ImGui::GetIO().MousePos;
    ctx.windowHovered = ImGui::IsWindowHovered();
    ctx.mouseDown     = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    ctx.mouseClicked  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    ctx.mouseReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    ctx.resources     = resources;
    ctx.srvHeap       = srvHeap;
    ctx.cmdList       = cmdList;
    ctx.buttonRects   = &buttonRects;
    ctx.blockers      = &blockers;

    for (const CanvasEntry& entry : canvases)
    {
        // キャンバス矩形と変換を確定（レイアウトはキャンバス空間で解決し、描画時に変換）
        UiRectPx canvasRect;
        if (entry.canvas->scaleMode == 0)
        {
            // ScaleToFit: 基準解像度を等比でビューポートへ収め、中央寄せ（レターボックス）
            const float refW = std::max(1.0f, entry.canvas->refWidth);
            const float refH = std::max(1.0f, entry.canvas->refHeight);
            ctx.scale   = std::min(vw / refW, vh / refH);
            ctx.originX = ox + (vw - refW * ctx.scale) * 0.5f;
            ctx.originY = oy + (vh - refH * ctx.scale) * 0.5f;
            canvasRect  = {0.0f, 0.0f, refW, refH};
        }
        else
        {
            // ConstantPixel: 左上原点・実ピクセル等倍
            ctx.scale   = 1.0f;
            ctx.originX = ox;
            ctx.originY = oy;
            canvasRect  = {0.0f, 0.0f, vw, vh};
        }

        auto it = children.find(entry.e);
        if (it == children.end()) continue;
        for (entt::entity child : it->second)
            DrawUiSubtree(child, canvasRect, ctx);
    }

    // --- 入力解決（Unity uGUI のレイキャスト方式。全キャンバスの矩形が確定してから）---
    // 最前面（描画で最後）のレイキャスト対象だけがクリック/ホバーを受け、重なった奥の
    // ボタンが同一クリックで同時に発火しないようにする。
    const bool mouseValid = ctx.windowHovered
                         && ctx.viewport.Contains(ctx.mousePos.x, ctx.mousePos.y);

    // 最前面ブロッカー: 後に描いたものが手前なので後ろから走査して最初のヒットを取る
    entt::entity topmost = entt::null;
    if (mouseValid)
    {
        for (auto it = blockers.rbegin(); it != blockers.rend(); ++it)
        {
            if (it->rect.Contains(ctx.mousePos.x, ctx.mousePos.y))
            {
                topmost = it->e;
                break;
            }
        }
    }

    // topmost 自身が interactable な UIButton ならそれ。無ければ Transform::parent を
    // 遡って最初のボタンへバブリング（ボタン内の子アイコン画像がクリックを吸っても親が
    // 反応する）。どこにも無ければクリックは吸収されただけ（どのボタンも反応しない）。
    entt::entity effectiveButton = entt::null;
    entt::entity walk = topmost;
    for (int depth = 0; depth < 64 && walk != entt::null && reg.valid(walk); ++depth)
    {
        if (const auto* b = reg.try_get<UIButton>(walk); b && b->interactable)
        {
            effectiveButton = walk;
            break;
        }
        const auto* t = reg.try_get<Transform>(walk);
        walk = t ? t->parent : entt::null;
    }

    // 各ボタンの状態更新（描画ティントは前フレーム状態で済ませてある）
    for (const UiHitEntry& hit : buttonRects)
    {
        auto* btn = reg.try_get<UIButton>(hit.e);
        if (!btn)
            continue;
        const bool inside = mouseValid && hit.rect.Contains(ctx.mousePos.x, ctx.mousePos.y);
        if (!btn->_pressed)
        {
            // 非押下中: 最前面判定に勝った 1 個だけがホバーし、押下開始できる
            btn->_hovered = (hit.e == effectiveButton);
            if (btn->_hovered && ctx.mouseClicked)
                btn->_pressed = true;
        }
        else
        {
            // 押下中（ポインタキャプチャ相当）: occlusion に関係なく押下を維持し、
            // release-inside でクリック確定（矩形の外で離したらキャンセル）
            btn->_hovered = inside;
            if (ctx.mouseReleased)
            {
                if (inside && !btn->onClickEvent.empty())
                    m_pendingClicks.push_back({btn->onClickEvent, hit.e});
                btn->_pressed = false;
            }
            else if (!ctx.mouseDown)
            {
                btn->_pressed = false;   // フォーカス喪失等で release を取り逃した場合の安全網
            }
        }
    }
}

void UISystem::DispatchPendingClicks(entt::registry& reg, EventBus& bus)
{
    if (m_pendingClicks.empty())
        return;
    // ハンドラ内でクリックが再度積まれても安全なように先に取り出す
    std::vector<UIPendingClick> clicks;
    clicks.swap(m_pendingClicks);
    for (const UIPendingClick& c : clicks)
    {
        EngineEvent ev;
        ev.name = c.eventName;
        if (c.source != entt::null && reg.valid(c.source))
            ev.source = c.source;
        bus.Emit(ev);   // 即時発火（呼び出し元が Lua OnUpdate より前のタイミングを保証する）
    }
}

void UISystem::ResetRuntimeState(entt::registry& reg)
{
    m_pendingClicks.clear();
    for (auto [e, btn] : reg.view<UIButton>().each())
    {
        btn._hovered = false;
        btn._pressed = false;
    }
}

} // namespace dx12e
