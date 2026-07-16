#include "ui/UISystem.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>

#include <cstdio>    // snprintf（tween 数字ロール）
#include <cstdlib>   // atof（数字ロールの from 捕捉）
#include <cstring>

#include <imgui.h>
#include <imgui_internal.h>   // ShadeVertsLinearColorGradientKeepAlpha（UIImage グラデーション）

#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"
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

// ---- 回転/スキューの視覚変換（UIRect.rotation / skewX）----
// レイアウト解決は常に軸平行（AABB）で行い、変換は「描画済み頂点」と「ヒット判定」にだけ
// pivot（スクリーン座標）回りで掛ける。ネストは DrawUiSubtree のキャプチャ入れ子で自然合成。

// M = T(pivot) · R(rot) · SkewX(tan) · T(-pivot)（スキューした形を回す）
UiXform2x3 MakeUiRotSkew(float rotDeg, float skewXDeg, float pivotX, float pivotY)
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float c = std::cos(rotDeg * kDegToRad);
    const float s = std::sin(rotDeg * kDegToRad);
    // ±85° でクランプ（tan の発散防止。それ以上は見た目も破綻する）
    const float k = std::tan(std::clamp(skewXDeg, -85.0f, 85.0f) * kDegToRad);
    UiXform2x3 m;
    m.m00 = c;
    m.m01 = c * k - s;
    m.m10 = s;
    m.m11 = s * k + c;
    m.m02 = pivotX - m.m00 * pivotX - m.m01 * pivotY;
    m.m12 = pivotY - m.m10 * pivotX - m.m11 * pivotY;
    return m;
}

UiXform2x3 UiXformCombine(const UiXform2x3& outer, const UiXform2x3& inner)
{
    UiXform2x3 m;
    m.m00 = outer.m00 * inner.m00 + outer.m01 * inner.m10;
    m.m01 = outer.m00 * inner.m01 + outer.m01 * inner.m11;
    m.m02 = outer.m00 * inner.m02 + outer.m01 * inner.m12 + outer.m02;
    m.m10 = outer.m10 * inner.m00 + outer.m11 * inner.m10;
    m.m11 = outer.m10 * inner.m01 + outer.m11 * inner.m11;
    m.m12 = outer.m10 * inner.m02 + outer.m11 * inner.m12 + outer.m12;
    return m;
}

// [vtxStart, dl->VtxBuffer.Size) の頂点位置を一括変換（ImRotate と同じ後がけ方式）。
// UV・色・インデックス・ClipRect には触れないため描画コマンド構造は不変。
void TransformUiVerts(ImDrawList* dl, int vtxStart, const UiXform2x3& m)
{
    ImDrawVert* v   = dl->VtxBuffer.Data + vtxStart;
    ImDrawVert* end = dl->VtxBuffer.Data + dl->VtxBuffer.Size;
    for (; v < end; ++v)
        v->pos = m.Apply(v->pos.x, v->pos.y);
}

// レイキャスト対象 1 件（エンティティ + 解決済み矩形）。描画順に積む。
// 変換なし（hasXform=false）: rect はクリップ交差済みのスクリーン矩形（従来どおり）。
// 変換あり: rect は「レイアウト空間」（変換前）の矩形のまま。判定はマウス点を invXform で
// レイアウト空間へ逆写像してから行い、クリップはスクリーン空間のまま別枠（clip）で AND する。
struct UiHitEntry
{
    entt::entity e = entt::null;
    UiRectPx     rect;
    bool         hasXform = false;
    UiXform2x3   xform;      // レイアウト空間 → 実スクリーン（累積）
    UiXform2x3   invXform;   // 実スクリーン → レイアウト空間
    bool         hasClip = false;
    UiRectPx     clip;       // スクリーン空間クリップ（スクロールビュー）

    bool Contains(float x, float y) const
    {
        if (hasXform)
        {
            if (hasClip && !clip.Contains(x, y)) return false;
            const ImVec2 p = invXform.Apply(x, y);
            return rect.Contains(p.x, p.y);
        }
        return rect.Contains(x, y);
    }
    // 変形後の中心（フォーカスナビの空間スコア用）
    ImVec2 Center() const
    {
        const float cx = (rect.minX + rect.maxX) * 0.5f;
        const float cy = (rect.minY + rect.maxY) * 0.5f;
        return hasXform ? xform.Apply(cx, cy) : ImVec2(cx, cy);
    }
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

    // 祖先から継承するカラー乗数（RGB。tween の color フラッシュが子孫にまとめて掛かる）。
    // 1 超えあり（白フラッシュ用。ToImCol 側で 0..1 に飽和される）。push/pop 式。
    DirectX::XMFLOAT3 colorMul{1.0f, 1.0f, 1.0f};

    // 祖先から継承する累積回転/スキュー変換（ヒット判定・resolvedOut 用。頂点には
    // DrawUiSubtree が local 変形だけを後がけする＝二重適用しない）。push/pop 式。
    bool       hasXform = false;
    UiXform2x3 xform;      // レイアウトスクリーン空間 → 実スクリーン
    UiXform2x3 invXform;   // 逆変換（push 時に一度だけ計算）

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
// 変換あり（ctx.hasXform）: rect はレイアウト空間・クリップはスクリーン空間で空間が違うため
// 交差で縮めず、クリップを別枠で持って判定時に AND する。早期リジェクトだけは
// 「変換後 4 隅の AABB × クリップ」で保守的に行う。
void PushHitRect(std::vector<UiHitEntry>* out, entt::entity e, const UiRectPx& s,
                 const UiRectPx* clip, const UiDrawContext& ctx)
{
    if (!out) return;
    UiHitEntry entry;
    entry.e = e;
    if (ctx.hasXform)
    {
        entry.rect     = s;
        entry.hasXform = true;
        entry.xform    = ctx.xform;
        entry.invXform = ctx.invXform;
        if (clip)
        {
            entry.hasClip = true;
            entry.clip    = *clip;
            // 早期リジェクト: 変換後 4 隅の AABB がクリップと交差しなければ積まない
            const ImVec2 c0 = ctx.xform.Apply(s.minX, s.minY);
            const ImVec2 c1 = ctx.xform.Apply(s.maxX, s.minY);
            const ImVec2 c2 = ctx.xform.Apply(s.maxX, s.maxY);
            const ImVec2 c3 = ctx.xform.Apply(s.minX, s.maxY);
            const float bMinX = std::min(std::min(c0.x, c1.x), std::min(c2.x, c3.x));
            const float bMinY = std::min(std::min(c0.y, c1.y), std::min(c2.y, c3.y));
            const float bMaxX = std::max(std::max(c0.x, c1.x), std::max(c2.x, c3.x));
            const float bMaxY = std::max(std::max(c0.y, c1.y), std::max(c2.y, c3.y));
            if (bMinX >= clip->maxX || bMaxX <= clip->minX
                || bMinY >= clip->maxY || bMaxY <= clip->minY)
                return;
        }
    }
    else
    {
        UiRectPx r = s;
        if (clip)
        {
            r.minX = std::max(r.minX, clip->minX);
            r.minY = std::max(r.minY, clip->minY);
            r.maxX = std::min(r.maxX, clip->maxX);
            r.maxY = std::min(r.maxY, clip->maxY);
            if (r.minX >= r.maxX || r.minY >= r.maxY) return;
        }
        entry.rect = r;
    }
    out->push_back(entry);
}

// イージング（p: 0..1 → 0..1）。列挙は UIAnimator::showEasing / UiTween::easing 共通:
//   0=リニア 1=イーズイン 2=イーズアウト 3=イン/アウト 4=バック(勢い) 5=バウンス 6=弾性
//   7=エクスポ(鋭く減速) 8=インバック(溜めて発進) 9=イン/アウトバック 10=クイント(強い減速)
//   11=サイン(ゆったり対称)
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
    case 7:   // easeOutExpo（鋭く立ち上がり滑らかに止まる。スナップの効いた UI 定番）
        return (p >= 1.0f) ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * p);
    case 8:   // easeInBack（動く前に逆方向へ溜める = anticipation。退場にも合う）
    {
        constexpr float c1 = 1.70158f;
        constexpr float c3 = c1 + 1.0f;
        return c3 * p * p * p - c1 * p * p;
    }
    case 9:   // easeInOutBack（溜め → 行き過ぎ → 収束）
    {
        constexpr float c2 = 1.70158f * 1.525f;
        if (p < 0.5f)
        {
            const float q = 2.0f * p;
            return q * q * ((c2 + 1.0f) * q - c2) * 0.5f;
        }
        const float q = 2.0f * p - 2.0f;
        return (q * q * ((c2 + 1.0f) * q + c2) + 2.0f) * 0.5f;
    }
    case 10:  // easeOutQuint（cubic より強い減速。大きい移動距離向き）
    {
        const float q = 1.0f - p;
        const float q2 = q * q;
        return 1.0f - q2 * q2 * q;
    }
    case 11:  // easeInOutSine（ゆったり対称。字幕・アンビエント向き）
        return 0.5f - 0.5f * std::cos(p * 3.14159265f);
    default:  // 0: リニア
        return p;
    }
}

// 数字ロール（UiTween::hasCount）: countFmt(printf 書式)で数値をテキスト化して UIText へ書く。
// 書式は ScriptEngine 側で [%][フラグ/幅/.精度][d|f] のみに検証済み（不正は "%d" に落ちている）
void SetUiCountText(UIText& txt, double v, const std::string& fmt)
{
    char buf[64];
    const char conv = fmt.empty() ? 'd' : fmt.back();
    if (conv == 'f')
        std::snprintf(buf, sizeof(buf), fmt.c_str(), v);
    else
        std::snprintf(buf, sizeof(buf), fmt.empty() ? "%d" : fmt.c_str(),
                      static_cast<int>(std::llround(v)));
    txt.text = buf;
}

