#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Types.h"
#include "ui/UiEase.h"

namespace dx12e
{

// .uianim（UIアニメーションクリップ）の中身と評価。
// GPU/ECS/ImGui に一切触れない純データ + 純関数なので単体テスト可能
// （tests/ui_anim_test.cpp 参照）。ランタイム適用は UISystem、オーサリングは
// AnimationEditorPanel が行う。

// ---------------------------------------------------------------------------
// アニメ可能プロパティ
// ---------------------------------------------------------------------------
// トラック1本 = スカラー1本。番号は .uianim にそのまま保存されるので、
// 既存の値の意味を変えるとアセット互換が壊れる。**追加は必ず末尾へ**。
enum UiAnimProp : int
{
    kUiPropOffsetMinX = 0,   // UIRect.offsetMin.x（px、レイアウトごと動く）
    kUiPropOffsetMinY,
    kUiPropOffsetMaxX,
    kUiPropOffsetMaxY,
    kUiPropAnchorMinX,       // UIRect.anchorMin.x（0..1）
    kUiPropAnchorMinY,
    kUiPropAnchorMaxX,
    kUiPropAnchorMaxY,
    kUiPropPivotX,
    kUiPropPivotY,
    kUiPropRotation,         // UIRect.rotation（度）
    kUiPropSkewX,            // UIRect.skewX（度）
    kUiPropScaleX,           // 視覚スケール（レイアウト矩形は動かさない）
    kUiPropScaleY,
    kUiPropGroupAlpha,       // 子孫へ継承するアルファ（0..1）
    kUiPropColorR,           // UIImage.color / UIText.color（両方持つなら両方書く）
    kUiPropColorG,
    kUiPropColorB,
    kUiPropColorA,           // この要素だけのアルファ（継承しない）
    kUiPropFillAmount,       // UIImage.fillAmount（0..1、ゲージ用）
    kUiPropFontSize,         // UIText.fontSize
    kUiPropUvScrollU,        // UIImage.uvScroll
    kUiPropUvScrollV,
    kUiPropSpriteFrame,      // UIImage の連番フレーム直指定（step 補間）
    kUiPropVisible,          // UIRect.visible（0/1、step 補間）
    kUiPropCount
};

// 表示名（エディタのトラック追加コンボ用）。範囲外は "?" を返す。
// 唯一 .cpp 側にある関数（文字列テーブルを翻訳可能な1か所に閉じ込めるため）。
const char* UiAnimPropName(int prop);

// 補間せず「直前キーの値を保持」するプロパティか（フレーム番号・表示フラグ）。
inline bool UiAnimPropIsStep(int prop)
{
    return prop == kUiPropSpriteFrame || prop == kUiPropVisible;
}

// そのプロパティのトラックが無いときの中立値。
// 乗算的に効くもの（スケール/アルファ/色/表示）は 1、加算的なものは 0。
inline float UiAnimPropDefault(int prop)
{
    switch (prop)
    {
    case kUiPropScaleX:
    case kUiPropScaleY:
    case kUiPropGroupAlpha:
    case kUiPropColorR:
    case kUiPropColorG:
    case kUiPropColorB:
    case kUiPropColorA:
    case kUiPropVisible:
        return 1.0f;
    default:
        return 0.0f;
    }
}

// ---------------------------------------------------------------------------
// クリップ
// ---------------------------------------------------------------------------
struct UiAnimKey
{
    f32 time  = 0.0f;
    f32 value = 0.0f;
    i32 easing = 2;   // このキー → 次のキーの区間に適用（UiEase の型番号、既定=イーズアウト）
};

struct UiAnimTrack
{
    // ルート（UIAnimPlayer を持つエンティティ）からの名前パス。"" は自分自身。
    // 例 "Panel/Title"。同名の子が複数いる場合は最初に見つかったものが対象。
    std::string target;
    i32 prop = kUiPropOffsetMinX;
    std::vector<UiAnimKey> keys;   // time 昇順（SortUiAnimClip が保証する）
};

// 再生中に EventBus へ発火するマーカー（足音・SEをアニメに埋め込む用途）。
struct UiAnimEvent
{
    f32 time = 0.0f;
    std::string event;
};

struct UiAnimClip
{
    std::string name;
    f32  duration = 1.0f;
    bool loop = false;
    std::vector<UiAnimTrack> tracks;
    std::vector<UiAnimEvent> events;
};

// ---------------------------------------------------------------------------
// 純関数
// ---------------------------------------------------------------------------

// クリップ内の最終キー/イベント時刻。空なら 0。
inline f32 UiAnimClipEndTime(const UiAnimClip& clip)
{
    f32 end = 0.0f;
    for (const auto& tr : clip.tracks)
        for (const auto& k : tr.keys)
            end = (std::max)(end, k.time);
    for (const auto& ev : clip.events)
        end = (std::max)(end, ev.time);
    return end;
}

// 全トラックのキーを time 昇順に整列し、イベントも整列する。
// duration <= 0 なら最終キー時刻（それも0なら1.0）を duration に採用する。
// 編集直後と読み込み直後に必ず通すこと（評価側は昇順を前提に二分探索する）。
inline void SortUiAnimClip(UiAnimClip& clip)
{
    for (auto& tr : clip.tracks)
    {
        // stable_sort: 同時刻キーの並び（= 後ろ勝ちの解決順）を編集順どおりに保つ
        std::stable_sort(tr.keys.begin(), tr.keys.end(),
                         [](const UiAnimKey& a, const UiAnimKey& b) { return a.time < b.time; });
    }
    std::stable_sort(clip.events.begin(), clip.events.end(),
                     [](const UiAnimEvent& a, const UiAnimEvent& b) { return a.time < b.time; });

    if (clip.duration <= 0.0f)
    {
        const f32 end = UiAnimClipEndTime(clip);
        clip.duration = (end > 0.0f) ? end : 1.0f;
    }
}

// 1トラックを time で評価。
//  - キー無し           → UiAnimPropDefault(tr.prop)
//  - 最初のキーより前   → 最初のキーの値（先頭でホールド）
//  - 最後のキーより後   → 最後のキーの値
//  - step プロパティ    → 直前キーの値をそのまま
//  - それ以外           → 左キーの easing で左右キーを補間
inline f32 EvaluateUiAnimTrack(const UiAnimTrack& tr, f32 time)
{
    const size_t n = tr.keys.size();
    if (n == 0) return UiAnimPropDefault(tr.prop);
    if (n == 1 || time <= tr.keys.front().time) return tr.keys.front().value;
    if (time >= tr.keys.back().time)            return tr.keys.back().value;

    // time 未満の最後のキーを二分探索（keys は昇順）
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1)
    {
        const size_t mid = (lo + hi) / 2;
        if (tr.keys[mid].time <= time) lo = mid; else hi = mid;
    }
    const UiAnimKey& a = tr.keys[lo];
    const UiAnimKey& b = tr.keys[lo + 1];

