#include "ui/UISystem.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
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
    std::vector<UiHitEntry>* sliderRects = nullptr;  // interactable な UISlider 全件
    std::vector<UiHitEntry>* toggleRects = nullptr;  // interactable な UIToggle 全件
    std::vector<UiHitEntry>* scrollRects = nullptr;  // UIScrollView のビューポート（ホイール配送用）
    std::vector<UiHitEntry>* blockers    = nullptr;  // クリックを遮る要素（ウィジェット + raycastBlock 画像）

    // UIScrollView のクリップ（スクリーン空間）。サブツリー内で収集するレイキャスト矩形は
    // これと交差してから積む＝スクロールではみ出た要素はクリックも効かない。ネストは交差。
    // DrawUiSubtree が push/pop 式に付け替える。
    const UiRectPx* hitClip = nullptr;

    // UIScrollView のコンテンツ計測先（キャンバス空間の合併矩形）。サブツリー内で UIRect を
    // 解決するたびに合併する。ネストは内側の計測のみに入る（push/pop 式）。
    UiRectPx* contentBounds = nullptr;

    // false = エディタプレビュー（RenderPreview / ResolveRects）: ボタンは normalColor
    // 固定で描き、_hovered/_pressed には読み書きとも一切触れない（ゲーム内状態を汚さない）。
    // UIAnimator / tween の視覚エフェクトも interactive 時のみ適用する（プレビューは最終ポーズ）。
    bool interactive = true;

    // 祖先から継承するアルファ乗数（UIAnimator / tween のフェードが子孫にまとめて掛かる）。
    // DrawUiSubtree が push/pop 式に更新する。
    float alphaMul = 1.0f;

    // エディタ支援（ResolveRects）: UIRect を持つ全要素の解決済みスクリーン矩形の出力先。
    // canvasEntity は現在トラバース中の UICanvas（キャンバス毎のループで更新）。
    std::vector<UiResolvedRect>* resolvedOut = nullptr;
    entt::entity canvasEntity = entt::null;
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

// レイキャスト矩形を ctx.hitClip（スクロールビューのクリップ）と交差してから out へ積む。
// 完全にはみ出ていれば積まない（クリップ外の要素はクリックも受けない）。
void PushHitRect(std::vector<UiHitEntry>* out, entt::entity e, const UiRectPx& s,
                 const UiRectPx* clip)
{
    if (!out) return;
    UiRectPx r = s;
    if (clip)
    {
        r.minX = std::max(r.minX, clip->minX);
        r.minY = std::max(r.minY, clip->minY);
        r.maxX = std::min(r.maxX, clip->maxX);
        r.maxY = std::min(r.maxY, clip->maxY);
        if (r.minX >= r.maxX || r.minY >= r.maxY) return;
    }
    out->push_back({e, r});
}

// イージング（p: 0..1 → 0..1）。列挙は UIAnimator::showEasing / UiTween::easing 共通:
//   0=リニア 1=イーズイン 2=イーズアウト 3=イン/アウト 4=バック(勢い) 5=バウンス 6=弾性
float UiEase(int type, float p)
{
    p = std::clamp(p, 0.0f, 1.0f);
    switch (type)
    {
    case 1:   // easeInCubic
        return p * p * p;
    case 2:   // easeOutCubic
    {
        const float q = 1.0f - p;
        return 1.0f - q * q * q;
    }
    case 3:   // easeInOutCubic
        return (p < 0.5f) ? 4.0f * p * p * p
                          : 1.0f - std::pow(-2.0f * p + 2.0f, 3.0f) * 0.5f;
    case 4:   // easeOutBack（少し行き過ぎて戻る）
    {
        constexpr float c1 = 1.70158f;
        constexpr float c3 = c1 + 1.0f;
        const float q = p - 1.0f;
        return 1.0f + c3 * q * q * q + c1 * q * q;
    }
    case 5:   // easeOutBounce
    {
        constexpr float n1 = 7.5625f, d1 = 2.75f;
        if (p < 1.0f / d1)        return n1 * p * p;
        if (p < 2.0f / d1)        { p -= 1.5f / d1;   return n1 * p * p + 0.75f; }
        if (p < 2.5f / d1)        { p -= 2.25f / d1;  return n1 * p * p + 0.9375f; }
        p -= 2.625f / d1;         return n1 * p * p + 0.984375f;
    }
    case 6:   // easeOutElastic（ビヨンと弾む）
    {
        if (p <= 0.0f) return 0.0f;
        if (p >= 1.0f) return 1.0f;
        constexpr float c4 = 6.2831853f / 3.0f;
        return std::pow(2.0f, -10.0f * p) * std::sin((p * 10.0f - 0.75f) * c4) + 1.0f;
    }
    default:  // 0: リニア
        return p;
    }
}