// UIAnimator / UITweenState を dt で進め、今フレームの視覚合成結果（_cur*）を確定する。
// Play / ゲームモード中のみ呼ばれる（RenderAndUpdateInput の冒頭）。
void UpdateUiAnimations(entt::registry& reg, float dt)
{
    dt = std::clamp(dt, 0.0f, 0.1f);   // ヒッチ時の飛びを抑える

    // 完了コールバック（tween の onComplete）。ビュー走査中に Lua を呼ぶと tweens vector が
    // 再入で伸びる恐れがあるため、集めておいて全走査後にまとめて発火する
    std::vector<std::function<void()>> completed;

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

        // 出現 / 消滅の状態機械（スケールは X/Y 別 = フリップ等の非等方アニメ用）
        float alpha = 1.0f, scaleX = 1.0f, scaleY = 1.0f, rot = 0.0f;
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
            case 2:  alpha = ev; scaleX = scaleY = 0.5f + 0.5f * ev;        break;  // ポップ
            case 3:  alpha = ev; off.x = -an.slideOffset * (1.0f - ev);     break;  // 左から
            case 4:  alpha = ev; off.x =  an.slideOffset * (1.0f - ev);     break;  // 右から
            case 5:  alpha = ev; off.y = -an.slideOffset * (1.0f - ev);     break;  // 上から
            case 6:  alpha = ev; off.y =  an.slideOffset * (1.0f - ev);     break;  // 下から
            case 7:  alpha = ev; scaleX = scaleY = 0.5f + 0.5f * ev;                // スピン入場
                     rot = -180.0f * (1.0f - ev);                           break;
            case 8:  // バウンド落下（showEasing に関わらずバウンスで着地。slideOffset=落下距離）
                     alpha = std::min(1.0f, p * 4.0f);
                     off.y = -an.slideOffset * (1.0f - UiEase(5, p));       break;
            case 9:  alpha = ev; scaleY = ev;                               break;  // フリップ（縦つぶれから開く）
            case 10: // シェイク入場（すっと出て減衰振動。振幅は slideOffset の 1/4）
                     alpha = std::min(1.0f, p * 3.0f);
                     off.x = std::sin(p * 25.1327412f) * an.slideOffset * 0.25f * (1.0f - p);
                                                                            break;
            case 11: alpha = ev; scaleX = ev;                               break;  // 横フリップ（扉/カード）
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
            else if (an.loopAnim == 2)                                                    // パルス
            {
                scaleX *= 1.0f + s * an.loopAmount;
                scaleY *= 1.0f + s * an.loopAmount;
            }
            else if (an.loopAnim == 3)                                                    // 点滅
                alpha *= 1.0f - (0.5f + 0.5f * s) * std::clamp(an.loopAmount, 0.0f, 1.0f);
            else if (an.loopAnim == 4) rot += an._loopT * an.loopSpeed * 360.0f;          // スピン
            else if (an.loopAnim == 5) rot += s * an.loopAmount;                          // スウィング(±度)
        }

        an._curScaleX = (std::max)(0.0f, scaleX * an._hoverS);
        an._curScaleY = (std::max)(0.0f, scaleY * an._hoverS);
        an._curAlpha  = std::clamp(alpha, 0.0f, 1.0f);
        an._curRot    = rot;
        an._curOffset = off;
    }

    // ---- tween（scene:tweenUi。移動は UIRect offset を直接進め、拡縮/アルファは視覚のみ）----
    for (auto [e, tw] : reg.view<UITweenState>().each())
    {
        float scaleX = tw.visScaleX, scaleY = tw.visScaleY;
        float alpha = tw.visAlpha, rot = tw.visRot;
        DirectX::XMFLOAT3 colv = tw.visColor;
        DirectX::XMFLOAT2 shakeOff{0.0f, 0.0f};
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
                t.scaleXFrom = tw.visScaleX;
                t.scaleYFrom = tw.visScaleY;
                t.alphaFrom  = tw.visAlpha;
                t.rotFrom    = tw.visRot;
                t.colFrom    = tw.visColor;
                if (t.hasFill)
                {
                    // from = 開始時の現在値（直前の状態から滑らかに繋がる = ゴーストバー可）
                    if (const auto* img = reg.try_get<UIImage>(e)) t.fillFrom = img->fillAmount;
                    else                                           t.hasFill  = false;
                }
                if (t.hasCount)
                {
                    if (const auto* ut = reg.try_get<UIText>(e))
                    {
                        if (!t.countFromSet)   // from 省略時は現在テキストの数値解釈（失敗=0）
                            t.countFrom = std::atof(ut->text.c_str());
                    }
                    else
                    {
                        t.hasCount = false;
                    }
                }
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
            if (t.hasScaleX) scaleX = t.scaleXFrom + (t.scaleXTo - t.scaleXFrom) * ev;
            if (t.hasScaleY) scaleY = t.scaleYFrom + (t.scaleYTo - t.scaleYFrom) * ev;
            if (t.hasAlpha)  alpha = t.alphaFrom + (t.alphaTo - t.alphaFrom) * ev;
            if (t.hasRotate) rot   = t.rotFrom + (t.rotTo - t.rotFrom) * ev;
            if (t.hasColor)
            {
                colv = {t.colFrom.x + (t.colTo.x - t.colFrom.x) * ev,
                        t.colFrom.y + (t.colTo.y - t.colFrom.y) * ev,
                        t.colFrom.z + (t.colTo.z - t.colFrom.z) * ev};
            }
            if (t.hasShake)
            {
                // 減衰付き振動（duration で振幅 0 へ。周波数違いの sin/cos で楕円軌道にしない）
                const float tt    = t.t - t.delay;
                const float decay = t.shakeAmp * (1.0f - p);
                shakeOff.x += std::sin(tt * t.shakeFreq * 6.2831853f) * decay;
                shakeOff.y += std::sin(tt * t.shakeFreq * 6.2831853f * 0.83f + 1.3f) * decay * 0.6f;
            }
            if (t.hasFill)
            {
                if (auto* img = reg.try_get<UIImage>(e))
                    img->fillAmount = t.fillFrom + (t.fillTo - t.fillFrom) * ev;
            }
            if (t.hasCount)
            {
                if (auto* ut = reg.try_get<UIText>(e))
                    SetUiCountText(*ut,
                                   t.countFrom + (t.countTo - t.countFrom)
                                       * static_cast<double>(ev),
                                   t.countFmt);
            }
            if (p >= 1.0f)
            {
                // 完了: 視覚値は持続させる（alpha=0 で消したままにできる）。shake は 0 へ減衰済み
                if (t.hasScaleX) tw.visScaleX = t.scaleXTo;
                if (t.hasScaleY) tw.visScaleY = t.scaleYTo;
                if (t.hasAlpha)  tw.visAlpha  = t.alphaTo;
                if (t.hasRotate) tw.visRot    = t.rotTo;
                if (t.hasColor)  tw.visColor  = t.colTo;
                if (t.onComplete) completed.push_back(std::move(t.onComplete));
                it = tw.tweens.erase(it);
            }
            else
            {
                ++it;
            }
        }
        tw._curScaleX = (std::max)(0.0f, scaleX);
        tw._curScaleY = (std::max)(0.0f, scaleY);
        tw._curAlpha  = std::clamp(alpha, 0.0f, 1.0f);
        tw._curRot    = rot;
        tw._curColor  = {(std::max)(0.0f, colv.x), (std::max)(0.0f, colv.y),
                         (std::max)(0.0f, colv.z)};   // 1 超えは許可（白フラッシュ）
        tw._curOffset = shakeOff;
    }

    // ---- UIText タイプライター（経過秒を進めるだけ。表示バイト数は描画側で算出）----
    for (auto [e, txt] : reg.view<UIText>().each())
    {
        if (txt.typewriterSpeed > 0.0f)
            txt._twT += dt;
    }

    // ---- tween 完了コールバック（走査完了後にまとめて発火 = Lua が tweenUi を呼んでも安全）----
    for (auto& fn : completed)
        fn();
}

ImU32 ToImCol(const DirectX::XMFLOAT4& c)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w));
}

// --- ゲームフォントレジストリ（UIText.fontPath）---
// ImFont* は ImGui アトラス（グローバル）が所有する。動的フォントアトラス
// （RendererHasTextures = TexUpdates 対応。NewFrame 中も Locked にならない）前提なので
// 描画中の遅延ロードで良く、グリフレンジ指定も不要（AddText 時に任意サイズで焼かれる）。
// ファイルは vfs::ReadAsset で読む = 出荷ゲームの暗号化 pak（game.pak）でも動く。
// キーは AssetsDir + 相対パス（プロジェクト切替で同名パスが別ファイルを指しても混ざらない）。
// 失敗はキャッシュして 1 回だけ警告 → 既定フォントへフォールバック。
ImFont* GetOrLoadUiFont(const std::string& relPath)
{
    if (relPath.empty())
        return nullptr;
    struct UiFontEntry { ImFont* font = nullptr; };
    static std::unordered_map<std::string, UiFontEntry> s_cache;
    const std::string key = PathResolver::AssetsDir() + relPath;
    auto it = s_cache.find(key);
    if (it != s_cache.end())
        return it->second.font;   // ロード失敗も nullptr でキャッシュ済み

    UiFontEntry entry;
    const std::vector<uint8_t> bytes = vfs::ReadAsset(relPath);
    if (bytes.empty())
    {
        Logger::Warn("UIText フォントが読めません（既定フォントで描画）: {}", relPath);
    }
    else
    {
        // アトラスがバッファを所有し IM_FREE で解放するため、ImGui アロケータで確保して渡す
        void* data = ImGui::MemAlloc(bytes.size());
        std::memcpy(data, bytes.data(), bytes.size());
        entry.font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
            data, static_cast<int>(bytes.size()));
        if (!entry.font)
            Logger::Warn("UIText フォントのロードに失敗（既定フォントで描画）: {}", relPath);
    }
    s_cache.emplace(key, entry);
    return entry.font;
}

// 9-slice: sliceBorder（左,上,右,下、元テクスチャpx）で 3x3 に分割して描く。
// 角はキャンバススケール倍の固定サイズ、辺と中央は引き伸ばし。
// 矩形が境界の合計より小さい場合は境界を等比で縮めて破綻を防ぐ。
// UV 側も境界合計が uvMin..uvMax の切り出しサブ矩形を超える場合は等比で縮めてからクランプする。
// fillClamp: 部分 fill（ゲージ）の表示矩形（スクリーン空間）。各セルをこの矩形で切り、
// セル内 UV も比例で切る＝クリップ矩形方式とピクセル一致の幾何 fill（GPU シザー非依存）。
void AddImage9Slice(ImDrawList* dl, ImTextureID texId,
                    const ImVec2& pMin, const ImVec2& pMax,
                    const ImVec2& uvMin, const ImVec2& uvMax,
                    float texW, float texH,
                    const DirectX::XMFLOAT4& border, float scale, ImU32 col,
                    const UiRectPx* fillClamp = nullptr)
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
            ImVec2 a(xs[c], ys[row]);
            ImVec2 b(xs[c + 1], ys[row + 1]);
            if (b.x - a.x < 0.5f || b.y - a.y < 0.5f) continue;   // 潰れたセルはスキップ
            ImVec2 uvA(us[c], vs[row]);
            ImVec2 uvB(us[c + 1], vs[row + 1]);
            if (fillClamp)
            {
                const float cw = b.x - a.x, ch = b.y - a.y;
                const ImVec2 na(std::max(a.x, fillClamp->minX), std::max(a.y, fillClamp->minY));
                const ImVec2 nb(std::min(b.x, fillClamp->maxX), std::min(b.y, fillClamp->maxY));
                if (nb.x - na.x < 0.5f || nb.y - na.y < 0.5f) continue;
                const ImVec2 nuvA(uvA.x + (uvB.x - uvA.x) * (na.x - a.x) / cw,
                                  uvA.y + (uvB.y - uvA.y) * (na.y - a.y) / ch);
                const ImVec2 nuvB(uvB.x - (uvB.x - uvA.x) * (b.x - nb.x) / cw,
                                  uvB.y - (uvB.y - uvA.y) * (b.y - nb.y) / ch);
                a = na; b = nb; uvA = nuvA; uvB = nuvB;
            }
            dl->AddImage(texId, a, b, uvA, uvB, col);
        }
    }
}

// グロススイープ（UIImage.gradientScrollSpeed）: [p0,p1] 方向の正規化位置 t に対し、帯中心
// cycle01 からの距離で頂点 RGB を bandCol へ三角減衰ブレンドする（α は保持）。帯は画面外から
// 入って画面外へ抜けるよう中心を [-余白, 1+余白] で走らせる。頂点直接シェードなので
// テクスチャ/9-slice/角丸/回転すべてに追従する（静的グラデと同じ流儀）。
void ShadeVertsGlossBand(ImDrawList* dl, int vtxStart, int vtxEnd,
                         const ImVec2& p0, const ImVec2& p1,
                         const DirectX::XMFLOAT4& bandCol, float cycle01)
{
    const ImVec2 d(p1.x - p0.x, p1.y - p0.y);
    const float len2 = d.x * d.x + d.y * d.y;
    if (len2 <= 0.0f) return;
    const float invLen2 = 1.0f / len2;
    constexpr float kHalfW  = 0.22f;   // 帯の半幅（全長に対する割合）
    constexpr float kMargin = kHalfW + 0.1f;
    const float c = -kMargin + cycle01 * (1.0f + 2.0f * kMargin);   // 帯中心
    ImDrawVert* v = dl->VtxBuffer.Data;
    for (int i = vtxStart; i < vtxEnd; ++i)
    {
        ImDrawVert& vt = v[i];
        const float t = ((vt.pos.x - p0.x) * d.x + (vt.pos.y - p0.y) * d.y) * invLen2;
        const float w = 1.0f - std::fabs(t - c) / kHalfW;
        if (w <= 0.0f) continue;
        ImVec4 cur = ImGui::ColorConvertU32ToFloat4(vt.col);
        cur.x += (bandCol.x - cur.x) * w;
        cur.y += (bandCol.y - cur.y) * w;
        cur.z += (bandCol.z - cur.z) * w;
        vt.col = ImGui::ColorConvertFloat4ToU32(cur);
    }
}

