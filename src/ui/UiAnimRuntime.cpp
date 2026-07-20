#include "ui/UiAnimRuntime.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "core/Logger.h"
#include "core/vfs/Vfs.h"
#include "ecs/Components.h"

namespace dx12e
{
namespace
{

// 名前パスを '/' で分割。連続スラッシュと前後の空要素は捨てる（"a//b/" → {"a","b"}）。
std::vector<std::string> SplitPath(const std::string& path)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < path.size())
    {
        const size_t sep = path.find('/', i);
        const size_t end = (sep == std::string::npos) ? path.size() : sep;
        if (end > i) out.emplace_back(path.substr(i, end - i));
        i = end + 1;
    }
    return out;
}

const std::string& EntityName(const entt::registry& reg, entt::entity e)
{
    static const std::string kEmpty;
    const auto* n = reg.try_get<NameTag>(e);
    return n ? n->name : kEmpty;
}

// 直下の子から name 一致を探す。同名が複数なら registry 走査順の最初。
entt::entity FindChildByName(const entt::registry& reg, entt::entity parent, const std::string& name)
{
    for (auto [child, tr] : reg.view<const Transform>().each())
    {
        if (tr.parent != parent) continue;
        if (EntityName(reg, child) == name) return child;
    }
    return entt::null;
}

} // namespace

// ---------------------------------------------------------------------------
// 対象エンティティの解決
// ---------------------------------------------------------------------------
entt::entity UiAnimRuntime::ResolveTarget(const entt::registry& reg, entt::entity root,
                                          const std::string& path)
{
    if (root == entt::null || !reg.valid(root)) return entt::null;
    entt::entity cur = root;
    for (const auto& seg : SplitPath(path))
    {
        cur = FindChildByName(reg, cur, seg);
        if (cur == entt::null) return entt::null;
    }
    return cur;
}

bool UiAnimRuntime::MakeTargetPath(const entt::registry& reg, entt::entity root, entt::entity e,
                                   std::string& outPath)
{
    outPath.clear();
    if (e == root) return true;

    // e から root まで親を遡り、逆順に繋ぐ。深さ上限は UISystem のバブリングと同じ 64
    // （循環した親子関係が混入しても無限ループしない）。
    std::vector<std::string> segs;
    entt::entity cur = e;
    for (int guard = 0; guard < 64; ++guard)
    {
        if (cur == entt::null || !reg.valid(cur)) return false;
        const auto* tr = reg.try_get<Transform>(cur);
        if (!tr) return false;
        segs.push_back(EntityName(reg, cur));
        if (tr->parent == root)
        {
            for (auto it = segs.rbegin(); it != segs.rend(); ++it)
            {
                if (!outPath.empty()) outPath += '/';
                outPath += *it;
            }
            return true;
        }
        cur = tr->parent;
    }
    return false;   // root の子孫ではなかった（または階層が深すぎ/循環）
}

// ---------------------------------------------------------------------------
// プロパティの読み書き
// ---------------------------------------------------------------------------
f32 UiAnimRuntime::ReadProp(const entt::registry& reg, entt::entity e, int prop)
{
    if (e == entt::null || !reg.valid(e)) return UiAnimPropDefault(prop);

    if (const auto* r = reg.try_get<UIRect>(e))
    {
        switch (prop)
        {
        case kUiPropOffsetMinX: return r->offsetMin.x;
        case kUiPropOffsetMinY: return r->offsetMin.y;
        case kUiPropOffsetMaxX: return r->offsetMax.x;
        case kUiPropOffsetMaxY: return r->offsetMax.y;
        case kUiPropAnchorMinX: return r->anchorMin.x;
        case kUiPropAnchorMinY: return r->anchorMin.y;
        case kUiPropAnchorMaxX: return r->anchorMax.x;
        case kUiPropAnchorMaxY: return r->anchorMax.y;
        case kUiPropPivotX:     return r->pivot.x;
        case kUiPropPivotY:     return r->pivot.y;
        case kUiPropRotation:   return r->rotation;
        case kUiPropSkewX:      return r->skewX;
        case kUiPropScaleX:     return r->scaleX;
        case kUiPropScaleY:     return r->scaleY;
        case kUiPropGroupAlpha: return r->alpha;
        case kUiPropVisible:    return r->visible ? 1.0f : 0.0f;
        default: break;
        }
    }

    // 色は UIImage 優先で読む（両方持つ要素はまず無いが、画像が主役という規約に揃える）
    if (const auto* img = reg.try_get<UIImage>(e))
    {
        switch (prop)
        {
        case kUiPropColorR:     return img->color.x;
        case kUiPropColorG:     return img->color.y;
        case kUiPropColorB:     return img->color.z;
        case kUiPropColorA:     return img->color.w;
        case kUiPropFillAmount: return img->fillAmount;
        case kUiPropUvScrollU:  return img->uvScroll.x;
        case kUiPropUvScrollV:  return img->uvScroll.y;
        default: break;
        }
    }
    if (const auto* txt = reg.try_get<UIText>(e))
    {
        switch (prop)
        {
        case kUiPropColorR:  return txt->color.x;
        case kUiPropColorG:  return txt->color.y;
        case kUiPropColorB:  return txt->color.z;
        case kUiPropColorA:  return txt->color.w;
        case kUiPropFontSize: return txt->fontSize;
        default: break;
        }
    }
    return UiAnimPropDefault(prop);
}