// UIAnimator / UITweenState を dt で進め、今フレームの視覚合成結果（_cur*）を確定する。
// Play / ゲームモード中のみ呼ばれる（RenderAndUpdateInput の冒頭）。
void UpdateUiAnimations(entt::registry& reg, float dt)
{
    dt = std::clamp(dt, 0.0f, 0.1f);   // ヒッチ時の飛びを抑える

    // ---- UIAnimator（出現 / ホバー・押下 / ループ）----
    for (auto [e, an] : reg.view<UIAnimator>().each())
    {
        // ホバー / 押下スケール（UIButton の前フレーム状態へ指数平滑で追従）
        float hoverTarget = 1.0f;
        if (const auto* btn = reg.try_get<UIButton>(e))
        {
            if (btn->interactable)
                hoverTarget = btn->_pressed ? an.pressScale
                            : (btn->_hovered ? an.hoverScale : 1.0f);
        }
        const float k = std::min(1.0f, dt * (std::max)(0.0f, an.hoverSpeed));
        an._hoverS += (hoverTarget - an._hoverS) * k;

        // 出現 / 消滅の状態機械
        float alpha = 1.0f, scale = 1.0f;
        DirectX::XMFLOAT2 off{0.0f, 0.0f};
        if (an._mode == 0)   // 未開始 → Play 開始（または showUi）で自動スタート
        {
            an._t = 0.0f;
            an._mode = (an.showAnim == 0) ? 2 : 1;
        }
        if (an._mode == 1 || an._mode == 3)
        {
            an._t += dt;
            const float dur = (std::max)(0.01f, an.showDuration);
            float p;
            if (an._mode == 1)   // showing（showDelay の間は 0 で待つ）
            {
                p = (an._t - an.showDelay) / dur;
                if (p >= 1.0f) an._mode = 2;
                p = std::clamp(p, 0.0f, 1.0f);
            }
            else                 // hiding = show の逆再生（delay は使わない）
            {
                p = an._t / dur;
                if (p >= 1.0f) an._mode = 4;
                p = 1.0f - std::clamp(p, 0.0f, 1.0f);
            }
            const float ev = UiEase(an.showEasing, p);
            switch (an.showAnim)
            {
            case 2:  alpha = ev; scale = 0.5f + 0.5f * ev;                  break;  // ポップ
            case 3:  alpha = ev; off.x = -an.slideOffset * (1.0f - ev);     break;  // 左から
            case 4:  alpha = ev; off.x =  an.slideOffset * (1.0f - ev);     break;  // 右から
            case 5:  alpha = ev; off.y = -an.slideOffset * (1.0f - ev);     break;  // 上から
            case 6:  alpha = ev; off.y =  an.slideOffset * (1.0f - ev);     break;  // 下から
            default: alpha = ev;                                            break;  // フェード
            }
        }
        else if (an._mode == 4)
        {
            alpha = 0.0f;   // 非表示（描画は DrawUiSubtree がサブツリーごとスキップ）
        }

        // ループ（表示中もアニメ中も常時加算）
        an._loopT += dt;
        if (an.loopAnim != 0)
        {
            const float s = std::sin(an._loopT * an.loopSpeed * 6.2831853f);
            if (an.loopAnim == 1)      off.y += s * an.loopAmount;                        // 浮遊
            else if (an.loopAnim == 2) scale *= 1.0f + s * an.loopAmount;                 // パルス
            else if (an.loopAnim == 3)                                                    // 点滅
                alpha *= 1.0f - (0.5f + 0.5f * s) * std::clamp(an.loopAmount, 0.0f, 1.0f);
        }

        an._curScale  = (std::max)(0.0f, scale * an._hoverS);
        an._curAlpha  = std::clamp(alpha, 0.0f, 1.0f);
        an._curOffset = off;
    }

    // ---- tween（scene:tweenUi。移動は UIRect offset を直接進め、拡縮/アルファは視覚のみ）----
    for (auto [e, tw] : reg.view<UITweenState>().each())
    {
        float scale = tw.visScale, alpha = tw.visAlpha;
        for (auto it = tw.tweens.begin(); it != tw.tweens.end();)
        {
            UiTween& t = *it;
            t.t += dt;
            if (t.t < t.delay) { ++it; continue; }
            if (!t.started)
            {
                // delay 明けに from 値を捕捉（作成時でなく開始時 = 直前の状態から滑らかに繋がる）
                t.started = true;
                if (t.hasMove)
                {
                    if (const auto* r = reg.try_get<UIRect>(e))
                    {
                        t.baseOffMin = r->offsetMin;
                        t.baseOffMax = r->offsetMax;
                    }
                    else
                    {
                        t.hasMove = false;
                    }
                }
                t.scaleFrom = tw.visScale;
                t.alphaFrom = tw.visAlpha;
            }
            const float p  = std::clamp((t.t - t.delay) / (std::max)(0.01f, t.duration), 0.0f, 1.0f);
            const float ev = UiEase(t.easing, p);
            if (t.hasMove)
            {
                if (auto* r = reg.try_get<UIRect>(e))
                {
                    r->offsetMin = {t.baseOffMin.x + t.moveDelta.x * ev,
                                    t.baseOffMin.y + t.moveDelta.y * ev};
                    r->offsetMax = {t.baseOffMax.x + t.moveDelta.x * ev,
                                    t.baseOffMax.y + t.moveDelta.y * ev};
                }
            }
            if (t.hasScale) scale = t.scaleFrom + (t.scaleTo - t.scaleFrom) * ev;
            if (t.hasAlpha) alpha = t.alphaFrom + (t.alphaTo - t.alphaFrom) * ev;
            if (p >= 1.0f)
            {
                // 完了: 視覚値は持続させる（alpha=0 で消したままにできる）
                if (t.hasScale) tw.visScale = t.scaleTo;
                if (t.hasAlpha) tw.visAlpha = t.alphaTo;
                it = tw.tweens.erase(it);
            }
            else
            {
                ++it;
            }
        }
        tw._curScale = (std::max)(0.0f, scale);
        tw._curAlpha = std::clamp(alpha, 0.0f, 1.0f);
    }
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
        if (!ctx.interactive)
        {
            // エディタプレビュー: 入力状態(_hovered/_pressed)には触れず normalColor 固定
            tint = btn->normalColor;
        }
        else if (btn->interactable)
        {
            tint = btn->_pressed ? btn->pressedColor
                 : (btn->_hovered ? btn->hoverColor : btn->normalColor);
            PushHitRect(ctx.buttonRects, e, s, ctx.hitClip);
        }
        else
        {
            btn->_hovered = false;
            btn->_pressed = false;
            tint = btn->normalColor;
        }
    }

    // --- UISlider / UIToggle: レイキャスト収集（入力解決はトラバース完了後。ボタンと同じ）---
    auto* sld = reg.try_get<UISlider>(e);
    auto* tgl = reg.try_get<UIToggle>(e);
    if (sld && ctx.interactive)
    {
        if (sld->interactable) PushHitRect(ctx.sliderRects, e, s, ctx.hitClip);
        else                   { sld->_hovered = false; sld->_dragging = false; }
    }
    if (tgl && ctx.interactive)
    {
        if (tgl->interactable) PushHitRect(ctx.toggleRects, e, s, ctx.hitClip);
        else                   { tgl->_hovered = false; tgl->_pressed = false; }
    }

    // クリックを遮る要素 = interactable なウィジェット（Button/Slider/Toggle）＋
    // raycastBlock=true の UIImage。UIText は遮らない（ラベルが親のクリックを妨げないように）。
    if (ctx.blockers)
    {
        const auto* imgBlock = reg.try_get<UIImage>(e);
        if ((btn && btn->interactable) || (sld && sld->interactable)
            || (tgl && tgl->interactable) || (imgBlock && imgBlock->raycastBlock))
            PushHitRect(ctx.blockers, e, s, ctx.hitClip);
    }

    // --- UIImage: テクスチャ（9-slice 対応）または単色矩形。ボタンの状態色を乗算 ---
    if (const auto* img = reg.try_get<UIImage>(e))
    {
        // fillAmount: 表示割合 0..1（HPバー/ゲージ用）。クリップ矩形方式なのでテクスチャ /
        // 9-slice / 角丸すべてで同じ「端から現れる」挙動になる。0 は完全非表示
        // （レイキャストは上の blockers 収集どおり効いたまま）。
        const float fill = std::clamp(img->fillAmount, 0.0f, 1.0f);
        if (fill > 0.0f)
        {
            const bool fillPartial = fill < 1.0f;
            if (fillPartial)
            {
                ImVec2 cMin(s.minX, s.minY);
                ImVec2 cMax(s.maxX, s.maxY);
                switch (img->fillDir)
                {
                case 1:  cMin.x = s.maxX - s.Width() * fill;  break;   // 右から
                case 2:  cMin.y = s.maxY - s.Height() * fill; break;   // 下から
                case 3:  cMax.y = s.minY + s.Height() * fill; break;   // 上から
                default: cMax.x = s.minX + s.Width() * fill;  break;   // 0: 左から
                }
                ctx.dl->PushClipRect(cMin, cMax, true);
            }

            const DirectX::XMFLOAT4 col{img->color.x * tint.x, img->color.y * tint.y,
                                        img->color.z * tint.z,
                                        img->color.w * tint.w * ctx.alphaMul};
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

            if (fillPartial)
                ctx.dl->PopClipRect();
        }
    }

    // --- UISlider: トラック(角丸バー) + 塗り + つまみ(円) を自前描画 ---
    if (sld)
    {
        const float range = (sld->maxValue > sld->minValue) ? (sld->maxValue - sld->minValue) : 1.0f;
        const float t = std::clamp((sld->value - sld->minValue) / range, 0.0f, 1.0f);
        const float trackH = std::max(2.0f, s.Height() * 0.35f);
        const float cy = (s.minY + s.maxY) * 0.5f;
        // つまみがはみ出さないよう、トラックの左右をつまみ半径ぶん内側へ
        const float knobR = std::max(3.0f, s.Height() * 0.5f);
        const float x0 = s.minX + knobR, x1 = s.maxX - knobR;
        const float knobX = x0 + (x1 - x0) * t;
        const bool hot = ctx.interactive && (sld->_hovered || sld->_dragging);

        auto col = [&](const DirectX::XMFLOAT4& c, float mul) {
            return ToImCol({c.x * mul, c.y * mul, c.z * mul, c.w * ctx.alphaMul});
        };
        ctx.dl->AddRectFilled(ImVec2(x0 - trackH * 0.5f, cy - trackH * 0.5f),
                              ImVec2(x1 + trackH * 0.5f, cy + trackH * 0.5f),
                              col(sld->trackColor, 1.0f), trackH * 0.5f);
        if (t > 0.0f)
            ctx.dl->AddRectFilled(ImVec2(x0 - trackH * 0.5f, cy - trackH * 0.5f),
                                  ImVec2(knobX, cy + trackH * 0.5f),
                                  col(sld->fillColor, 1.0f), trackH * 0.5f);
        ctx.dl->AddCircleFilled(ImVec2(knobX, cy), knobR * (hot ? 1.08f : 1.0f),
                                col(sld->knobColor, hot ? 1.0f : 0.92f));
        ctx.dl->AddCircle(ImVec2(knobX, cy), knobR * (hot ? 1.08f : 1.0f),
                          ToImCol({0.0f, 0.0f, 0.0f, 0.35f * ctx.alphaMul}));
    }

    // --- UIToggle: 角丸の箱 + isOn なら内側に塗り ---
    if (tgl)
    {
        const bool hot = ctx.interactive && (tgl->_hovered || tgl->_pressed);
        const float round = std::min(s.Width(), s.Height()) * 0.22f;
        const float mul = tgl->_pressed ? 0.8f : (hot ? 1.25f : 1.0f);
        ctx.dl->AddRectFilled(ImVec2(s.minX, s.minY), ImVec2(s.maxX, s.maxY),
                              ToImCol({tgl->boxColor.x * mul, tgl->boxColor.y * mul,
                                       tgl->boxColor.z * mul, tgl->boxColor.w * ctx.alphaMul}),
                              round);
        if (tgl->isOn)
        {
            const float inset = std::min(s.Width(), s.Height()) * 0.22f;
            ctx.dl->AddRectFilled(ImVec2(s.minX + inset, s.minY + inset),
                                  ImVec2(s.maxX - inset, s.maxY - inset),
                                  ToImCol({tgl->checkColor.x, tgl->checkColor.y,
                                           tgl->checkColor.z, tgl->checkColor.w * ctx.alphaMul}),
                                  round * 0.6f);
        }
    }

    // --- UIText: 整列 + 折り返し（ImGui 共有フォントのスケール描画。ボタンティントは掛けない）---
    if (const auto* txt = reg.try_get<UIText>(e))
    {
        if (!txt->text.empty() && txt->color.w * ctx.alphaMul > 0.0f)
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
            const DirectX::XMFLOAT4 tcol{txt->color.x, txt->color.y, txt->color.z,
                                         txt->color.w * ctx.alphaMul};
            ctx.dl->AddText(font, fontSize, ImVec2(tx, ty), ToImCol(tcol),
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
    const float parentAlpha = ctx.alphaMul;
    if (const auto* rect = ctx.reg->try_get<UIRect>(e))
    {
        if (!rect->visible) return;
        current = ResolveUiRect(*rect, parentRect);

        // --- UIAnimator / tween の視覚エフェクト（Play / ゲームモード中のみ）---
        // 矩形をキャンバス空間で変形するので、子孫のレイアウト解決・レイキャスト矩形にも
        // そのまま効く（ボタンごと動く/縮む）。アルファは ctx.alphaMul で子孫へ継承。
        if (ctx.interactive)
        {
            float fxScale = 1.0f, fxAlpha = 1.0f, fxOffX = 0.0f, fxOffY = 0.0f;
            if (const auto* an = ctx.reg->try_get<UIAnimator>(e))
            {
                if (an->_mode == 4)
                {
                    ctx.alphaMul = parentAlpha;
                    return;   // hideUi 完了 = サブツリーごと非表示（クリックも通らない）
                }
                fxScale *= an->_curScale;
                fxAlpha *= an->_curAlpha;
                fxOffX  += an->_curOffset.x;
                fxOffY  += an->_curOffset.y;
            }
            if (const auto* tw = ctx.reg->try_get<UITweenState>(e))
            {
                fxScale *= tw->_curScale;
                fxAlpha *= tw->_curAlpha;
            }
            if (fxScale != 1.0f || fxOffX != 0.0f || fxOffY != 0.0f)
            {
                const float cx = (current.minX + current.maxX) * 0.5f + fxOffX;
                const float cy = (current.minY + current.maxY) * 0.5f + fxOffY;
                const float hw = current.Width()  * 0.5f * fxScale;
                const float hh = current.Height() * 0.5f * fxScale;
                current = {cx - hw, cy - hh, cx + hw, cy + hh};
            }
            ctx.alphaMul = parentAlpha * fxAlpha;
        }

        if (ctx.resolvedOut)
        {
            // エディタ支援（ResolveRects）: 描画順のままスクリーン矩形を収集
            const UiRectPx s = ToScreen(current, ctx);
            ctx.resolvedOut->push_back({e, ImVec2(s.minX, s.minY), ImVec2(s.maxX, s.maxY),
                                        ctx.scale, ImVec2(ctx.originX, ctx.originY),
                                        ctx.canvasEntity});
        }
        // スクロールビューのコンテンツ計測（キャンバス空間。自分の矩形を親側の計測へ合併）
        if (ctx.contentBounds)
        {
            ctx.contentBounds->minX = std::min(ctx.contentBounds->minX, current.minX);
            ctx.contentBounds->minY = std::min(ctx.contentBounds->minY, current.minY);
            ctx.contentBounds->maxX = std::max(ctx.contentBounds->maxX, current.maxX);
            ctx.contentBounds->maxY = std::max(ctx.contentBounds->maxY, current.maxY);
        }
        if (ctx.dl)
            DrawUiElement(e, current, ctx);
    }

    // --- UIScrollView: 子をスクロール平行移動 + クリップして描き、コンテンツを計測 ---
    auto* sv = ctx.reg->try_get<UIScrollView>(e);
    if (sv && ctx.reg->all_of<UIRect>(e))
    {
        // クリップ（スクリーン空間）。ネストは親クリップと交差
        UiRectPx clip = ToScreen(current, ctx);
        if (ctx.hitClip)
        {
            clip.minX = std::max(clip.minX, ctx.hitClip->minX);
            clip.minY = std::max(clip.minY, ctx.hitClip->minY);
            clip.maxX = std::min(clip.maxX, ctx.hitClip->maxX);
            clip.maxY = std::min(clip.maxY, ctx.hitClip->maxY);
        }

        // スクロールを 0..(コンテンツ−ビュー) にクランプ（前フレームの計測値を使う）
        sv->scrollX = std::clamp(sv->scrollX, 0.0f,
                                 std::max(0.0f, sv->_contentW - current.Width()));
        sv->scrollY = std::clamp(sv->scrollY, 0.0f,
                                 std::max(0.0f, sv->_contentH - current.Height()));

        // 子には「スクロール分だけ上/左へ平行移動した親矩形」を渡す（アンカー解決ごと動く）
        UiRectPx contentRect = current;
        contentRect.minX -= sv->scrollX; contentRect.maxX -= sv->scrollX;
        contentRect.minY -= sv->scrollY; contentRect.maxY -= sv->scrollY;

        const UiRectPx* prevClip   = ctx.hitClip;
        UiRectPx*       prevBounds = ctx.contentBounds;
        UiRectPx bounds{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
        ctx.hitClip       = &clip;
        ctx.contentBounds = &bounds;
        if (ctx.dl)
            ctx.dl->PushClipRect(ImVec2(clip.minX, clip.minY), ImVec2(clip.maxX, clip.maxY), true);

        auto it = ctx.children->find(e);
        if (it != ctx.children->end())
            for (entt::entity child : it->second)
                DrawUiSubtree(child, contentRect, ctx);

        if (ctx.dl)
            ctx.dl->PopClipRect();
        ctx.hitClip       = prevClip;
        ctx.contentBounds = prevBounds;

        // 計測結果（コンテンツ原点=平行移動後の親左上からの広がり）を保存 → 次フレームのクランプへ
        sv->_contentW = (bounds.maxX > bounds.minX) ? std::max(0.0f, bounds.maxX - contentRect.minX) : 0.0f;
        sv->_contentH = (bounds.maxY > bounds.minY) ? std::max(0.0f, bounds.maxY - contentRect.minY) : 0.0f;

        // スクロールバー（インジケータのみ。ドラッグ不可）
        if (ctx.dl && sv->showBar)
        {
            const UiRectPx s = ToScreen(current, ctx);
            const ImU32 bcol = ToImCol({sv->barColor.x, sv->barColor.y, sv->barColor.z,
                                        sv->barColor.w * ctx.alphaMul});
            const float barW = std::max(2.0f, 4.0f * ctx.scale);
            if (sv->vertical && sv->_contentH > current.Height() + 0.5f)
            {
                const float frac  = current.Height() / sv->_contentH;
                const float thumb = std::max(12.0f, s.Height() * frac);
                const float t     = sv->scrollY / std::max(1.0f, sv->_contentH - current.Height());
                const float y0    = s.minY + (s.Height() - thumb) * t;
                ctx.dl->AddRectFilled(ImVec2(s.maxX - barW - 2.0f, y0),
                                      ImVec2(s.maxX - 2.0f, y0 + thumb), bcol, barW * 0.5f);
            }
            if (sv->horizontal && sv->_contentW > current.Width() + 0.5f)
            {
                const float frac  = current.Width() / sv->_contentW;
                const float thumb = std::max(12.0f, s.Width() * frac);
                const float t     = sv->scrollX / std::max(1.0f, sv->_contentW - current.Width());
                const float x0    = s.minX + (s.Width() - thumb) * t;
                ctx.dl->AddRectFilled(ImVec2(x0, s.maxY - barW - 2.0f),
                                      ImVec2(x0 + thumb, s.maxY - 2.0f), bcol, barW * 0.5f);
            }
        }

        // ホイール配送用にビューポート矩形を収集（クリップ交差済み）
        if (ctx.interactive)
            PushHitRect(ctx.scrollRects, e, ToScreen(current, ctx), prevClip);
    }
    else
    {
        auto it = ctx.children->find(e);
        if (it != ctx.children->end())
        {
            for (entt::entity child : it->second)
                DrawUiSubtree(child, current, ctx);
        }
    }
    ctx.alphaMul = parentAlpha;   // 兄弟へ波及しないよう復元（push/pop）
}

// RenderAndUpdateInput / RenderPreview / ResolveRects の共通部:
// キャンバス収集（sortOrder 昇順）→ 子リスト構築 → キャンバス毎のレイアウト解決＋
// 描画（ctx.dl 有効時）/ 矩形収集（ctx.resolvedOut 有効時）。入力解決は含まない
// （RenderAndUpdateInput だけがこの後に行う）。ctx には入力スナップショット等を
// 呼び出し元で設定済みのものを受け取り、ここでは children とキャンバス毎の変換
// （originX/Y・scale・canvasEntity）だけを更新する。
// 戻り値: 可視キャンバスが 1 つ以上あったか（false なら何もしていない）。
bool ResolveAndDrawCanvases(entt::registry& reg, float ox, float oy, float vw, float vh,
                            UiDrawContext& ctx)
{
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
        return false;
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
    // 兄弟順を UIRect::order 昇順に（stable = 同値・UIRect 無しノードは走査順を維持）。
    // UIエディタの階層ツリーの並べ替えがここに効く（後ろほど手前に描かれる）。
    for (auto& [parent, list] : children)
    {
        std::stable_sort(list.begin(), list.end(),
                         [&reg](entt::entity a, entt::entity b)
                         {
                             const auto* ra = reg.try_get<UIRect>(a);
                             const auto* rb = reg.try_get<UIRect>(b);
                             return (ra ? ra->order : 0) < (rb ? rb->order : 0);
                         });
    }
    ctx.children = &children;

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
        ctx.canvasEntity = entry.e;

        auto it = children.find(entry.e);
        if (it == children.end()) continue;
        for (entt::entity child : it->second)
            DrawUiSubtree(child, canvasRect, ctx);
    }

    ctx.children = nullptr;   // children はローカル所有。ダングリング参照を残さない
    return true;
}

} // namespace