// ==== シェイプ描画（UIImage.shape / 放射 fill / タイル繰り返し）====
// 形状はスクリーン空間の凸ポリゴンとして構築し、単色は AddConvex/ConcavePolyFilled（AA 付き）、
// テクスチャは扇の三角形分割 + 基準矩形→UV の写像（=画像を形で切り抜く。AA なし）で描く。
// 放射 fill は「中心から境界への角度掃引」を同じポリゴン境界に対して行う（矩形にも効く）。

constexpr float kUiDeg2Rad = 3.14159265358979323846f / 180.0f;

// 角度（度。0=真上・時計回り正）→ スクリーン方向ベクトル（Y は下向き正）
ImVec2 UiAngleDir(float deg)
{
    const float r = deg * kUiDeg2Rad;
    return ImVec2(std::sin(r), -std::cos(r));
}

// shape の輪郭ポリゴン（時計回り・矩形 s に内接）。1=楕円(48分割) 3=ダイヤ 4=六角形(頂点が上)
// 5=三角形(上向き。向きは UIRect.rotation で変える) それ以外=矩形。リング(2)は別経路（円弧ストローク）
void BuildUiShapePoly(int shape, const UiRectPx& s, ImVector<ImVec2>& out)
{
    out.resize(0);
    const float cx = (s.minX + s.maxX) * 0.5f;
    const float cy = (s.minY + s.maxY) * 0.5f;
    const float hw = s.Width() * 0.5f;
    const float hh = s.Height() * 0.5f;
    switch (shape)
    {
    case 1:
        for (int i = 0; i < 48; ++i)
        {
            const ImVec2 d = UiAngleDir(static_cast<float>(i) * (360.0f / 48.0f));
            out.push_back(ImVec2(cx + d.x * hw, cy + d.y * hh));
        }
        break;
    case 3:
        out.push_back(ImVec2(cx, s.minY));
        out.push_back(ImVec2(s.maxX, cy));
        out.push_back(ImVec2(cx, s.maxY));
        out.push_back(ImVec2(s.minX, cy));
        break;
    case 4:
        for (int i = 0; i < 6; ++i)
        {
            const ImVec2 d = UiAngleDir(static_cast<float>(i) * 60.0f);
            out.push_back(ImVec2(cx + d.x * hw, cy + d.y * hh));
        }
        break;
    case 5:
        out.push_back(ImVec2(cx, s.minY));
        out.push_back(ImVec2(s.maxX, s.maxY));
        out.push_back(ImVec2(s.minX, s.maxY));
        break;
    default:
        out.push_back(ImVec2(s.minX, s.minY));
        out.push_back(ImVec2(s.maxX, s.minY));
        out.push_back(ImVec2(s.maxX, s.maxY));
        out.push_back(ImVec2(s.minX, s.maxY));
        break;
    }
}

// 凸ポリゴンを軸平行の半平面でその場クリップ（Sutherland–Hodgman の 1 辺ぶん。線形 fill 用）。
// axis: 0=x 1=y。keepLess=true なら「座標 <= limit」側を残す
void ClipUiPolyAxis(ImVector<ImVec2>& poly, int axis, bool keepLess, float limit)
{
    static ImVector<ImVec2> s_tmp;   // 描画パスは単一スレッド・再入なし
    s_tmp.resize(0);
    const int n = poly.Size;
    for (int i = 0; i < n; ++i)
    {
        const ImVec2 a = poly[i];
        const ImVec2 b = poly[(i + 1) % n];
        const float av = (axis == 0) ? a.x : a.y;
        const float bv = (axis == 0) ? b.x : b.y;
        const bool ain = keepLess ? (av <= limit) : (av >= limit);
        const bool bin = keepLess ? (bv <= limit) : (bv >= limit);
        if (ain)
            s_tmp.push_back(a);
        if (ain != bin && bv != av)
        {
            const float t = (limit - av) / (bv - av);
            s_tmp.push_back(ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t));
        }
    }
    poly = s_tmp;
}

// 中心 c から角度 deg 方向のレイと凸ポリゴン境界の交点（c はポリゴン内部前提）
ImVec2 UiPolyBoundaryPoint(const ImVector<ImVec2>& poly, const ImVec2& c, float deg)
{
    const ImVec2 d = UiAngleDir(deg);
    float bestT = 0.0f;
    const int n = poly.Size;
    for (int i = 0; i < n; ++i)
    {
        const ImVec2 a = poly[i];
        const ImVec2 b = poly[(i + 1) % n];
        const float ex = b.x - a.x, ey = b.y - a.y;
        const float den = d.x * ey - d.y * ex;   // cross(d, e)
        if (den > -1e-9f && den < 1e-9f) continue;
        const float acx = a.x - c.x, acy = a.y - c.y;
        const float t = (acx * ey - acy * ex) / den;   // cross(ac, e) / cross(d, e)
        const float u = (acx * d.y - acy * d.x) / den; // cross(ac, d) / cross(d, e)
        if (t > 0.0f && u >= -0.001f && u <= 1.001f && t > bestT)
            bestT = t;
    }
    return ImVec2(c.x + d.x * bestT, c.y + d.y * bestT);
}

// 放射 fill（扇形）ポリゴン: [0]=中心、以降が a0 から sweep 度ぶんの境界点列。
// 約 6 度刻み + 範囲内のポリゴン頂点角を挿入 = 矩形でも辺にぴったり沿う
void BuildUiPiePoly(const ImVector<ImVec2>& shapePoly, const ImVec2& c,
                    float a0, float sweep, ImVector<ImVec2>& out)
{
    out.resize(0);
    out.push_back(c);
    if (sweep == 0.0f) return;
    static ImVector<float> s_angles;
    s_angles.resize(0);
    const float a1 = a0 + sweep;
    const float lo = std::min(a0, a1), hi = std::max(a0, a1);
    const int steps = std::max(2, static_cast<int>(std::fabs(sweep) / 6.0f) + 1);
    for (int i = 0; i <= steps; ++i)
        s_angles.push_back(a0 + sweep * (static_cast<float>(i) / static_cast<float>(steps)));
    for (int i = 0; i < shapePoly.Size; ++i)
    {
        const float dx = shapePoly[i].x - c.x;
        const float dy = shapePoly[i].y - c.y;
        const float va = std::atan2(dx, -dy) / kUiDeg2Rad;   // 0=真上・時計回り
        for (float k = -720.0f; k <= 720.0f; k += 360.0f)
        {
            const float cand = va + k;
            if (cand > lo + 0.01f && cand < hi - 0.01f)
                s_angles.push_back(cand);
        }
    }
    std::sort(s_angles.begin(), s_angles.end());
    if (sweep < 0.0f)
        std::reverse(s_angles.begin(), s_angles.end());
    float prev = FLT_MAX;
    for (int i = 0; i < s_angles.Size; ++i)
    {
        if (std::fabs(s_angles[i] - prev) < 0.05f) continue;
        prev = s_angles[i];
        out.push_back(UiPolyBoundaryPoint(shapePoly, c, s_angles[i]));
    }
}

// テクスチャ付きポリゴン: pts[0] を扇の要とした三角形分割。UV は基準矩形 s → [uv0,uv1] の
// 写像（=画像を形で切り抜く）。凸形状（pts[0] が凸頂点）と扇形（pts[0]=中心）の両方に使える
void AddUiPolyTextured(ImDrawList* dl, ImTextureID texId, const ImVec2* pts, int n,
                       const UiRectPx& s, const ImVec2& uv0, const ImVec2& uv1, ImU32 col)
{
    if (n < 3) return;
    const float invW = 1.0f / std::max(1e-6f, s.Width());
    const float invH = 1.0f / std::max(1e-6f, s.Height());
    dl->PushTexture(texId);
    dl->PrimReserve((n - 2) * 3, n);
    const unsigned int base = dl->_VtxCurrentIdx;
    for (int i = 0; i < n; ++i)
    {
        const ImVec2 uv(uv0.x + (uv1.x - uv0.x) * (pts[i].x - s.minX) * invW,
                        uv0.y + (uv1.y - uv0.y) * (pts[i].y - s.minY) * invH);
        dl->PrimWriteVtx(pts[i], uv, col);
    }
    for (int i = 2; i < n; ++i)
    {
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + i - 1));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + i));
    }
    dl->PopTexture();
}

// 放射グラデ（gradientDir=4）: 中心からの距離 0..1 で col1→col2 の RGB を上書き（α は保持。
// ShadeVertsLinearColorGradientKeepAlpha の放射版）
void ShadeVertsRadial(ImDrawList* dl, int vtxStart, int vtxEnd,
                      const ImVec2& c, float maxDist, ImU32 col1, ImU32 col2)
{
    if (maxDist <= 0.0f) return;
    const ImVec4 c1 = ImGui::ColorConvertU32ToFloat4(col1);
    const ImVec4 c2 = ImGui::ColorConvertU32ToFloat4(col2);
    const float invD = 1.0f / maxDist;
    ImDrawVert* v = dl->VtxBuffer.Data;
    for (int i = vtxStart; i < vtxEnd; ++i)
    {
        ImDrawVert& vt = v[i];
        const float dx = vt.pos.x - c.x, dy = vt.pos.y - c.y;
        const float t = std::clamp(std::sqrt(dx * dx + dy * dy) * invD, 0.0f, 1.0f);
        ImVec4 cur = ImGui::ColorConvertU32ToFloat4(vt.col);
        cur.x = c1.x + (c2.x - c1.x) * t;
        cur.y = c1.y + (c2.y - c1.y) * t;
        cur.z = c1.z + (c2.z - c1.z) * t;
        vt.col = ImGui::ColorConvertFloat4ToU32(cur);
    }
}

// タイル描画: uv 範囲が [0,1] を超える場合に整数 UV 境界でクワッド分割して描く
// （バックエンドのサンプラが CLAMP のためジオメトリ側でリピートを再現）。uvScroll と併用で
// パターンが流れる。極端な繰り返し数(>256)は安全のため単一クワッドへフォールバック
void AddImageTiled(ImDrawList* dl, ImTextureID texId, const UiRectPx& s,
                   const ImVec2& uv0, const ImVec2& uv1, ImU32 col)
{
    const float spanU = uv1.x - uv0.x, spanV = uv1.y - uv0.y;
    if (spanU <= 0.0f || spanV <= 0.0f || spanU > 256.0f || spanV > 256.0f)
    {
        dl->AddImage(texId, ImVec2(s.minX, s.minY), ImVec2(s.maxX, s.maxY), uv0, uv1, col);
        return;
    }
    static ImVector<float> s_cutsU, s_cutsV;
    auto buildCuts = [](float a, float b, ImVector<float>& out) {
        out.resize(0);
        out.push_back(a);
        for (float k = std::floor(a) + 1.0f; k < b; k += 1.0f)
            if (k > a) out.push_back(k);
        out.push_back(b);
    };
    buildCuts(uv0.x, uv1.x, s_cutsU);
    buildCuts(uv0.y, uv1.y, s_cutsV);
    const float w = s.Width(), h = s.Height();
    for (int vi = 0; vi + 1 < s_cutsV.Size; ++vi)
    {
        const float v0 = s_cutsV[vi], v1 = s_cutsV[vi + 1];
        const float y0 = s.minY + (v0 - uv0.y) / spanV * h;
        const float y1 = s.minY + (v1 - uv0.y) / spanV * h;
        const float fv = std::floor(v0);
        for (int ui = 0; ui + 1 < s_cutsU.Size; ++ui)
        {
            const float u0 = s_cutsU[ui], u1 = s_cutsU[ui + 1];
            const float x0 = s.minX + (u0 - uv0.x) / spanU * w;
            const float x1 = s.minX + (u1 - uv0.x) / spanU * w;
            const float fu = std::floor(u0);
            dl->AddImage(texId, ImVec2(x0, y0), ImVec2(x1, y1),
                         ImVec2(u0 - fu, v0 - fv), ImVec2(u1 - fu, v1 - fv), col);
        }
    }
}

