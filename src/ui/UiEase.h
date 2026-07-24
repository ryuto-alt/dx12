#pragma once

#include <algorithm>
#include <cmath>

namespace dx12e
{

// イージング（p: 0..1 → 0..1）。純関数・GPU/ECS 非依存 → 単体テスト可能。
// 列挙は UIAnimator::showEasing / UiTween::easing / UiAnimKey::easing で共通:
//   0=リニア 1=イーズイン 2=イーズアウト 3=イン/アウト 4=バック(勢い) 5=バウンス 6=弾性
//   7=エクスポ(鋭く減速) 8=インバック(溜めて発進) 9=イン/アウトバック 10=クイント(強い減速)
//   11=サイン(ゆったり対称)
// 新しい型番号を足すときは kUiEaseCount と kUiEaseNames も必ず揃えること
// （タイムラインエディタのコンボと ScriptEngine の文字列マップがこれを引く）。
constexpr int kUiEaseCount = 12;

inline const char* const* UiEaseNames()
{
    static const char* names[kUiEaseCount] = {
        "リニア", "イーズイン", "イーズアウト", "イン/アウト",
        "バック", "バウンス", "弾性", "エクスポ",
        "インバック", "イン/アウトバック", "クイント", "サイン",
    };
    return names;
}

inline float UiEase(int type, float p)
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

} // namespace dx12e