void UISystem::RenderAndUpdateInput(entt::registry& reg, ImDrawList* dl,
                                    float ox, float oy, float vw, float vh,
                                    ResourceManager* resources, DescriptorHeap* srvHeap,
                                    ID3D12GraphicsCommandList* cmdList,
                                    const UiNavInput& nav)
{
    if (!dl || vw <= 0.0f || vh <= 0.0f)
        return;

    // UIAnimator / tween を進める（Play / ゲームモード中のみここへ来る。エディタプレビューは
    // RenderPreview 経路なのでアニメは進まず最終ポーズで表示される）。
    UpdateUiAnimations(reg, ImGui::GetIO().DeltaTime);

    // このフレームの入力スナップショット（##GameUI ウィンドウ内で呼ばれる前提）
    std::vector<UiHitEntry> buttonRects;   // interactable な UIButton（描画順）
    std::vector<UiHitEntry> sliderRects;   // interactable な UISlider
    std::vector<UiHitEntry> toggleRects;   // interactable な UIToggle
    std::vector<UiHitEntry> scrollRects;   // UIScrollView のビューポート
    std::vector<UiHitEntry> blockers;      // クリックを遮る要素（描画順。後ろほど手前）
    UiDrawContext ctx;
    ctx.reg           = &reg;
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
    ctx.sliderRects   = &sliderRects;
    ctx.toggleRects   = &toggleRects;
    ctx.scrollRects   = &scrollRects;
    ctx.blockers      = &blockers;

    // レイアウト解決＋描画＋レイキャスト収集（RenderPreview / ResolveRects と共通）
    if (!ResolveAndDrawCanvases(reg, ox, oy, vw, vh, ctx))
        return;

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

    // topmost 自身が interactable なウィジェット（Button/Slider/Toggle）ならそれ。無ければ
    // Transform::parent を遡って最初のウィジェットへバブリング（ウィジェット内の子アイコン
    // 画像がクリックを吸っても親が反応する）。どこにも無ければクリックは吸収されただけ。
    entt::entity effectiveWidget = entt::null;
    entt::entity walk = topmost;
    for (int depth = 0; depth < 64 && walk != entt::null && reg.valid(walk); ++depth)
    {
        const auto* b = reg.try_get<UIButton>(walk);
        const auto* sl = reg.try_get<UISlider>(walk);
        const auto* tg = reg.try_get<UIToggle>(walk);
        if ((b && b->interactable) || (sl && sl->interactable) || (tg && tg->interactable))
        {
            effectiveWidget = walk;
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
            const bool wasHovered = btn->_hovered;
            btn->_hovered = (hit.e == effectiveWidget);
            if (btn->_hovered && !wasHovered && !btn->hoverSound.empty())
                m_pendingSfx.push_back(btn->hoverSound);   // ホバー開始の瞬間だけ 1 回
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
                if (inside)
                {
                    if (!btn->onClickEvent.empty())
                        m_pendingClicks.push_back({btn->onClickEvent, hit.e});
                    if (!btn->clickSound.empty())
                        m_pendingSfx.push_back(btn->clickSound);
                }
                btn->_pressed = false;
            }
            else if (!ctx.mouseDown)
            {
                btn->_pressed = false;   // フォーカス喪失等で release を取り逃した場合の安全網
            }
        }
    }

    // --- UIToggle: ボタンと同じ press → release-inside で isOn 反転 + onChangeEvent ---
    for (const UiHitEntry& hit : toggleRects)
    {
        auto* tgl = reg.try_get<UIToggle>(hit.e);
        if (!tgl)
            continue;
        const bool inside = mouseValid && hit.rect.Contains(ctx.mousePos.x, ctx.mousePos.y);
        if (!tgl->_pressed)
        {
            tgl->_hovered = (hit.e == effectiveWidget);
            if (tgl->_hovered && ctx.mouseClicked)
                tgl->_pressed = true;
        }
        else
        {
            tgl->_hovered = inside;
            if (ctx.mouseReleased)
            {
                if (inside)
                {
                    tgl->isOn = !tgl->isOn;
                    if (!tgl->onChangeEvent.empty())
                        m_pendingClicks.push_back({tgl->onChangeEvent, hit.e,
                                                   true, tgl->isOn ? 1.0 : 0.0});
                }
                tgl->_pressed = false;
            }
            else if (!ctx.mouseDown)
            {
                tgl->_pressed = false;
            }
        }
    }

    // --- UISlider: クリックで即その位置へ + ドラッグ追従（ポインタキャプチャ相当）---
    for (const UiHitEntry& hit : sliderRects)
    {
        auto* sld = reg.try_get<UISlider>(hit.e);
        if (!sld)
            continue;
        if (!sld->_dragging)
        {
            sld->_hovered = (hit.e == effectiveWidget);
            if (sld->_hovered && ctx.mouseClicked)
                sld->_dragging = true;
        }
        if (sld->_dragging)
        {
            if (!ctx.mouseDown)
            {
                sld->_dragging = false;
            }
            else
            {
                // マウス X → 実値（描画と同じ「つまみ半径ぶん内側」のトラック範囲で正規化）
                const float knobR = std::max(3.0f, hit.rect.Height() * 0.5f);
                const float x0 = hit.rect.minX + knobR, x1 = hit.rect.maxX - knobR;
                const float t = std::clamp((ctx.mousePos.x - x0) / std::max(1.0f, x1 - x0),
                                           0.0f, 1.0f);
                float v = sld->minValue + (sld->maxValue - sld->minValue) * t;
                if (sld->step > 0.0f)
                    v = sld->minValue + std::round((v - sld->minValue) / sld->step) * sld->step;
                v = std::clamp(v, std::min(sld->minValue, sld->maxValue),
                               std::max(sld->minValue, sld->maxValue));
                if (v != sld->value)
                {
                    sld->value = v;
                    if (!sld->onChangeEvent.empty())
                        m_pendingClicks.push_back({sld->onChangeEvent, hit.e, true,
                                                   static_cast<double>(v)});
                }
            }
        }
    }

    // --- UIScrollView: マウスホイール（最前面 = 最後に描かれたビューが受ける）---
    // クランプは翌フレームの描画側（コンテンツ計測とセット）で行う。
    const float wheel = ImGui::GetIO().MouseWheel;
    if (mouseValid && wheel != 0.0f)
    {
        for (auto it = scrollRects.rbegin(); it != scrollRects.rend(); ++it)
        {
            if (!it->rect.Contains(ctx.mousePos.x, ctx.mousePos.y))
                continue;
            if (auto* sv = reg.try_get<UIScrollView>(it->e))
            {
                if (sv->vertical)
                    sv->scrollY -= wheel * sv->wheelSpeed;
                else if (sv->horizontal)
                    sv->scrollX -= wheel * sv->wheelSpeed;
            }
            break;   // 一番手前のスクロールビューだけが受ける（ネスト時の同時スクロール防止）
        }
    }

    // ===== フォーカスナビゲーション（ゲームパッド / キーボード）=====
    // フォーカス対象 = このフレームに収集した操作可能ウィジェット全件（描画順）。
    std::vector<UiHitEntry> focusables;
    focusables.reserve(buttonRects.size() + sliderRects.size() + toggleRects.size());
    focusables.insert(focusables.end(), buttonRects.begin(), buttonRects.end());
    focusables.insert(focusables.end(), sliderRects.begin(), sliderRects.end());
    focusables.insert(focusables.end(), toggleRects.begin(), toggleRects.end());

    const auto findFocusable = [&focusables](entt::entity e) -> const UiHitEntry* {
        for (const auto& f : focusables)
            if (f.e == e) return &f;
        return nullptr;
    };

    // マウスクリックでもフォーカスは移る（パッド⇄マウスの併用で迷子にならないように）
    if (ctx.mouseClicked && effectiveWidget != entt::null && findFocusable(effectiveWidget))
        m_focused = effectiveWidget;

    // ---- 方向入力のエッジ + キーリピート（最初 0.4s、以後 0.12s）----
    const bool dirHeld[4] = {nav.left, nav.right, nav.up, nav.down};
    const bool dirPrev[4] = {m_prevNav.left, m_prevNav.right, m_prevNav.up, m_prevNav.down};
    int dir = -1;
    for (int i = 0; i < 4; ++i)
        if (dirHeld[i] && !dirPrev[i]) { dir = i; break; }   // 新規押下が最優先
    if (dir >= 0)
    {
        m_navHeldDir  = dir;
        m_navRepeatT  = 0.4f;
    }
    else if (m_navHeldDir >= 0 && dirHeld[m_navHeldDir])
    {
        m_navRepeatT -= ImGui::GetIO().DeltaTime;
        if (m_navRepeatT <= 0.0f) { dir = m_navHeldDir; m_navRepeatT = 0.12f; }
    }
    else
    {
        m_navHeldDir = -1;
    }

    if (dir >= 0 && !focusables.empty())
    {
        const UiHitEntry* cur = findFocusable(m_focused);
        auto* focusedSlider = (cur && reg.all_of<UISlider>(m_focused))
                            ? &reg.get<UISlider>(m_focused) : nullptr;
        if (focusedSlider && (dir == 0 || dir == 1))
        {
            // フォーカス中のスライダー: 左右は移動ではなく値変更（step 未設定なら 1/20 刻み）
            auto& sld = *focusedSlider;
            const float range = sld.maxValue - sld.minValue;
            const float delta = (sld.step > 0.0f) ? sld.step : range * 0.05f;
            float v = sld.value + (dir == 1 ? delta : -delta);
            if (sld.step > 0.0f)
                v = sld.minValue + std::round((v - sld.minValue) / sld.step) * sld.step;
            v = std::clamp(v, std::min(sld.minValue, sld.maxValue),
                           std::max(sld.minValue, sld.maxValue));
            if (v != sld.value)
            {
                sld.value = v;
                if (!sld.onChangeEvent.empty())
                    m_pendingClicks.push_back({sld.onChangeEvent, m_focused, true,
                                               static_cast<double>(v)});
            }
        }
        else if (!cur)
        {
            // フォーカスが無い/消えた: 一番左上のウィジェットへ（初回のナビ入力で必ず現れる）
            const UiHitEntry* best = &focusables.front();
            for (const auto& f : focusables)
                if (f.rect.minY + f.rect.minX * 0.001f
                    < best->rect.minY + best->rect.minX * 0.001f)
                    best = &f;
            m_focused = best->e;
            m_confirmHeld = false;
            if (const auto* b = reg.try_get<UIButton>(m_focused); b && !b->hoverSound.empty())
                m_pendingSfx.push_back(b->hoverSound);
        }
        else
        {
            // 空間ナビゲーション: 方向の成分が正で、直交ずれのペナルティ込み最短の候補へ
            const float cx = (cur->rect.minX + cur->rect.maxX) * 0.5f;
            const float cy = (cur->rect.minY + cur->rect.maxY) * 0.5f;
            const UiHitEntry* best = nullptr;
            float bestScore = FLT_MAX;
            for (const auto& f : focusables)
            {
                if (f.e == m_focused) continue;
                const float fx = (f.rect.minX + f.rect.maxX) * 0.5f - cx;
                const float fy = (f.rect.minY + f.rect.maxY) * 0.5f - cy;
                float primary = 0.0f, ortho = 0.0f;
                switch (dir)
                {
                case 0: primary = -fx; ortho = std::fabs(fy); break;   // 左
                case 1: primary =  fx; ortho = std::fabs(fy); break;   // 右
                case 2: primary = -fy; ortho = std::fabs(fx); break;   // 上
                default: primary = fy; ortho = std::fabs(fx); break;   // 下
                }
                if (primary <= 1.0f) continue;   // その方向に無い
                const float score = primary + ortho * 2.0f;
                if (score < bestScore) { bestScore = score; best = &f; }
            }
            if (best)
            {
                m_focused = best->e;
                m_confirmHeld = false;
                if (const auto* b = reg.try_get<UIButton>(m_focused); b && !b->hoverSound.empty())
                    m_pendingSfx.push_back(b->hoverSound);
                // ponytail: スクロールビュー内のフォーカス先への自動スクロールは未対応。
                // リスト UI をパッドで操作する時に必要になったら「包含スクロールビューへ
                // 矩形が収まるよう scrollY を調整」を足す
            }
        }
    }

    // ---- 決定（押しで押下表示、離しで確定 = マウスの release-inside と同じ手触り）----
    const bool confirmPressed  = nav.confirm && !m_prevNav.confirm;
    const bool confirmReleased = !nav.confirm && m_prevNav.confirm;
    if (const UiHitEntry* cur = findFocusable(m_focused))
    {
        if (auto* btn = reg.try_get<UIButton>(m_focused))
        {
            if (confirmPressed) { btn->_pressed = true; m_confirmHeld = true; }
            if (confirmReleased && m_confirmHeld)
            {
                btn->_pressed = false;
                m_confirmHeld = false;
                if (!btn->onClickEvent.empty())
                    m_pendingClicks.push_back({btn->onClickEvent, m_focused});
                if (!btn->clickSound.empty())
                    m_pendingSfx.push_back(btn->clickSound);
            }
        }
        else if (auto* tgl = reg.try_get<UIToggle>(m_focused))
        {
            if (confirmPressed) { tgl->_pressed = true; m_confirmHeld = true; }
            if (confirmReleased && m_confirmHeld)
            {
                tgl->_pressed = false;
                m_confirmHeld = false;
                tgl->isOn = !tgl->isOn;
                if (!tgl->onChangeEvent.empty())
                    m_pendingClicks.push_back({tgl->onChangeEvent, m_focused,
                                               true, tgl->isOn ? 1.0 : 0.0});
            }
        }

        // フォーカスリング（UI 全体の最前面。少し外側の角丸枠 + 薄いグロー）
        const UiRectPx& r = cur->rect;
        const ImU32 ring = IM_COL32(120, 180, 255, 235);
        dl->AddRect(ImVec2(r.minX - 5.0f, r.minY - 5.0f), ImVec2(r.maxX + 5.0f, r.maxY + 5.0f),
                    IM_COL32(120, 180, 255, 60), 8.0f, 0, 6.0f);
        dl->AddRect(ImVec2(r.minX - 3.0f, r.minY - 3.0f), ImVec2(r.maxX + 3.0f, r.maxY + 3.0f),
                    ring, 6.0f, 0, 2.0f);
    }
    else
    {
        m_confirmHeld = false;
    }

    m_prevNav = nav;
}