// 1 要素の描画とレイキャスト収集（入力の解決はトラバース完了後）。rect はキャンバス空間の解決済み矩形。
void DrawUiElement(entt::entity e, const UiRectPx& rect, UiDrawContext& ctx)
{
    auto& reg = *ctx.reg;
    const UiRectPx s = ToScreen(rect, ctx);
    const DirectX::XMFLOAT3& cm = ctx.colorMul;   // tween color フラッシュ（RGB 乗算・子孫継承）

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
            PushHitRect(ctx.buttonRects, e, s, ctx.hitClip, ctx);
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
        if (sld->interactable) PushHitRect(ctx.sliderRects, e, s, ctx.hitClip, ctx);
        else                   { sld->_hovered = false; sld->_dragging = false; }
    }
    if (tgl && ctx.interactive)
    {
        if (tgl->interactable) PushHitRect(ctx.toggleRects, e, s, ctx.hitClip, ctx);
        else                   { tgl->_hovered = false; tgl->_pressed = false; }
    }

    // クリックを遮る要素 = interactable なウィジェット（Button/Slider/Toggle）＋
    // raycastBlock=true の UIImage。UIText は遮らない（ラベルが親のクリックを妨げないように）。
    if (ctx.blockers)
    {
        const auto* imgBlock = reg.try_get<UIImage>(e);
        if ((btn && btn->interactable) || (sld && sld->interactable)
            || (tgl && tgl->interactable) || (imgBlock && imgBlock->raycastBlock))
            PushHitRect(ctx.blockers, e, s, ctx.hitClip, ctx);
    }

    // --- UIImage: テクスチャ / 単色を形状(shape)で描く。ボタンの状態色を乗算 ---
    if (const auto* img = reg.try_get<UIImage>(e))
    {
        // fillAmount: 表示割合 0..1（HPバー/ゲージ用）。幾何方式＝表示部分の形へ縮め、
        // テクスチャは UV も比例で切る（クリップ矩形方式とピクセル一致）。GPU シザーは
        // スクリーン軸固定なので、回転（頂点変換）と干渉しないようクリップには依存しない。
        // 0 は完全非表示（レイキャストは上の blockers 収集どおり効いたまま）。
        const float fill = std::clamp(img->fillAmount, 0.0f, 1.0f);
        if (fill > 0.0f)
        {
            const bool fillPartial = fill < 1.0f;
            const bool radialFill  = fillPartial && img->fillDir >= 4;   // 4=時計回り 5=反時計回り
            const int  shapeKind   = std::clamp(img->shape, 0, 5);
            const ImVec2 sc((s.minX + s.maxX) * 0.5f, (s.minY + s.maxY) * 0.5f);

            // 線形 fill の表示部分（スクリーン空間）。放射 fill / リングでは使わない
            UiRectPx fs = s;
            if (fillPartial && !radialFill)
            {
                switch (img->fillDir)
                {
                case 1:  fs.minX = s.maxX - s.Width() * fill;  break;   // 右から
                case 2:  fs.minY = s.maxY - s.Height() * fill; break;   // 下から
                case 3:  fs.maxY = s.minY + s.Height() * fill; break;   // 上から
                default: fs.maxX = s.minX + s.Width() * fill;  break;   // 0: 左から
                }
            }

            const DirectX::XMFLOAT4 col{img->color.x * tint.x * cm.x,
                                        img->color.y * tint.y * cm.y,
                                        img->color.z * tint.z * cm.z,
                                        img->color.w * tint.w * ctx.alphaMul};
            const ImU32 ucol = ToImCol(col);

            // テクスチャ解決（エディタアイコンと同じ経路: SRV index → GPU ハンドル(u64) =
            // ImTextureID。GetOrLoadTexture はパスキーでキャッシュされ毎フレーム安価）
            ImTextureID texId = 0;
            float texW = 0.0f, texH = 0.0f;
            if (!img->texturePath.empty() && ctx.resources && ctx.srvHeap && ctx.cmdList)
            {
                const std::string abs = PathResolver::AssetsDir() + img->texturePath;
                if (Texture* tex = ctx.resources->GetOrLoadTexture(
                        PathResolver::Utf8ToWide(abs), ctx.cmdList, true))
                {
                    texId = static_cast<ImTextureID>(
                        ctx.srvHeap->GetGpuHandle(tex->GetSrvIndex()).ptr);
                    texW = static_cast<float>(tex->GetWidth());
                    texH = static_cast<float>(tex->GetHeight());
                }
            }
            const bool hasTex = texId != 0;
            // 9-slice はテクスチャ付き矩形専用（放射 fill とは非両立 = その場合は平板扱い）
            const bool nineSlice = hasTex && shapeKind == 0 && !radialFill
                                   && (img->sliceBorder.x > 0.0f || img->sliceBorder.y > 0.0f
                                       || img->sliceBorder.z > 0.0f || img->sliceBorder.w > 0.0f);

            // UV（uvScroll 反映。9-slice では無効）
            ImVec2 uv0(img->uvMin.x, img->uvMin.y);
            ImVec2 uv1(img->uvMax.x, img->uvMax.y);
            if (!nineSlice && (img->uvScroll.x != 0.0f || img->uvScroll.y != 0.0f))
            {
                const double tm = ImGui::GetTime();
                const float sx = static_cast<float>(
                    tm * img->uvScroll.x - std::floor(tm * img->uvScroll.x));
                const float sy = static_cast<float>(
                    tm * img->uvScroll.y - std::floor(tm * img->uvScroll.y));
                uv0.x += sx; uv1.x += sx;
                uv0.y += sy; uv1.y += sy;
            }

            // 形状ポリゴン（矩形以外、または矩形への放射 fill で使う）
            static ImVector<ImVec2> s_shapePts, s_drawPts;
            const bool usePoly = shapeKind == 1 || shapeKind >= 3
                                 || (shapeKind == 0 && radialFill);
            if (usePoly)
                BuildUiShapePoly(shapeKind, s, s_shapePts);

            // --- ドロップシャドウ（本体より先に描く。フル形状の近似 = テクスチャのアルファ
            // 形状は反映しない。fillAmount<1 でもフル形状 = ゲージが減っても影は不変）---
            if (img->shadowColor.w > 0.0f)
            {
                const float offX = img->shadowOffset.x * ctx.scale;
                const float offY = img->shadowOffset.y * ctx.scale;
                const float soft = std::max(0.0f, img->shadowSoftness) * ctx.scale;
                const DirectX::XMFLOAT4 shBase{img->shadowColor.x * cm.x,
                                               img->shadowColor.y * cm.y,
                                               img->shadowColor.z * cm.z,
                                               img->shadowColor.w * ctx.alphaMul};
                static constexpr float kFall[4] = {0.42f, 0.24f, 0.12f, 0.05f};
                if (shapeKind == 2)
                {
                    // リング: 同じ帯円を 1 回だけオフセット描き（簡易近似）
                    const float th = std::max(1.0f, img->ringThickness * ctx.scale);
                    const float rx = std::max(0.5f, s.Width() * 0.5f - th * 0.5f);
                    const float ry = std::max(0.5f, s.Height() * 0.5f - th * 0.5f);
                    ctx.dl->PathEllipticalArcTo(ImVec2(sc.x + offX, sc.y + offY),
                                                ImVec2(rx, ry), 0.0f,
                                                0.0f, 2.0f * 3.14159265f, 0);
                    ctx.dl->PathStroke(ToImCol(shBase), 0, th);
                }
                else if (usePoly && shapeKind != 0)
                {
                    // 多角形: 中心拡張の層でソフト化（矩形の 4 層方式と同じ減衰）
                    const float hw = std::max(1e-3f, s.Width() * 0.5f);
                    const float hh = std::max(1e-3f, s.Height() * 0.5f);
                    const int layers = (soft < 0.5f) ? 1 : 4;
                    for (int li = 0; li < layers; ++li)
                    {
                        const float grow = (layers == 1)
                            ? 0.0f : soft * static_cast<float>(li + 1) / 4.0f;
                        const float kx = (hw + grow) / hw, ky = (hh + grow) / hh;
                        s_drawPts.resize(0);
                        for (int i = 0; i < s_shapePts.Size; ++i)
                            s_drawPts.push_back(
                                ImVec2(sc.x + (s_shapePts[i].x - sc.x) * kx + offX,
                                       sc.y + (s_shapePts[i].y - sc.y) * ky + offY));
                        DirectX::XMFLOAT4 lc = shBase;
                        if (layers == 4) lc.w *= kFall[li];
                        ctx.dl->AddConvexPolyFilled(s_drawPts.Data, s_drawPts.Size,
                                                    ToImCol(lc));
                    }
                }
                else if (soft < 0.5f)
                {
                    ctx.dl->AddRectFilled(ImVec2(s.minX + offX, s.minY + offY),
                                          ImVec2(s.maxX + offX, s.maxY + offY),
                                          ToImCol(shBase), img->cornerRadius * ctx.scale);
                }
                else
                {
                    // 4 層を外へ広げつつアルファ減衰 = 疑似ソフトシャドウ
                    const float baseRad = img->cornerRadius * ctx.scale;
                    for (int i = 0; i < 4; ++i)
                    {
                        const float grow = soft * static_cast<float>(i + 1) / 4.0f;
                        DirectX::XMFLOAT4 lc = shBase;
                        lc.w *= kFall[i];
                        ctx.dl->AddRectFilled(
                            ImVec2(s.minX + offX - grow, s.minY + offY - grow),
                            ImVec2(s.maxX + offX + grow, s.maxY + offY + grow),
                            ToImCol(lc), baseRad + grow);
                    }
                }
            }

            // グラデーション: 影の後・本体の前から頂点範囲を記録し、本体描画後に
            // シェードを後がけする（テクスチャ/9-slice/角丸/形状すべて対応、回転にも追従）
            const int gradStart = (img->gradientDir > 0) ? ctx.dl->VtxBuffer.Size : -1;

            // --- 本体 ---
            if (shapeKind == 2)
            {
                // リング(帯円)。fill は常に円弧: fillOrigin から fill*360 度
                // （fillDir=5 で反時計回り。0-3 の線形方向指定でも円弧として扱う）
                const float th = std::max(1.0f, img->ringThickness * ctx.scale);
                const float rx = std::max(0.5f, s.Width() * 0.5f - th * 0.5f);
                const float ry = std::max(0.5f, s.Height() * 0.5f - th * 0.5f);
                const float sweep = 360.0f * fill * ((img->fillDir == 5) ? -1.0f : 1.0f);
                const float aA = (img->fillOrigin - 90.0f) * kUiDeg2Rad;   // ImGui は +X 基準
                const float aB = aA + sweep * kUiDeg2Rad;
                ctx.dl->PathEllipticalArcTo(sc, ImVec2(rx, ry), 0.0f,
                                            std::min(aA, aB), std::max(aA, aB), 0);
                ctx.dl->PathStroke(ucol, 0, th);
            }
            else if (usePoly)
            {
                if (radialFill)
                {
                    // 放射 fill: クールダウン円/矩形ワイプ。fillOrigin から掃引
                    const float sweep = 360.0f * fill * ((img->fillDir == 5) ? -1.0f : 1.0f);
                    BuildUiPiePoly(s_shapePts, sc, img->fillOrigin, sweep, s_drawPts);
                }
                else
                {
                    s_drawPts = s_shapePts;
                    if (fillPartial)
                    {
                        switch (img->fillDir)
                        {
                        case 1:  ClipUiPolyAxis(s_drawPts, 0, false, fs.minX); break;
                        case 2:  ClipUiPolyAxis(s_drawPts, 1, false, fs.minY); break;
                        case 3:  ClipUiPolyAxis(s_drawPts, 1, true,  fs.maxY); break;
                        default: ClipUiPolyAxis(s_drawPts, 0, true,  fs.maxX); break;
                        }
                    }
                }
                if (s_drawPts.Size >= 3)
                {
                    if (hasTex)
                        AddUiPolyTextured(ctx.dl, texId, s_drawPts.Data, s_drawPts.Size,
                                          s, uv0, uv1, ucol);
                    else if (radialFill)   // 扇形は 180 度超で凹になる
                        ctx.dl->AddConcavePolyFilled(s_drawPts.Data, s_drawPts.Size, ucol);
                    else
                        ctx.dl->AddConvexPolyFilled(s_drawPts.Data, s_drawPts.Size, ucol);
                }
            }
            else if (nineSlice)
            {
                AddImage9Slice(ctx.dl, texId, ImVec2(s.minX, s.minY), ImVec2(s.maxX, s.maxY),
                               uv0, uv1, texW, texH, img->sliceBorder, ctx.scale, ucol,
                               fillPartial ? &fs : nullptr);
            }
            else if (hasTex)
            {
                // 平板: 表示部分に合わせて UV を比例クロップ。範囲が [0,1] を超えるなら
                // タイル分割（uvMax>1 のパターン繰り返し / uvScroll の流し）
                const float w = std::max(1e-6f, s.Width());
                const float h = std::max(1e-6f, s.Height());
                const ImVec2 fuv0(uv0.x + (uv1.x - uv0.x) * (fs.minX - s.minX) / w,
                                  uv0.y + (uv1.y - uv0.y) * (fs.minY - s.minY) / h);
                const ImVec2 fuv1(uv1.x - (uv1.x - uv0.x) * (s.maxX - fs.maxX) / w,
                                  uv1.y - (uv1.y - uv0.y) * (s.maxY - fs.maxY) / h);
                if (fuv0.x < 0.0f || fuv0.y < 0.0f || fuv1.x > 1.0f || fuv1.y > 1.0f)
                    AddImageTiled(ctx.dl, texId, fs, fuv0, fuv1, ucol);
                else
                    ctx.dl->AddImage(texId, ImVec2(fs.minX, fs.minY), ImVec2(fs.maxX, fs.maxY),
                                     fuv0, fuv1, ucol);
            }
            else
            {
                // 単色矩形（角丸対応。テクスチャロード失敗時も要素が見えるようにする）
                const float rad = img->cornerRadius * ctx.scale;
                if (fillPartial && rad > 0.0f)
                {
                    if (ctx.hasXform)
                    {
                        // 回転/スキュー時: GPU シザーは頂点変形後にスクリーン軸で切って
                        // しまうため、縮んだ角丸矩形で代用（切り口も丸くなる既知の制限）
                        ctx.dl->AddRectFilled(ImVec2(fs.minX, fs.minY), ImVec2(fs.maxX, fs.maxY),
                                              ucol, rad);
                    }
                    else
                    {
                        // 角丸 + 部分 fill のみクリップ方式を温存（丸みの見た目維持）
                        ctx.dl->PushClipRect(ImVec2(fs.minX, fs.minY), ImVec2(fs.maxX, fs.maxY),
                                             true);
                        ctx.dl->AddRectFilled(ImVec2(s.minX, s.minY), ImVec2(s.maxX, s.maxY),
                                              ucol, rad);
                        ctx.dl->PopClipRect();
                    }
                }
                else
                {
                    ctx.dl->AddRectFilled(ImVec2(fs.minX, fs.minY), ImVec2(fs.maxX, fs.maxY), ucol,
                                          fillPartial ? 0.0f : rad);
                }
            }

            // --- グラデーション後がけ（端点は常にフル矩形 = ゲージが減っても色が潰れない）---
            if (gradStart >= 0 && ctx.dl->VtxBuffer.Size > gradStart)
            {
                // 終端色にも tint / 本体アルファを乗算（KeepAlpha = 頂点のアルファは保持され、
                // gradientColor2 のアルファは使われない）
                const DirectX::XMFLOAT4 col2{img->gradientColor2.x * tint.x * cm.x,
                                             img->gradientColor2.y * tint.y * cm.y,
                                             img->gradientColor2.z * tint.z * cm.z, col.w};
                if (img->gradientDir == 4)
                {
                    // 放射: 中心 → 形状の最遠点（スポットライト/ビネット風）
                    float maxD = 0.0f;
                    if (usePoly && shapeKind != 0)
                    {
                        for (int i = 0; i < s_shapePts.Size; ++i)
                        {
                            const float dx = s_shapePts[i].x - sc.x;
                            const float dy = s_shapePts[i].y - sc.y;
                            maxD = std::max(maxD, dx * dx + dy * dy);
                        }
                        maxD = std::sqrt(maxD);
                    }
                    else
                    {
                        const float hw = s.Width() * 0.5f, hh = s.Height() * 0.5f;
                        maxD = std::sqrt(hw * hw + hh * hh);
                    }
                    ShadeVertsRadial(ctx.dl, gradStart, ctx.dl->VtxBuffer.Size, sc, maxD,
                                     ucol, ToImCol(col2));
                }
                else
                {
                    ImVec2 p0(s.minX, s.minY), p1;
                    switch (img->gradientDir)
                    {
                    case 2:  p1 = ImVec2(s.minX, s.maxY); break;   // 縦（上→下）
                    case 3:  p1 = ImVec2(s.maxX, s.maxY); break;   // 斜め（左上→右下）
                    default: p1 = ImVec2(s.maxX, s.minY); break;   // 1: 横（左→右）
                    }
                    if (img->gradientScrollSpeed != 0.0f)
                    {
                        // グロススイープ: 静的グラデの代わりに gradientColor2 の光帯が
                        // gradientDir 方向へ周期的に流れる（負の速度で逆方向）
                        const double cyc = ImGui::GetTime()
                                           * static_cast<double>(img->gradientScrollSpeed);
                        float cycle01 = static_cast<float>(cyc - std::floor(cyc));
                        ShadeVertsGlossBand(ctx.dl, gradStart, ctx.dl->VtxBuffer.Size,
                                            p0, p1, col2, cycle01);
                    }
                    else
                    {
                        ImGui::ShadeVertsLinearColorGradientKeepAlpha(
                            ctx.dl, gradStart, ctx.dl->VtxBuffer.Size, p0, p1, ucol,
                            ToImCol(col2));
                    }
                }
            }

            // --- セグメント区切り（矩形 + 線形 fill 専用の分割ゲージ。表示部分にだけ重ねる）---
            if (img->segments > 1 && shapeKind == 0 && img->fillDir <= 3)
            {
                const int nSeg = std::min(img->segments, 64);
                const float gw = std::max(1.0f, img->segmentGap * ctx.scale);
                const ImU32 segCol = ToImCol({img->segmentColor.x * cm.x,
                                              img->segmentColor.y * cm.y,
                                              img->segmentColor.z * cm.z,
                                              img->segmentColor.w * ctx.alphaMul});
                const bool horiz = img->fillDir <= 1;
                for (int i = 1; i < nSeg; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(nSeg);
                    if (horiz)
                    {
                        const float x = s.minX + s.Width() * t;
                        if (x < fs.minX - 0.5f || x > fs.maxX + 0.5f) continue;
                        ctx.dl->AddRectFilled(ImVec2(x - gw * 0.5f, fs.minY),
                                              ImVec2(x + gw * 0.5f, fs.maxY), segCol);
                    }
                    else
                    {
                        const float y = s.minY + s.Height() * t;
                        if (y < fs.minY - 0.5f || y > fs.maxY + 0.5f) continue;
                        ctx.dl->AddRectFilled(ImVec2(fs.minX, y - gw * 0.5f),
                                              ImVec2(fs.maxX, y + gw * 0.5f), segCol);
                    }
                }
            }

            // --- 縁取り（フル形状に描く = ゲージ減少に影響されない）---
            if (img->outlineWidth > 0.0f && img->outlineColor.w > 0.0f)
            {
                const ImU32 ocol = ToImCol({img->outlineColor.x * cm.x,
                                            img->outlineColor.y * cm.y,
                                            img->outlineColor.z * cm.z,
                                            img->outlineColor.w * ctx.alphaMul});
                const float ow = std::max(1.0f, img->outlineWidth * ctx.scale);
                if (shapeKind == 2)
                {
                    // リング: 外周 + 内周の 2 本
                    const float th = std::max(1.0f, img->ringThickness * ctx.scale);
                    const float rxO = std::max(0.5f, s.Width() * 0.5f);
                    const float ryO = std::max(0.5f, s.Height() * 0.5f);
                    ctx.dl->AddEllipse(sc, ImVec2(rxO, ryO), ocol, 0.0f, 0, ow);
                    ctx.dl->AddEllipse(sc, ImVec2(std::max(0.5f, rxO - th),
                                                  std::max(0.5f, ryO - th)),
                                       ocol, 0.0f, 0, ow);
                }
                else if (usePoly && shapeKind != 0)
                {
                    ctx.dl->AddPolyline(s_shapePts.Data, s_shapePts.Size, ocol,
                                        ImDrawFlags_Closed, ow);
                }
                else if (img->outlineStyle == 1)
                {
                    // 破線（cornerRadius は無視）
                    const float dash = std::max(2.0f, img->outlineDash * ctx.scale);
                    const float gap = dash * 0.6f;
                    auto dashedLine = [&](float ax, float ay, float bx, float by) {
                        const float dx = bx - ax, dy = by - ay;
                        const float len = std::sqrt(dx * dx + dy * dy);
                        if (len <= 0.0f) return;
                        const float ux = dx / len, uy = dy / len;
                        for (float d0 = 0.0f; d0 < len; d0 += dash + gap)
                        {
                            const float d1 = std::min(d0 + dash, len);
                            ctx.dl->AddLine(ImVec2(ax + ux * d0, ay + uy * d0),
                                            ImVec2(ax + ux * d1, ay + uy * d1), ocol, ow);
                        }
                    };
                    dashedLine(s.minX, s.minY, s.maxX, s.minY);
                    dashedLine(s.maxX, s.minY, s.maxX, s.maxY);
                    dashedLine(s.maxX, s.maxY, s.minX, s.maxY);
                    dashedLine(s.minX, s.maxY, s.minX, s.minY);
                }
                else if (img->outlineStyle == 2)
                {
                    // コーナーブラケット（四隅の鉤括弧。SF/ターゲット HUD 定番。cornerRadius 無視）
                    const float L = std::min({std::max(2.0f, img->outlineDash * ctx.scale),
                                              s.Width() * 0.5f, s.Height() * 0.5f});
                    auto arm = [&](float ax, float ay, float bx, float by) {
                        ctx.dl->AddLine(ImVec2(ax, ay), ImVec2(bx, by), ocol, ow);
                    };
                    arm(s.minX, s.minY, s.minX + L, s.minY);
                    arm(s.minX, s.minY, s.minX, s.minY + L);
                    arm(s.maxX, s.minY, s.maxX - L, s.minY);
                    arm(s.maxX, s.minY, s.maxX, s.minY + L);
                    arm(s.maxX, s.maxY, s.maxX - L, s.maxY);
                    arm(s.maxX, s.maxY, s.maxX, s.maxY - L);
                    arm(s.minX, s.maxY, s.minX + L, s.maxY);
                    arm(s.minX, s.maxY, s.minX, s.maxY - L);
                }
                else
                {
                    // 実線（角丸に追従）
                    ctx.dl->AddRect(ImVec2(s.minX, s.minY), ImVec2(s.maxX, s.maxY), ocol,
                                    img->cornerRadius * ctx.scale, 0, ow);
                }
            }
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
            return ToImCol({c.x * mul * cm.x, c.y * mul * cm.y, c.z * mul * cm.z,
                            c.w * ctx.alphaMul});
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
                              ToImCol({tgl->boxColor.x * mul * cm.x, tgl->boxColor.y * mul * cm.y,
                                       tgl->boxColor.z * mul * cm.z,
                                       tgl->boxColor.w * ctx.alphaMul}),
                              round);
        if (tgl->isOn)
        {
            const float inset = std::min(s.Width(), s.Height()) * 0.22f;
            ctx.dl->AddRectFilled(ImVec2(s.minX + inset, s.minY + inset),
                                  ImVec2(s.maxX - inset, s.maxY - inset),
                                  ToImCol({tgl->checkColor.x * cm.x, tgl->checkColor.y * cm.y,
                                           tgl->checkColor.z * cm.z,
                                           tgl->checkColor.w * ctx.alphaMul}),
                                  round * 0.6f);
        }
    }

    // --- UIText: 整列 + 折り返し（ImGui 共有フォントのスケール描画。ボタンティントは掛けない）---
    if (const auto* txt = reg.try_get<UIText>(e))
    {
        if (!txt->text.empty() && txt->color.w * ctx.alphaMul > 0.0f)
        {
            ImFont* font = ImGui::GetFont();
            if (!txt->fontPath.empty())
                if (ImFont* custom = GetOrLoadUiFont(txt->fontPath))
                    font = custom;
            const float fontSize = std::max(1.0f, txt->fontSize * ctx.scale);
            const float wrapW = txt->wrap ? std::max(1.0f, s.Width()) : 0.0f;
            // per-glyph モード（字間 / 文字アニメ）。wrap とは非両立 = wrap 優先で通常描画
            const bool perGlyph = !txt->wrap
                                  && (txt->letterSpacing != 0.0f || txt->charAnim != 0);
            const float sp = txt->letterSpacing * ctx.scale;
            auto utf8Adv = [](const char* p) {
                const auto c = static_cast<unsigned char>(*p);
                return (c < 0xC0) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            };
            ImVec2 ts;
            if (perGlyph)
            {
                // 1 文字ずつ測って字間込みの行幅を出す（\n 対応。行高 = fontSize）
                const char* p = txt->text.c_str();
                const char* pAll = p + txt->text.size();
                float lw = 0.0f, maxW = 0.0f;
                int lines = 1, chars = 0;
                while (p < pAll)
                {
                    if (*p == '\n')
                    {
                        maxW = std::max(maxW, (chars > 0) ? std::max(0.0f, lw - sp) : 0.0f);
                        lw = 0.0f; chars = 0; ++lines; ++p;
                        continue;
                    }
                    const char* pn = std::min(p + utf8Adv(p), pAll);
                    lw += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, p, pn).x + sp;
                    ++chars;
                    p = pn;
                }
                maxW = std::max(maxW, (chars > 0) ? std::max(0.0f, lw - sp) : 0.0f);
                ts = ImVec2(maxW, fontSize * static_cast<float>(lines));
            }
            else
            {
                ts = font->CalcTextSizeA(fontSize, FLT_MAX, wrapW, txt->text.c_str());
            }
            // ブロック単位の整列（CalcTextSizeA は折り返し後の全体サイズを返す）
            float tx = s.minX;
            if (txt->alignH == 1)      tx = s.minX + (s.Width() - ts.x) * 0.5f;
            else if (txt->alignH == 2) tx = s.maxX - ts.x;
            float ty = s.minY;
            if (txt->alignV == 1)      ty = s.minY + (s.Height() - ts.y) * 0.5f;
            else if (txt->alignV == 2) ty = s.maxY - ts.y;
            const DirectX::XMFLOAT4 tcol{txt->color.x * cm.x, txt->color.y * cm.y,
                                         txt->color.z * cm.z,
                                         txt->color.w * ctx.alphaMul};

            // タイプライター: 先頭 N 文字だけ描く（UTF-8 コードポイント単位 = マルチバイト
            // 文字の途中で切らない）。整列(ts)は全文サイズで固定 = 表示が進んでも文字が
            // ずれない。エディタプレビュー(interactive=false)は常に全文
            const char* textBegin = txt->text.c_str();
            const char* textEnd   = nullptr;   // nullptr = 全文
            if (ctx.interactive && txt->typewriterSpeed > 0.0f)
            {
                const char* pAll = textBegin + txt->text.size();
                const char* pCur = textBegin;
                float remain = txt->_twT * txt->typewriterSpeed;
                while (pCur < pAll && remain >= 1.0f)
                {
                    pCur = std::min(pCur + utf8Adv(pCur), pAll);
                    remain -= 1.0f;
                }
                textEnd = pCur;
            }
            if (textEnd == textBegin)
            {
                // タイプライターでまだ 1 文字も出ていない
            }
            else if (perGlyph)
            {
                // per-glyph: 1 文字ずつ配置して字間/ウェーブ/ジッター/レインボーを掛ける。
                // 影 → 縁取り → 本体の 3 パスで層順を保つ（本体パスだけグラデ対象）
                struct UiGlyphDraw { const char* b; const char* e; float x, y; ImU32 col; };
                static ImVector<UiGlyphDraw> s_glyphs;
                s_glyphs.resize(0);
                const float animT = static_cast<float>(ImGui::GetTime());
                const float amp = txt->charAnimAmount * ctx.scale;
                const char* pStop = textEnd ? textEnd : (textBegin + txt->text.size());
                const char* p = textBegin;
                float gx = tx, gy = ty;
                int idx = 0;
                while (p < pStop)
                {
                    if (*p == '\n')
                    {
                        gx = tx; gy += fontSize; ++p;
                        continue;
                    }
                    const char* pn = std::min(p + utf8Adv(p), pStop);
                    const float w = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, p, pn).x;
                    float dx = 0.0f, dy = 0.0f;
                    ImU32 bodyCol = ToImCol(tcol);
                    if (txt->charAnim == 1)        // ウェーブ（上下うねり）
                    {
                        dy = std::sin(animT * txt->charAnimSpeed * 6.2831853f
                                      + static_cast<float>(idx) * 0.55f) * amp;
                    }
                    else if (txt->charAnim == 2)   // ジッター（時間量子化ハッシュで震え）
                    {
                        const int q = static_cast<int>(
                            animT * std::max(0.1f, txt->charAnimSpeed) * 8.0f);
                        unsigned int h = static_cast<unsigned int>(idx) * 374761393u
                                         + static_cast<unsigned int>(q) * 668265263u;
                        h = (h ^ (h >> 13)) * 1274126177u;
                        dx = (static_cast<float>(h & 0xFF) / 255.0f - 0.5f) * amp;
                        dy = (static_cast<float>((h >> 8) & 0xFF) / 255.0f - 0.5f) * amp;
                    }
                    else if (txt->charAnim == 3)   // レインボー（文字ごとに色相をずらして回す）
                    {
                        float r, g, b;
                        const float hue = animT * txt->charAnimSpeed * 0.25f
                                          + static_cast<float>(idx) * 0.09f;
                        ImGui::ColorConvertHSVtoRGB(hue - std::floor(hue), 0.75f, 1.0f, r, g, b);
                        bodyCol = ToImCol({r * cm.x, g * cm.y, b * cm.z,
                                           txt->color.w * ctx.alphaMul});
                    }
                    s_glyphs.push_back({p, pn, gx + dx, gy + dy, bodyCol});
                    gx += w + sp;
                    p = pn;
                    ++idx;
                }
                if (txt->shadowColor.w > 0.0f)
                {
                    const ImU32 shCol = ToImCol({txt->shadowColor.x * cm.x,
                                                 txt->shadowColor.y * cm.y,
                                                 txt->shadowColor.z * cm.z,
                                                 txt->shadowColor.w * ctx.alphaMul});
                    const float ox = txt->shadowOffset.x * ctx.scale;
                    const float oy = txt->shadowOffset.y * ctx.scale;
                    for (int i = 0; i < s_glyphs.Size; ++i)
                        ctx.dl->AddText(font, fontSize,
                                        ImVec2(s_glyphs[i].x + ox, s_glyphs[i].y + oy),
                                        shCol, s_glyphs[i].b, s_glyphs[i].e);
                }
                if (txt->outlineWidth > 0.0f && txt->outlineColor.w > 0.0f)
                {
                    const float ow = std::max(1.0f, txt->outlineWidth * ctx.scale);
                    const ImU32 ocol = ToImCol({txt->outlineColor.x * cm.x,
                                                txt->outlineColor.y * cm.y,
                                                txt->outlineColor.z * cm.z,
                                                txt->outlineColor.w * ctx.alphaMul});
                    static constexpr float kDirs[8][2] = {
                        {-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f},
                        {-0.7071f, -0.7071f}, {0.7071f, -0.7071f},
                        {-0.7071f, 0.7071f},  {0.7071f, 0.7071f}};
                    for (const auto& d : kDirs)
                        for (int i = 0; i < s_glyphs.Size; ++i)
                            ctx.dl->AddText(font, fontSize,
                                            ImVec2(s_glyphs[i].x + d[0] * ow,
                                                   s_glyphs[i].y + d[1] * ow),
                                            ocol, s_glyphs[i].b, s_glyphs[i].e);
                }
                const int gStart = (txt->gradientDir > 0 && txt->charAnim != 3)
                                       ? ctx.dl->VtxBuffer.Size : -1;
                for (int i = 0; i < s_glyphs.Size; ++i)
                    ctx.dl->AddText(font, fontSize, ImVec2(s_glyphs[i].x, s_glyphs[i].y),
                                    s_glyphs[i].col, s_glyphs[i].b, s_glyphs[i].e);
                if (gStart >= 0 && ctx.dl->VtxBuffer.Size > gStart)
                {
                    const ImVec2 p0(tx, ty);
                    const ImVec2 p1 = (txt->gradientDir == 2) ? ImVec2(tx, ty + ts.y)
                                                              : ImVec2(tx + ts.x, ty);
                    ImGui::ShadeVertsLinearColorGradientKeepAlpha(
                        ctx.dl, gStart, ctx.dl->VtxBuffer.Size, p0, p1, ToImCol(tcol),
                        ToImCol({txt->gradientColor2.x * cm.x, txt->gradientColor2.y * cm.y,
                                 txt->gradientColor2.z * cm.z, tcol.w}));
                }
            }
            else
            {
                // 影（1 オフセットの先描き）→ 縁取り（8 方位の重ね描き）→ 本体。
                // 全部このサブツリーの頂点キャプチャ内なので回転にも一緒に追従する
                if (txt->shadowColor.w > 0.0f)
                {
                    ctx.dl->AddText(font, fontSize,
                                    ImVec2(tx + txt->shadowOffset.x * ctx.scale,
                                           ty + txt->shadowOffset.y * ctx.scale),
                                    ToImCol({txt->shadowColor.x * cm.x, txt->shadowColor.y * cm.y,
                                             txt->shadowColor.z * cm.z,
                                             txt->shadowColor.w * ctx.alphaMul}),
                                    textBegin, textEnd, wrapW);
                }
                if (txt->outlineWidth > 0.0f && txt->outlineColor.w > 0.0f)
                {
                    const float ow = std::max(1.0f, txt->outlineWidth * ctx.scale);
                    const ImU32 ocol = ToImCol({txt->outlineColor.x * cm.x,
                                                txt->outlineColor.y * cm.y,
                                                txt->outlineColor.z * cm.z,
                                                txt->outlineColor.w * ctx.alphaMul});
                    static constexpr float kDirs[8][2] = {
                        {-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f},
                        {-0.7071f, -0.7071f}, {0.7071f, -0.7071f},
                        {-0.7071f, 0.7071f},  {0.7071f, 0.7071f}};
                    for (const auto& d : kDirs)
                        ctx.dl->AddText(font, fontSize,
                                        ImVec2(tx + d[0] * ow, ty + d[1] * ow), ocol,
                                        textBegin, textEnd, wrapW);
                }
                // テキストグラデ（本体のみ。影/縁取りには掛けない）
                const int gStart = (txt->gradientDir > 0) ? ctx.dl->VtxBuffer.Size : -1;
                ctx.dl->AddText(font, fontSize, ImVec2(tx, ty), ToImCol(tcol),
                                textBegin, textEnd, wrapW);
                if (gStart >= 0 && ctx.dl->VtxBuffer.Size > gStart)
                {
                    const ImVec2 p0(tx, ty);
                    const ImVec2 p1 = (txt->gradientDir == 2) ? ImVec2(tx, ty + ts.y)
                                                              : ImVec2(tx + ts.x, ty);
                    ImGui::ShadeVertsLinearColorGradientKeepAlpha(
                        ctx.dl, gStart, ctx.dl->VtxBuffer.Size, p0, p1, ToImCol(tcol),
                        ToImCol({txt->gradientColor2.x * cm.x, txt->gradientColor2.y * cm.y,
                                 txt->gradientColor2.z * cm.z, tcol.w}));
                }
            }
        }
    }
}