void UiAnimRuntime::WriteProp(entt::registry& reg, entt::entity e, int prop, f32 v)
{
    if (e == entt::null || !reg.valid(e)) return;

    if (auto* r = reg.try_get<UIRect>(e))
    {
        switch (prop)
        {
        case kUiPropOffsetMinX: r->offsetMin.x = v; return;
        case kUiPropOffsetMinY: r->offsetMin.y = v; return;
        case kUiPropOffsetMaxX: r->offsetMax.x = v; return;
        case kUiPropOffsetMaxY: r->offsetMax.y = v; return;
        case kUiPropAnchorMinX: r->anchorMin.x = v; return;
        case kUiPropAnchorMinY: r->anchorMin.y = v; return;
        case kUiPropAnchorMaxX: r->anchorMax.x = v; return;
        case kUiPropAnchorMaxY: r->anchorMax.y = v; return;
        case kUiPropPivotX:     r->pivot.x = v; return;
        case kUiPropPivotY:     r->pivot.y = v; return;
        case kUiPropRotation:   r->rotation = v; return;
        case kUiPropSkewX:      r->skewX = v; return;
        case kUiPropScaleX:     r->scaleX = v; return;
        case kUiPropScaleY:     r->scaleY = v; return;
        case kUiPropGroupAlpha: r->alpha = v; return;
        case kUiPropVisible:    r->visible = (v >= 0.5f); return;
        default: break;
        }
    }

    // 色系は UIImage と UIText の両方へ書く（同じ要素が両方持つケースで片方だけ残らないように）
    bool handled = false;
    if (auto* img = reg.try_get<UIImage>(e))
    {
        switch (prop)
        {
        case kUiPropColorR:     img->color.x = v; handled = true; break;
        case kUiPropColorG:     img->color.y = v; handled = true; break;
        case kUiPropColorB:     img->color.z = v; handled = true; break;
        case kUiPropColorA:     img->color.w = v; handled = true; break;
        case kUiPropFillAmount: img->fillAmount = std::clamp(v, 0.0f, 1.0f); handled = true; break;
        case kUiPropUvScrollU:  img->uvScroll.x = v; handled = true; break;
        case kUiPropUvScrollV:  img->uvScroll.y = v; handled = true; break;
        case kUiPropSpriteFrame:
        {
            // シート分割は UIImage 自身の animCols/animRows を流用する（連番アニメと同じ設定を
            // 使い回せる）。animFrames は 0 のままでよい = 自動再生とは独立にコマを指定できる。
            const i32 cols = (img->animCols > 0) ? img->animCols : 1;
            const i32 rows = (img->animRows > 0) ? img->animRows : 1;
            const SpriteUvRect uv = SpriteSheetCellUv(cols, rows, static_cast<i32>(std::lround(v)));
            img->uvMin = {uv.u0, uv.v0};
            img->uvMax = {uv.u1, uv.v1};
            handled = true;
            break;
        }
        default: break;
        }
    }
    if (auto* txt = reg.try_get<UIText>(e))
    {
        switch (prop)
        {
        case kUiPropColorR:   txt->color.x = v; handled = true; break;
        case kUiPropColorG:   txt->color.y = v; handled = true; break;
        case kUiPropColorB:   txt->color.z = v; handled = true; break;
        case kUiPropColorA:   txt->color.w = v; handled = true; break;
        case kUiPropFontSize: txt->fontSize = (std::max)(1.0f, v); handled = true; break;
        default: break;
        }
    }
    (void)handled;   // 対象コンポーネントが無いトラックは黙って無視（要素を消しても壊れない）
}