    if (UiAnimPropIsStep(tr.prop)) return a.value;

    const f32 span = b.time - a.time;
    if (span <= 0.0f) return b.value;                 // 同時刻キーの重なりは後ろ勝ち
    const f32 p = UiEase(a.easing, (time - a.time) / span);
    return a.value + (b.value - a.value) * p;
}

// クリップの再生時刻を [0, duration] に畳む。
//  loop=false: duration で止まる（戻り値 true = 再生完了）
//  loop=true : 剰余で巻き戻す（戻り値は常に false）
// duration <= 0 のクリップは 0 に固定して完了扱いにする（ゼロ除算・無限ループ回避）。
inline bool WrapUiAnimTime(f32& time, f32 duration, bool loop)
{
    if (duration <= 0.0f) { time = 0.0f; return !loop; }
    if (time < 0.0f) time = 0.0f;
    if (!loop)
    {
        if (time >= duration) { time = duration; return true; }
        return false;
    }
    time = std::fmod(time, duration);
    if (time < 0.0f) time += duration;
    return false;
}

// (prevTime, curTime] に入るイベントを out へ積む。
// ループでラップした（curTime < prevTime）フレームは (prev, end] + [0, cur] の2区間を見る。
// 境界は「開いた左・閉じた右」で統一しているので、同じイベントが2回出ることはない。
inline void CollectUiAnimEvents(const UiAnimClip& clip, f32 prevTime, f32 curTime,
                                std::vector<const UiAnimEvent*>& out)
{
    if (clip.events.empty()) return;
    const bool wrapped = curTime < prevTime;
    // ★再生開始フレームだけは区間を閉じる（`>=`）。再生は prevTime=curTime=0 から始まるので、
    //   半開区間 (prev, cur] だと **time=0 のイベントが一度も当たらない**。
    //   ループするクリップは 2 周目から拾えるが、単発クリップは永久に鳴らない。
    //   「メニューが開く瞬間に効果音」という一番自然なマーカーが無言で死ぬ場所だった。
    // ★curTime > prevTime のときだけ。t=0 で止まったまま更新され続ける（prev==cur==0）ケースで
    //   毎フレーム再発火しないようにする。
    const bool atStart = (prevTime <= 0.0f) && (curTime > prevTime);
    for (const auto& ev : clip.events)
    {
        const bool hit = wrapped ? (ev.time > prevTime || ev.time <= curTime)
                                 : ((atStart ? ev.time >= prevTime : ev.time > prevTime)
                                    && ev.time <= curTime);
        if (hit) out.push_back(&ev);
    }
}

// ---------------------------------------------------------------------------
// JSON IO（UiAnimAsset.cpp）
// ---------------------------------------------------------------------------

// JSON バイト列 → UiAnimClip。パース失敗なら false（out は不定）。
// 成功時は SortUiAnimClip 済みで返る。未知フィールドは無視、欠落は既定値。
bool ParseUiAnimClip(const std::vector<uint8_t>& jsonBytes, UiAnimClip& out);

// UiAnimClip → .uianim 用 JSON 文字列（整形あり）。
std::string SerializeUiAnimClip(const UiAnimClip& clip);

} // namespace dx12e