void DrawUiSubtree(entt::entity e, const UiRectPx& parentRect, UiDrawContext& ctx);

// 直下の子への再帰。UILayout があれば子（UIRect 持ち）へセル矩形を順に配る
// （VBox/HBox/Grid。子は配られたセルを親矩形としてアンカー解決する = 子の書式はそのまま）。
// UIRect を持たない子はレイアウトを消費せず親矩形を素通しする。
void DrawUiChildren(entt::entity e, const UiRectPx& parentRect, UiDrawContext& ctx)
{
    auto it = ctx.children->find(e);
    if (it == ctx.children->end()) return;
    const auto* lay = ctx.reg->try_get<UILayout>(e);
    if (!lay || !ctx.reg->all_of<UIRect>(e))
    {
        for (entt::entity child : it->second)
            DrawUiSubtree(child, parentRect, ctx);
        return;
    }
    UiRectPx inner = parentRect;
    inner.minX += lay->padding.x;
    inner.minY += lay->padding.y;
    inner.maxX -= lay->padding.z;
    inner.maxY -= lay->padding.w;
    const float spacing = lay->spacing;
    const int   cols = std::max(1, lay->gridCols);
    int idx = 0;
    for (entt::entity child : it->second)
    {
        if (!ctx.reg->all_of<UIRect>(child))
        {
            DrawUiSubtree(child, parentRect, ctx);
            continue;
        }
        // セルサイズ: 積む軸は必ず正、交差軸は 0 以下でインナーいっぱい
        UiRectPx cell = inner;
        switch (lay->mode)
        {
        case 1:   // HBox（横並び）
        {
            const float cw = std::max(1.0f, lay->cellW);
            const float ch = (lay->cellH > 0.0f) ? lay->cellH : inner.Height();
            cell.minX = inner.minX + static_cast<float>(idx) * (cw + spacing);
            cell.maxX = cell.minX + cw;
            cell.maxY = cell.minY + ch;
            break;
        }
        case 2:   // Grid（左上から行優先）
        {
            const float cw = std::max(1.0f, lay->cellW);
            const float ch = std::max(1.0f, lay->cellH);
            cell.minX = inner.minX + static_cast<float>(idx % cols) * (cw + spacing);
            cell.minY = inner.minY + static_cast<float>(idx / cols) * (ch + spacing);
            cell.maxX = cell.minX + cw;
            cell.maxY = cell.minY + ch;
            break;
        }
        default:  // VBox（縦積み）
        {
            const float cw = (lay->cellW > 0.0f) ? lay->cellW : inner.Width();
            const float ch = std::max(1.0f, lay->cellH);
            cell.minY = inner.minY + static_cast<float>(idx) * (ch + spacing);
            cell.maxY = cell.minY + ch;
            cell.maxX = cell.minX + cw;
            break;
        }
        }
        ++idx;
        DrawUiSubtree(child, cell, ctx);
    }
}

