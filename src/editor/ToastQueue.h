#pragma once

// ===== トースト通知のキュー（純ロジック。描画は editor/Toast.cpp）=====
// 依存は標準ライブラリだけ（tests/editor_ux_test.cpp が単体で検証する）。
//
// 仕様:
//   ・種別 Info / Success / Warn / Error。既定の表示時間は 3 秒、Error は 6 秒（seconds <= 0 で既定）。
//   ・同じ種別・同じ本文がまだ表示中なら新しい 1 枚を積まず件数(count)を +1 して寿命を延ばす（連打の洪水を防ぐ）。
//   ・同時に持つのは kMaxItems 枚まで。あふれたら一番古いものから消す。
//   ・Update(dt, hoveredId) : ホバー中の 1 枚は寿命が減らない（読んでいる最中に消えない）。
//   ・Dismiss(id) : クリックで即座に閉じる（フェードは描画側が age/life から計算する）。

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace dx12e::ui
{

enum class ToastKind : uint8_t { Info = 0, Success = 1, Warn = 2, Error = 3 };

struct ToastItem
{
    uint32_t    id    = 0;
    ToastKind   kind  = ToastKind::Info;
    std::string text;
    int         count = 1;        // 畳み込んだ回数（2 以上で「x3」と出す）
    float       age   = 0.0f;     // 表示してからの秒
    float       life  = 3.0f;     // 消えるまでの秒
    bool        dismissed = false; // クリックで閉じた（短いフェードアウト後に取り除く）
    float       dismissAge = 0.0f;
};

class ToastQueue
{
public:
    static constexpr size_t kMaxItems = 6;
    static constexpr float  kDefaultSeconds = 3.0f;
    static constexpr float  kErrorSeconds   = 6.0f;
    static constexpr float  kFadeIn  = 0.15f;
    static constexpr float  kFadeOut = 0.25f;

    static float DefaultLife(ToastKind k) { return k == ToastKind::Error ? kErrorSeconds : kDefaultSeconds; }

    // 積む。戻り値は id（畳み込んだ場合は既存の id）。
    uint32_t Push(ToastKind kind, std::string text, float seconds = -1.0f)
    {
        const float life = seconds > 0.0f ? seconds : DefaultLife(kind);
        for (auto& t : m_items)
        {
            if (!t.dismissed && t.kind == kind && t.text == text)
            {
                ++t.count;
                t.age = 0.0f;                 // 寿命を延ばす
                t.life = (std::max)(t.life, life);
                return t.id;
            }
        }
        ToastItem it;
        it.id = ++m_nextId;
        it.kind = kind;
        it.text = std::move(text);
        it.life = life;
        m_items.push_back(std::move(it));
        while (m_items.size() > kMaxItems) m_items.erase(m_items.begin());
        return m_items.back().id;
    }

    // 時間を進める。寿命を過ぎた / 閉じてフェードが終わったものを取り除く。
    void Update(float dt, uint32_t hoveredId = 0)
    {
        for (auto& t : m_items)
        {
            if (t.dismissed) { t.dismissAge += dt; continue; }
            if (t.id == hoveredId) continue;   // 読んでいる最中は減らさない
            t.age += dt;
        }
        m_items.erase(std::remove_if(m_items.begin(), m_items.end(), [](const ToastItem& t)
        {
            return t.dismissed ? t.dismissAge >= kFadeOut : t.age >= t.life + kFadeOut;
        }), m_items.end());
    }

    bool Dismiss(uint32_t id)
    {
        for (auto& t : m_items)
            if (t.id == id && !t.dismissed) { t.dismissed = true; t.dismissAge = 0.0f; return true; }
        return false;
    }

    void Clear() { m_items.clear(); }

    const std::vector<ToastItem>& Items() const { return m_items; }
    // まだ閉じていない（フェードアウト中を除く）枚数。
    size_t LiveCount() const
    {
        return static_cast<size_t>(std::count_if(m_items.begin(), m_items.end(),
            [](const ToastItem& t) { return !t.dismissed && t.age < t.life; }));
    }

    // 描画用の不透明度 0..1（フェードイン → 1 → フェードアウト）。
    static float Alpha(const ToastItem& t)
    {
        if (t.dismissed) return (std::max)(0.0f, 1.0f - t.dismissAge / kFadeOut);
        const float in  = (std::min)(1.0f, t.age / kFadeIn);
        const float out = t.age <= t.life ? 1.0f : (std::max)(0.0f, 1.0f - (t.age - t.life) / kFadeOut);
        return (std::min)(in, out);
    }

private:
    std::vector<ToastItem> m_items;
    uint32_t m_nextId = 0;
};

} // namespace dx12e::ui
