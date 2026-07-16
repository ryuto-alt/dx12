#include "ui/UISystem.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>

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
        float alpha = 1.0f, scale = 1.0f, rot = 0.0f;
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
            case 7:  alpha = ev; scale = 0.5f + 0.5f * ev;                          // スピン入場
                     rot = -180.0f * (1.0f - ev);                           break;
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
            else if (an.loopAnim == 4) rot += an._loopT * an.loopSpeed * 360.0f;          // スピン
            else if (an.loopAnim == 5) rot += s * an.loopAmount;                          // スウィング(±度)
        }

        an._curScale  = (std::max)(0.0f, scale * an._hoverS);
        an._curAlpha  = std::clamp(alpha, 0.0f, 1.0f);
        an._curRot    = rot;
        an._curOffset = off;
    }

    // ---- tween（scene:tweenUi。移動は UIRect offset を直接進め、拡縮/アルファは視覚のみ）----
    for (auto [e, tw] : reg.view<UITweenState>().each())
    {
        float scale = tw.visScale, alpha = tw.visAlpha, rot = tw.visRot;
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
                t.rotFrom   = tw.visRot;
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
            if (t.hasScale)  scale = t.scaleFrom + (t.scaleTo - t.scaleFrom) * ev;
            if (t.hasAlpha)  alpha = t.alphaFrom + (t.alphaTo - t.alphaFrom) * ev;
            if (t.hasRotate) rot   = t.rotFrom + (t.rotTo - t.rotFrom) * ev;
            if (p >= 1.0f)
            {
                // 完了: 視覚値は持続させる（alpha=0 で消したままにできる）
                if (t.hasScale)  tw.visScale = t.scaleTo;
                if (t.hasAlpha)  tw.visAlpha = t.alphaTo;
                if (t.hasRotate) tw.visRot   = t.rotTo;
                it = tw.tweens.erase(it);
            }
            else
            {
                ++it;
            }
        }
        tw._curScale = (std::max)(0.0f, scale);
        tw._curAlpha = std::clamp(alpha, 0.0f, 1.0f);
        tw._curRot   = rot;
    }
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

    // --- UIImage: テクスチャ（9-slice 対応）または単色矩形。ボタンの状態色を乗算 ---
    if (const auto* img = reg.try_get<UIImage>(e))
    {
        // fillAmount: 表示割合 0..1（HPバー/ゲージ用）。幾何方式＝表示部分の矩形へ縮め、
        // テクスチャは UV も比例で切る（クリップ矩形方式とピクセル一致）。GPU シザーは
        // スクリーン軸固定なので、回転（頂点変換）と干渉しないようクリップには依存しない。
        // 0 は完全非表示（レイキャストは上の blockers 収集どおり効いたまま）。
        const float fill = std::clamp(img->fillAmount, 0.0f, 1.0f);
        if (fill > 0.0f)
        {
            const bool fillPartial = fill < 1.0f;
            UiRectPx fs = s;   // 表示部分（スクリーン空間）
            if (fillPartial)
            {
                switch (img->fillDir)
                {
                case 1:  fs.minX = s.maxX - s.Width() * fill;  break;   // 右から
                case 2:  fs.minY = s.maxY - s.Height() * fill; break;   // 下から
                case 3:  fs.maxY = s.minY + s.Height() * fill; break;   // 上から
                default: fs.maxX = s.minX + s.Width() * fill;  break;   // 0: 左から
                }
            }

            const DirectX::XMFLOAT4 col{img->color.x * tint.x, img->color.y * tint.y,
                                        img->color.z * tint.z,
                                        img->color.w * tint.w * ctx.alphaMul};
            const ImU32 ucol = ToImCol(col);

            // --- ドロップシャドウ（本体より先に描く。矩形近似 = テクスチャのアルファ形状は
            // 反映しない。fillAmount<1 でもフル矩形 = ゲージが減っても影は変わらないのが正しい）---
            if (img->shadowColor.w > 0.0f)
            {
                const float offX = img->shadowOffset.x * ctx.scale;
                const float offY = img->shadowOffset.y * ctx.scale;
                const float soft = std::max(0.0f, img->shadowSoftness) * ctx.scale;
                const float baseRad = img->cornerRadius * ctx.scale;
                if (soft < 0.5f)
                {
                    ctx.dl->AddRectFilled(
                        ImVec2(s.minX + offX, s.minY + offY),
                        ImVec2(s.maxX + offX, s.maxY + offY),
                        ToImCol({img->shadowColor.x, img->shadowColor.y, img->shadowColor.z,
                                 img->shadowColor.w * ctx.alphaMul}),
                        baseRad);
                }
                else
                {
                    // 4 層を外へ広げつつアルファ減衰 = 疑似ソフトシャドウ
                    static constexpr float kFall[4] = {0.42f, 0.24f, 0.12f, 0.05f};
                    for (int i = 0; i < 4; ++i)
                    {
                        const float grow = soft * static_cast<float>(i + 1) / 4.0f;
                        ctx.dl->AddRectFilled(
                            ImVec2(s.minX + offX - grow, s.minY + offY - grow),
                            ImVec2(s.maxX + offX + grow, s.maxY + offY + grow),
                            ToImCol({img->shadowColor.x, img->shadowColor.y, img->shadowColor.z,
                                     img->shadowColor.w * ctx.alphaMul * kFall[i]}),
                            baseRad + grow);
                    }
                }
            }

            // グラデーション: 影の後・本体の前から頂点範囲を記録し、本体描画後に
            // 線形シェードを後がけする（テクスチャ/9-slice/角丸すべて対応、回転にも追従）
            const int gradStart = (img->gradientDir > 0) ? ctx.dl->VtxBuffer.Size : -1;
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
                        AddImage9Slice(ctx.dl, texId, ImVec2(s.minX, s.minY), ImVec2(s.maxX, s.maxY),
                                       uv0, uv1,
                                       static_cast<float>(tex->GetWidth()),
                                       static_cast<float>(tex->GetHeight()),
                                       img->sliceBorder, ctx.scale, ucol,
                                       fillPartial ? &fs : nullptr);
                    else
                    {
                        // 平板: 表示部分に合わせて UV を比例クロップ
                        const float w = std::max(1e-6f, s.Width());
                        const float h = std::max(1e-6f, s.Height());
                        const ImVec2 fuv0(uv0.x + (uv1.x - uv0.x) * (fs.minX - s.minX) / w,
                                          uv0.y + (uv1.y - uv0.y) * (fs.minY - s.minY) / h);
                        const ImVec2 fuv1(uv1.x - (uv1.x - uv0.x) * (s.maxX - fs.maxX) / w,
                                          uv1.y - (uv1.y - uv0.y) * (s.maxY - fs.maxY) / h);
                        ctx.dl->AddImage(texId, ImVec2(fs.minX, fs.minY), ImVec2(fs.maxX, fs.maxY),
                                         fuv0, fuv1, ucol);
                    }
                    drawn = true;
                }
            }
            // texturePath 空 or ロード失敗 → 単色矩形（角丸対応）。失敗時も要素が見えるようにする
            if (!drawn)
            {
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
                ImVec2 p0(s.minX, s.minY), p1;
                switch (img->gradientDir)
                {
                case 2:  p1 = ImVec2(s.minX, s.maxY); break;   // 縦（上→下）
                case 3:  p1 = ImVec2(s.maxX, s.maxY); break;   // 斜め（左上→右下）
                default: p1 = ImVec2(s.maxX, s.minY); break;   // 1: 横（左→右）
                }
                // 終端色にも tint / 本体アルファを乗算（KeepAlpha = 頂点のアルファは保持され、
                // gradientColor2 のアルファは使われない）
                const ImU32 ucol2 = ToImCol({img->gradientColor2.x * tint.x,
                                             img->gradientColor2.y * tint.y,
                                             img->gradientColor2.z * tint.z, col.w});
                ImGui::ShadeVertsLinearColorGradientKeepAlpha(
                    ctx.dl, gradStart, ctx.dl->VtxBuffer.Size, p0, p1, ucol, ucol2);
            }

            // --- 縁取り（枠線。角丸に追従。ゲージ減少に影響されないようフル矩形）---
            if (img->outlineWidth > 0.0f && img->outlineColor.w > 0.0f)
            {
                ctx.dl->AddRect(ImVec2(s.minX, s.minY), ImVec2(s.maxX, s.maxY),
                                ToImCol({img->outlineColor.x, img->outlineColor.y,
                                         img->outlineColor.z,
                                         img->outlineColor.w * ctx.alphaMul}),
                                img->cornerRadius * ctx.scale, 0,
                                std::max(1.0f, img->outlineWidth * ctx.scale));
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
            if (!txt->fontPath.empty())
                if (ImFont* custom = GetOrLoadUiFont(txt->fontPath))
                    font = custom;
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

            // 影（1 オフセットの先描き）→ 縁取り（8 方位の重ね描き）→ 本体。
            // 全部このサブツリーの頂点キャプチャ内なので回転にも一緒に追従する
            if (txt->shadowColor.w > 0.0f)
            {
                ctx.dl->AddText(font, fontSize,
                                ImVec2(tx + txt->shadowOffset.x * ctx.scale,
                                       ty + txt->shadowOffset.y * ctx.scale),
                                ToImCol({txt->shadowColor.x, txt->shadowColor.y,
                                         txt->shadowColor.z,
                                         txt->shadowColor.w * ctx.alphaMul}),
                                txt->text.c_str(), nullptr, wrapW);
            }
            if (txt->outlineWidth > 0.0f && txt->outlineColor.w > 0.0f)
            {
                const float ow = std::max(1.0f, txt->outlineWidth * ctx.scale);
                const ImU32 ocol = ToImCol({txt->outlineColor.x, txt->outlineColor.y,
                                            txt->outlineColor.z,
                                            txt->outlineColor.w * ctx.alphaMul});
                static constexpr float kDirs[8][2] = {
                    {-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f},
                    {-0.7071f, -0.7071f}, {0.7071f, -0.7071f},
                    {-0.7071f, 0.7071f},  {0.7071f, 0.7071f}};
                for (const auto& d : kDirs)
                    ctx.dl->AddText(font, fontSize,
                                    ImVec2(tx + d[0] * ow, ty + d[1] * ow), ocol,
                                    txt->text.c_str(), nullptr, wrapW);
            }
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
                fxRot   += an->_curRot;
                fxOffX  += an->_curOffset.x;
                fxOffY  += an->_curOffset.y;
            }
            if (const auto* tw = ctx.reg->try_get<UITweenState>(e))
            {
                fxScale *= tw->_curScale;
                fxAlpha *= tw->_curAlpha;
                fxRot   += tw->_curRot;
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

        // --- 回転/スキュー（視覚変換。レイアウトは AABB のまま）---
        // pivot はエフェクト適用後の矩形のスクリーン座標で取る。UIScrollView ノード自身は
        // 軸平行 GPU シザーと両立できないため無視（Inspector にも注記）。
        // interactive ゲート外 = エディタプレビュー（RenderPreview）でも見える静的属性。
        {
            float rotDeg  = rect->rotation + fxRot;   // 静的回転 + Animator/tween の追加回転（度の加算 = 可換）
            float skewDeg = rect->skewX;
            if (ctx.reg->all_of<UIScrollView>(e))
            {
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
            PushHitRect(ctx.scrollRects, e, ToScreen(current, ctx), prevClip, ctx);
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

    // キャプチャ終端: 自要素＋子孫＋スクロールバーの頂点へ local 変形だけを後がけする。
    // 祖先の変形は祖先自身のキャプチャが（この頂点範囲も含めて）適用するので、ここで
    // ctx.xform（累積）を掛けると二重適用になる。
    if (vtxStart >= 0 && ctx.dl)
        TransformUiVerts(ctx.dl, vtxStart, localXform);

    ctx.alphaMul = parentAlpha;   // 兄弟へ波及しないよう復元（push/pop）
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
    // UIAnimator: 次の Play で出現アニメが最初から再生されるように全ランタイム状態を戻す
    for (auto [e, an] : reg.view<UIAnimator>().each())
    {
        an._t = 0.0f;
        an._mode = 0;
        an._hoverS = 1.0f;
        an._loopT = 0.0f;
        an._curScale = 1.0f;
        an._curAlpha = 1.0f;
        an._curRot   = 0.0f;
        an._curOffset = {0.0f, 0.0f};
    }
    // tween はランタイム専用なので丸ごと破棄
    {
        auto view = reg.view<UITweenState>();
        reg.remove<UITweenState>(view.begin(), view.end());
    }
}

} // namespace dx12e