// 親→子の DFS。UIRect を持つノードは矩形を解決して描画し、持たないノードは親矩形を素通しする。
// visible=false の UIRect はサブツリーごとスキップ。兄弟順は children 構築時の走査順
// （HierarchyPanel と同じ view<Transform> の順序）。
void DrawUiSubtree(entt::entity e, const UiRectPx& parentRect, UiDrawContext& ctx)
{
    UiRectPx current = parentRect;
    const float parentAlpha = ctx.alphaMul;
    const DirectX::XMFLOAT3 parentColor = ctx.colorMul;
    const bool       parentHasXform = ctx.hasXform;   // 回転/スキューの push/pop（alphaMul と同じ規律）
    const UiXform2x3 parentXform    = ctx.xform;
    const UiXform2x3 parentInv      = ctx.invXform;
    int        vtxStart = -1;   // >=0 なら関数末尾で [vtxStart, Size) へ local 変形を後がけ
    UiXform2x3 localXform;
    if (const auto* rect = ctx.reg->try_get<UIRect>(e))
    {
        if (!rect->visible) return;
        current = ResolveUiRect(*rect, parentRect);

        // --- UIAnimator / tween の視覚エフェクト（Play / ゲームモード中のみ）---
        // 矩形をキャンバス空間で変形するので、子孫のレイアウト解決・レイキャスト矩形にも
        // そのまま効く（ボタンごと動く/縮む）。アルファは ctx.alphaMul で子孫へ継承。
        // 回転（_curRot）は下の回転/スキューブロックで UIRect.rotation へ加算合成する。
        float fxRot = 0.0f;
        if (ctx.interactive)
        {
            float fxScaleX = 1.0f, fxScaleY = 1.0f, fxAlpha = 1.0f, fxOffX = 0.0f, fxOffY = 0.0f;
            if (const auto* an = ctx.reg->try_get<UIAnimator>(e))
            {
                if (an->_mode == 4)
                {
                    ctx.alphaMul = parentAlpha;
                    return;   // hideUi 完了 = サブツリーごと非表示（クリックも通らない）
                }
                fxScaleX *= an->_curScaleX;
                fxScaleY *= an->_curScaleY;
                fxAlpha  *= an->_curAlpha;
                fxRot    += an->_curRot;
                fxOffX   += an->_curOffset.x;
                fxOffY   += an->_curOffset.y;
            }
            if (const auto* tw = ctx.reg->try_get<UITweenState>(e))
            {
                fxScaleX *= tw->_curScaleX;
                fxScaleY *= tw->_curScaleY;
                fxAlpha  *= tw->_curAlpha;
                fxRot    += tw->_curRot;
                fxOffX   += tw->_curOffset.x;   // shake
                fxOffY   += tw->_curOffset.y;
                ctx.colorMul = {parentColor.x * tw->_curColor.x,
                                parentColor.y * tw->_curColor.y,
                                parentColor.z * tw->_curColor.z};
            }
            if (fxScaleX != 1.0f || fxScaleY != 1.0f || fxOffX != 0.0f || fxOffY != 0.0f)
            {
                const float cx = (current.minX + current.maxX) * 0.5f + fxOffX;
                const float cy = (current.minY + current.maxY) * 0.5f + fxOffY;
                const float hw = current.Width()  * 0.5f * fxScaleX;
                const float hh = current.Height() * 0.5f * fxScaleY;
                current = {cx - hw, cy - hh, cx + hw, cy + hh};
            }
            ctx.alphaMul = parentAlpha * fxAlpha;
        }

        // --- 回転/スキュー（視覚変換。レイアウトは AABB のまま）---
        // pivot はエフェクト適用後の矩形のスクリーン座標で取る。UIScrollView ノード自身は
        // 軸平行 GPU シザーと両立できないため無視（Inspector にも注記）。
        // interactive ゲート外 = エディタプレビュー（RenderPreview）でも見える静的属性。
        {
            float rotDeg  = rect->rotation + fxRot;   // 静的回転 + Animator/tween の追加回転（度の加算 = 可換）
            float skewDeg = rect->skewX;
            if (ctx.reg->all_of<UIScrollView>(e) || rect->clipChildren)
            {
                // 軸平行 GPU シザー（スクロール/マスク）と回転は両立しないため無効化
                rotDeg  = 0.0f;
                skewDeg = 0.0f;
            }
            if (rotDeg != 0.0f || skewDeg != 0.0f)
            {
                const UiRectPx sp = ToScreen(current, ctx);
                localXform = MakeUiRotSkew(rotDeg, skewDeg,
                                           sp.minX + sp.Width()  * rect->pivot.x,
                                           sp.minY + sp.Height() * rect->pivot.y);
                ctx.xform    = UiXformCombine(parentXform, localXform);
                ctx.invXform = ctx.xform.Inverted();
                ctx.hasXform = true;
                if (ctx.dl)
                    vtxStart = ctx.dl->VtxBuffer.Size;   // キャプチャ開始
            }
        }

        if (ctx.resolvedOut)
        {
            // エディタ支援（ResolveRects）: 描画順のままスクリーン矩形を収集
            // （min/max はレイアウト空間のまま。hasXform/xform で見た目の四辺形が分かる）
            const UiRectPx s = ToScreen(current, ctx);
            ctx.resolvedOut->push_back({e, ImVec2(s.minX, s.minY), ImVec2(s.maxX, s.maxY),
                                        ctx.scale, ImVec2(ctx.originX, ctx.originY),
                                        ctx.canvasEntity, ctx.hasXform, ctx.xform});
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

        // フリック慣性: release 後は速度を指数減衰させながら流す（クランプとセットで
        // 行うのでここ。Play 中のみ = エディタプレビュー(interactive=false)では動かさない）
        if (ctx.interactive && !sv->_dragging && (sv->_velX != 0.0f || sv->_velY != 0.0f))
        {
            const float dt = ImGui::GetIO().DeltaTime;
            sv->scrollX += sv->_velX * dt;
            sv->scrollY += sv->_velY * dt;
            const float k = std::exp(-sv->flickDecay * dt);
            sv->_velX *= k;
            sv->_velY *= k;
            // 十分遅くなったら停止（サブピクセルで延々流れ続けない）
            if (std::fabs(sv->_velX) < 1.0f) sv->_velX = 0.0f;
            if (std::fabs(sv->_velY) < 1.0f) sv->_velY = 0.0f;
        }

        // スクロールを 0..(コンテンツ−ビュー) にクランプ（前フレームの計測値を使う）
        const float rawSX = sv->scrollX, rawSY = sv->scrollY;
        sv->scrollX = std::clamp(sv->scrollX, 0.0f,
                                 std::max(0.0f, sv->_contentW - current.Width()));
        sv->scrollY = std::clamp(sv->scrollY, 0.0f,
                                 std::max(0.0f, sv->_contentH - current.Height()));
        // 端でクランプされたらその軸の慣性は止める（壁で跳ねずに止まるタッチUIの定番。
        // ドラッグ中も端に張り付いた時点で速度を捨てる=壁際 release でフリックしない）
        if (ctx.interactive)
        {
            if (sv->scrollX != rawSX) sv->_velX = 0.0f;
            if (sv->scrollY != rawSY) sv->_velY = 0.0f;
        }

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

        DrawUiChildren(e, contentRect, ctx);

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

        // ホイール/ドラッグ配送用にビューポート矩形を収集（クリップ交差済み）
        if (ctx.interactive)
        {
            sv->_scale = ctx.scale;   // 入力解決のスクリーン→キャンバスpx換算用に保存
            PushHitRect(ctx.scrollRects, e, ToScreen(current, ctx), prevClip, ctx);
        }
    }
    else
    {
        // --- UIRect.clipChildren: スクロールなしの純粋マスク（ScrollView と同じ軸平行
        // シザー。はみ出た子は描画もクリックも効かない）---
        const auto* rectC = ctx.reg->try_get<UIRect>(e);
        const bool clipKids = rectC && rectC->clipChildren;
        UiRectPx clip{};
        const UiRectPx* prevClip = ctx.hitClip;
        if (clipKids)
        {
            clip = ToScreen(current, ctx);
            if (prevClip)
            {
                clip.minX = std::max(clip.minX, prevClip->minX);
                clip.minY = std::max(clip.minY, prevClip->minY);
                clip.maxX = std::min(clip.maxX, prevClip->maxX);
                clip.maxY = std::min(clip.maxY, prevClip->maxY);
            }
            ctx.hitClip = &clip;
            if (ctx.dl)
                ctx.dl->PushClipRect(ImVec2(clip.minX, clip.minY),
                                     ImVec2(clip.maxX, clip.maxY), true);
        }

        DrawUiChildren(e, current, ctx);

        if (clipKids)
        {
            if (ctx.dl)
                ctx.dl->PopClipRect();
            ctx.hitClip = prevClip;
        }
    }

    // キャプチャ終端: 自要素＋子孫＋スクロールバーの頂点へ local 変形だけを後がけする。
    // 祖先の変形は祖先自身のキャプチャが（この頂点範囲も含めて）適用するので、ここで
    // ctx.xform（累積）を掛けると二重適用になる。
    if (vtxStart >= 0 && ctx.dl)
        TransformUiVerts(ctx.dl, vtxStart, localXform);

    ctx.alphaMul = parentAlpha;   // 兄弟へ波及しないよう復元（push/pop）
    ctx.colorMul = parentColor;
    ctx.hasXform = parentHasXform;
    ctx.xform    = parentXform;
    ctx.invXform = parentInv;
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
            if (it->Contains(ctx.mousePos.x, ctx.mousePos.y))
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

    // --- UIScrollView: ドラッグ/フリックスクロール（dragScroll。タッチUI相当）---
    // mouseClicked でビューポート内の最前面ビューをドラッグ候補にし、開始点からスクリーン
    // 6px を超えて動いたらドラッグ確定=以後マウスに張り付いてスクロール。確定した瞬間に
    // 進行中のウィジェット押下をキャンセルする（リスト内のボタンをドラッグしてスクロール
    // してもクリック誤発火しない）。慣性の減衰・端の停止は描画側のクランプとセットで行う。
    // ボタンの release-inside より先に処理する（同一フレームの確定+release でも発火させない）。
    {
        const float dt = ImGui::GetIO().DeltaTime;
        // 進行中ドラッグの継続/終了（スライダー同様ポインタキャプチャ相当。ビュー外でも追従）
        for (auto [se, sv] : reg.view<UIScrollView>().each())
        {
            if (!sv._dragging)
                continue;
            // release フレームぶんの移動も先に取り込んでから終了判定する（しきい値超過と
            // release が同一フレームの超高速フリックでも誤クリックさせず、速度にも入れる）
            const float sdx = ctx.mousePos.x - sv._prevMX;   // スクリーンpx
            const float sdy = ctx.mousePos.y - sv._prevMY;
            // 開始点から 6px 超えたらドラッグ確定（それまでは候補のまま=クリック誤爆防止）
            if (!sv._dragMoved && sdx * sdx + sdy * sdy > 6.0f * 6.0f)
            {
                sv._dragMoved = true;
                // 確定した瞬間、進行中のボタン/トグル押下・スライダードラッグをキャンセル。
                // 以後 mouseClicked なしでは再押下されないので、この押下シーケンスで
                // release-inside クリックが発火することはない
                for (auto [be, btn] : reg.view<UIButton>().each())
                    btn._pressed = false;
                for (auto [te, tgl] : reg.view<UIToggle>().each())
                    tgl._pressed = false;
                for (auto [le, sld] : reg.view<UISlider>().each())
                    sld._dragging = false;
            }
            if (sv._dragMoved)
            {
                // マウス移動デルタ（スクリーン→キャンバスpx換算）で許可軸を直接動かす=指に
                // 張り付く。クランプは翌フレームの描画側（ホイールと同じ）
                const float scl = (sv._scale > 1e-6f) ? sv._scale : 1.0f;
                const float dx = sdx / scl;
                const float dy = sdy / scl;
                if (dt > 1e-6f)
                {
                    // 直近速度の指数平滑（数フレームぶん。release 時にそのまま慣性へ渡る）
                    const float a = std::min(1.0f, dt * 20.0f);
                    if (sv.horizontal) sv._velX += (-dx / dt - sv._velX) * a;
                    if (sv.vertical)   sv._velY += (-dy / dt - sv._velY) * a;
                }
                if (sv.horizontal) sv.scrollX -= dx;
                if (sv.vertical)   sv.scrollY -= dy;
                sv._prevMX = ctx.mousePos.x;
                sv._prevMY = ctx.mousePos.y;
            }
            if (!ctx.mouseDown)
            {
                // release: ドラッグ中に平滑追跡した直近速度がそのまま慣性の初速になる
                //（flickDecay=0 は慣性なし=離した瞬間停止）
                sv._dragging  = false;
                sv._dragMoved = false;
                if (sv.flickDecay <= 0.0f)
                {
                    sv._velX = 0.0f;
                    sv._velY = 0.0f;
                }
            }
        }
        // ドラッグ候補の開始（ホイールと同じ「最前面のビュー 1 つ」だけ。ネスト時は手前優先）
        if (mouseValid && ctx.mouseClicked)
        {
            for (auto it = scrollRects.rbegin(); it != scrollRects.rend(); ++it)
            {
                if (!it->Contains(ctx.mousePos.x, ctx.mousePos.y))
                    continue;
                if (auto* sv = reg.try_get<UIScrollView>(it->e); sv && sv->dragScroll)
                {
                    sv->_dragging  = true;
                    sv->_dragMoved = false;
                    sv->_prevMX = ctx.mousePos.x;   // 確定までは「押下開始点」として保持
                    sv->_prevMY = ctx.mousePos.y;
                    sv->_velX = 0.0f;   // 触った瞬間に進行中の慣性を止める（触って止めるのも定番）
                    sv->_velY = 0.0f;
                }
                break;   // 一番手前のスクロールビューだけが受ける（ホイールと同じ選び方）
            }
        }
    }

    // 各ボタンの状態更新（描画ティントは前フレーム状態で済ませてある）
    for (const UiHitEntry& hit : buttonRects)
    {
        auto* btn = reg.try_get<UIButton>(hit.e);
        if (!btn)
            continue;
        const bool inside = mouseValid && hit.Contains(ctx.mousePos.x, ctx.mousePos.y);
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
        const bool inside = mouseValid && hit.Contains(ctx.mousePos.x, ctx.mousePos.y);
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
                // マウス X → 実値（描画と同じ「つまみ半径ぶん内側」のトラック範囲で正規化）。
                // 回転スライダーはマウスをレイアウト空間へ逆写像してから同じ式に流す
                // （描画もレイアウト空間で emit してから回しているため厳密に一致する）。
                ImVec2 m = ctx.mousePos;
                if (hit.hasXform)
                    m = hit.invXform.Apply(m.x, m.y);
                const float knobR = std::max(3.0f, hit.rect.Height() * 0.5f);
                const float x0 = hit.rect.minX + knobR, x1 = hit.rect.maxX - knobR;
                const float t = std::clamp((m.x - x0) / std::max(1.0f, x1 - x0),
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
            if (!it->Contains(ctx.mousePos.x, ctx.mousePos.y))
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
            // フォーカスが無い/消えた: 一番左上のウィジェットへ（初回のナビ入力で必ず現れる）。
            // 回転ウィジェットは変形後の中心で比較する
            const UiHitEntry* best = &focusables.front();
            float bestKey = best->Center().y + best->Center().x * 0.001f;
            for (const auto& f : focusables)
            {
                const ImVec2 c = f.Center();
                const float key = c.y + c.x * 0.001f;
                if (key < bestKey) { bestKey = key; best = &f; }
            }
            m_focused = best->e;
            m_confirmHeld = false;
            if (const auto* b = reg.try_get<UIButton>(m_focused); b && !b->hoverSound.empty())
                m_pendingSfx.push_back(b->hoverSound);
        }
        else
        {
            // 空間ナビゲーション: 方向の成分が正で、直交ずれのペナルティ込み最短の候補へ
            // （回転ウィジェットは変形後の中心で比較）
            const ImVec2 cc = cur->Center();
            const float cx = cc.x, cy = cc.y;
            const UiHitEntry* best = nullptr;
            float bestScore = FLT_MAX;
            for (const auto& f : focusables)
            {
                if (f.e == m_focused) continue;
                const ImVec2 fc = f.Center();
                const float fx = fc.x - cx;
                const float fy = fc.y - cy;
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

        // フォーカスリング（UI 全体の最前面。少し外側の角丸枠 + 薄いグロー）。
        // 回転ウィジェットは拡張矩形の 4 隅を変形した四辺形で描く（角丸は諦める）
        const UiRectPx& r = cur->rect;
        const ImU32 ring = IM_COL32(120, 180, 255, 235);
        const ImU32 glow = IM_COL32(120, 180, 255, 60);
        if (cur->hasXform)
        {
            const auto quad = [&](float grow, ImU32 col, float th) {
                const ImVec2 a = cur->xform.Apply(r.minX - grow, r.minY - grow);
                const ImVec2 b = cur->xform.Apply(r.maxX + grow, r.minY - grow);
                const ImVec2 c = cur->xform.Apply(r.maxX + grow, r.maxY + grow);
                const ImVec2 d = cur->xform.Apply(r.minX - grow, r.maxY + grow);
                dl->AddQuad(a, b, c, d, col, th);
            };
            quad(5.0f, glow, 6.0f);
            quad(3.0f, ring, 2.0f);
        }
        else
        {
            dl->AddRect(ImVec2(r.minX - 5.0f, r.minY - 5.0f), ImVec2(r.maxX + 5.0f, r.maxY + 5.0f),
                        glow, 8.0f, 0, 6.0f);
            dl->AddRect(ImVec2(r.minX - 3.0f, r.minY - 3.0f), ImVec2(r.maxX + 3.0f, r.maxY + 3.0f),
                        ring, 6.0f, 0, 2.0f);
        }
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
    // UIScrollView: ドラッグ/フリックの進行状態を破棄（押下残留の回収と同じ流儀）
    for (auto [e, sv] : reg.view<UIScrollView>().each())
    {
        sv._dragging  = false;
        sv._dragMoved = false;
        sv._velX = 0.0f;
        sv._velY = 0.0f;
    }
    // UIAnimator: 次の Play で出現アニメが最初から再生されるように全ランタイム状態を戻す
    for (auto [e, an] : reg.view<UIAnimator>().each())
    {
        an._t = 0.0f;
        an._mode = 0;
        an._hoverS = 1.0f;
        an._loopT = 0.0f;
        an._curScaleX = 1.0f;
        an._curScaleY = 1.0f;
        an._curAlpha = 1.0f;
        an._curRot   = 0.0f;
        an._curOffset = {0.0f, 0.0f};
    }
    // タイプライターも次の Play で先頭から
    for (auto [e, txt] : reg.view<UIText>().each())
        txt._twT = 0.0f;
    // tween はランタイム専用なので丸ごと破棄
    {
        auto view = reg.view<UITweenState>();
        reg.remove<UITweenState>(view.begin(), view.end());
    }
}

} // namespace dx12e