// ---------------------------------------------------------------------------
// アセットのロード / キャッシュ
// ---------------------------------------------------------------------------
std::vector<uint8_t> UiAnimRuntime::ReadAssetBytes(const std::string& relPath) const
{
    // ゲームモードなら pak から、エディタならディスクから（vfs が両方を吸収する）
    std::vector<uint8_t> bytes = vfs::ReadAsset(relPath);
    if (!bytes.empty()) return bytes;

    // vfs が空を返すのは「pak 未マウント かつ AssetsDir 未設定」のケースが主。
    // パネルから直接 assetsDir を渡されている場合はそっちで読み直す。
    if (m_assetsDir.empty()) return bytes;
    std::ifstream f(m_assetsDir + relPath, std::ios::binary);
    if (!f) return bytes;
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

const UiAnimClip* UiAnimRuntime::GetClip(const std::string& relPath)
{
    if (relPath.empty()) return nullptr;
    if (const auto it = m_clips.find(relPath); it != m_clips.end())
        return it->second.ok ? &it->second.data : nullptr;

    ClipSlot slot;
    const std::vector<uint8_t> bytes = ReadAssetBytes(relPath);
    if (bytes.empty())
        Logger::Warn("UiAnim: クリップを読めない: {}", relPath);
    else if (!ParseUiAnimClip(bytes, slot.data))
        Logger::Warn("UiAnim: クリップの JSON が不正: {}", relPath);
    else
        slot.ok = true;

    // 失敗も覚える（毎フレーム I/O を叩かない）。Invalidate で再試行できる。
    const auto [it, _] = m_clips.emplace(relPath, std::move(slot));
    return it->second.ok ? &it->second.data : nullptr;
}

const SpriteAnimSheet* UiAnimRuntime::GetSheet(const std::string& relPath)
{
    if (relPath.empty()) return nullptr;
    if (const auto it = m_sheets.find(relPath); it != m_sheets.end())
        return it->second.ok ? &it->second.data : nullptr;

    SheetSlot slot;
    const std::vector<uint8_t> bytes = ReadAssetBytes(relPath);
    if (bytes.empty())
        Logger::Warn("SpriteAnim: シートを読めない: {}", relPath);
    else if (!ParseSpriteAnimSheet(bytes, slot.data))
        Logger::Warn("SpriteAnim: シートの JSON が不正: {}", relPath);
    else
        slot.ok = true;

    const auto [it, _] = m_sheets.emplace(relPath, std::move(slot));
    return it->second.ok ? &it->second.data : nullptr;
}

void UiAnimRuntime::Invalidate(const std::string& relPath)
{
    if (relPath.empty())
    {
        m_clips.clear();
        m_sheets.clear();
        return;
    }
    m_clips.erase(relPath);
    m_sheets.erase(relPath);
}

// ---------------------------------------------------------------------------
// 再生制御
// ---------------------------------------------------------------------------
void UiAnimRuntime::OnPlayStart(entt::registry& reg)
{
    for (auto [e, pl] : reg.view<UIAnimPlayer>().each())
    {
        pl._time = pl._prevTime = 0.0f;
        pl._finished = false;
        pl._playing  = pl.playOnStart && !pl.clipPath.empty();
        pl._curScaleX = pl._curScaleY = pl._curAlpha = 1.0f;
    }
    for (auto [e, sa] : reg.view<SpriteAnimator>().each())
    {
        sa._time = 0.0f;
        sa._finished = false;
        sa._playing  = sa.playOnStart && !sa.sheetPath.empty();
        sa._curFrame = -1;
    }
}

void UiAnimRuntime::OnPlayStop(entt::registry& reg)
{
    // 再生位置だけ畳む。コンポーネント値そのものは Stop 時のシーン復元が元に戻すので触らない
    // （ここで既定値へ戻すと、Play していないシーンの見た目まで書き換えてしまう）。
    for (auto [e, pl] : reg.view<UIAnimPlayer>().each())
    {
        pl._playing = false;
        pl._time = pl._prevTime = 0.0f;
        pl._finished = false;
    }
    for (auto [e, sa] : reg.view<SpriteAnimator>().each())
    {
        sa._playing = false;
        sa._time = 0.0f;
        sa._finished = false;
    }
}

void UiAnimRuntime::ApplyClipAt(entt::registry& reg, entt::entity root, const UiAnimClip& clip,
                                f32 time)
{
    for (const auto& tr : clip.tracks)
    {
        const entt::entity target = ResolveTarget(reg, root, tr.target);
        if (target == entt::null) continue;         // 消された要素のトラックは黙って飛ばす
        WriteProp(reg, target, tr.prop, EvaluateUiAnimTrack(tr, time));
    }
}

void UiAnimRuntime::Update(entt::registry& reg, f32 dt, std::vector<PendingEvent>& outEvents)
{
    dt = std::clamp(dt, 0.0f, 0.1f);   // UpdateUiAnimations と同じヒッチ対策

    // ---- UIAnimPlayer（.uianim クリップ）----
    for (auto [e, pl] : reg.view<UIAnimPlayer>().each())
    {
        if (!pl._playing || pl.clipPath.empty()) continue;
        const UiAnimClip* clip = GetClip(pl.clipPath);
        if (!clip)
        {
            pl._playing = false;   // 読めないクリップで毎フレーム警告を出し続けない
            continue;
        }

        pl._prevTime = pl._time;
        pl._time += dt * pl.speed;

        const bool loop = pl.loop || clip->loop;
        const bool done = WrapUiAnimTime(pl._time, clip->duration, loop);

        ApplyClipAt(reg, e, *clip, pl._time);

        // イベント: 逆再生中は区間が反転するので、進行方向に合わせて (prev, cur] を作り直す
        std::vector<const UiAnimEvent*> hits;
        if (pl.speed >= 0.0f) CollectUiAnimEvents(*clip, pl._prevTime, pl._time, hits);
        else                  CollectUiAnimEvents(*clip, pl._time, pl._prevTime, hits);
        for (const auto* ev : hits)
            outEvents.push_back({e, ev->event});

        if (done && !pl._finished)
        {
            pl._finished = true;
            pl._playing  = false;
            if (!pl.finishEvent.empty()) outEvents.push_back({e, pl.finishEvent});
        }
    }

    // ---- SpriteAnimator（.spranim シート）----
    for (auto [e, sa] : reg.view<SpriteAnimator>().each())
    {
        if (!sa._playing || sa.sheetPath.empty()) continue;
        const SpriteAnimSheet* sheet = GetSheet(sa.sheetPath);
        if (!sheet) { sa._playing = false; continue; }
        const SpriteAnimSeq* seq = FindSpriteSeq(*sheet, sa.currentSeq);
        if (!seq || seq->frames.empty()) continue;

        sa._time += dt * sa.speed;
        if (sa._time < 0.0f) sa._time = 0.0f;

        bool finished = false;
        const i32 idx = SpriteSeqIndexAt(*seq, sa._time, &finished);
        if (idx < 0) continue;

        const i32 cell = seq->frames[static_cast<size_t>(idx)];
        sa._curFrame = cell;
        const SpriteUvRect uv = SpriteSheetCellUv(sheet->cols, sheet->rows, cell);

        // Sprite2D / UIImage の両方に効く（片方しか無いのが普通だが、条件分岐を増やさない）
        if (auto* sp = reg.try_get<Sprite2D>(e))
        {
            sp->uvMin = {uv.u0, uv.v0};
            sp->uvMax = {uv.u1, uv.v1};
            sp->animFrames = 0;   // 旧フリップブックと二重駆動させない（こちらが勝つ）
            if (sa.applyTexture && !sheet->texturePath.empty()) sp->texturePath = sheet->texturePath;
        }
        if (auto* img = reg.try_get<UIImage>(e))
        {
            img->uvMin = {uv.u0, uv.v0};
            img->uvMax = {uv.u1, uv.v1};
            img->animFrames = 0;
            if (sa.applyTexture && !sheet->texturePath.empty()) img->texturePath = sheet->texturePath;
        }

        if (finished && !sa._finished)
        {
            sa._finished = true;
            sa._playing  = false;
            if (!seq->finishEvent.empty()) outEvents.push_back({e, seq->finishEvent});
        }
    }
}

} // namespace dx12e