void UISystem::RenderPreview(entt::registry& reg, ImDrawList* dl,
                             float ox, float oy, float vw, float vh,
                             ResourceManager* resources, DescriptorHeap* srvHeap,
                             ID3D12GraphicsCommandList* cmdList)
{
    if (!dl || vw <= 0.0f || vh <= 0.0f)
        return;

    // 入力処理なしのプレビュー: interactive=false でボタンは normalColor 固定、
    // _hovered/_pressed は読み書きしない。buttonRects/blockers も収集しない
    // （= 入力解決フェーズを丸ごとスキップ）。
    UiDrawContext ctx;
    ctx.reg         = &reg;
    ctx.dl          = dl;
    ctx.viewport    = {ox, oy, ox + vw, oy + vh};
    ctx.resources   = resources;
    ctx.srvHeap     = srvHeap;
    ctx.cmdList     = cmdList;
    ctx.interactive = false;

    ResolveAndDrawCanvases(reg, ox, oy, vw, vh, ctx);
}

void UISystem::ResolveRects(entt::registry& reg,
                            float ox, float oy, float vw, float vh,
                            std::vector<UiResolvedRect>& out)
{
    out.clear();
    if (vw <= 0.0f || vh <= 0.0f)
        return;

    // dl=nullptr で描画をスキップし、レイアウト解決と矩形収集のみ行う（副作用なし）
    UiDrawContext ctx;
    ctx.reg         = &reg;
    ctx.interactive = false;
    ctx.resolvedOut = &out;

    ResolveAndDrawCanvases(reg, ox, oy, vw, vh, ctx);
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
        if (c.hasValue)
            ev.set("value", c.value);   // スライダー実値 / トグル 1・0（Lua 側は e.value）
        bus.Emit(ev);   // 即時発火（呼び出し元が Lua OnUpdate より前のタイミングを保証する）
    }
}

void UISystem::ResetRuntimeState(entt::registry& reg)
{
    m_pendingClicks.clear();
    m_pendingSfx.clear();
    m_focused     = entt::null;
    m_prevNav     = {};
    m_navHeldDir  = -1;
    m_navRepeatT  = 0.0f;
    m_confirmHeld = false;
    for (auto [e, btn] : reg.view<UIButton>().each())
    {
        btn._hovered = false;
        btn._pressed = false;
    }
    for (auto [e, sld] : reg.view<UISlider>().each())
    {
        sld._hovered  = false;
        sld._dragging = false;
    }
    for (auto [e, tgl] : reg.view<UIToggle>().each())
    {
        tgl._hovered = false;
        tgl._pressed = false;
    }
    // UIAnimator: 次の Play で出現アニメが最初から再生されるように全ランタイム状態を戻す
    for (auto [e, an] : reg.view<UIAnimator>().each())
    {
        an._t = 0.0f;
        an._mode = 0;
        an._hoverS = 1.0f;
        an._loopT = 0.0f;
        an._curScale = 1.0f;
        an._curAlpha = 1.0f;
        an._curOffset = {0.0f, 0.0f};
    }
    // tween はランタイム専用なので丸ごと破棄
    {
        auto view = reg.view<UITweenState>();
        reg.remove<UITweenState>(view.begin(), view.end());
    }
}

} // namespace dx12e
