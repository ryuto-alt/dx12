#pragma once

// ===== text::EllipsizeEnd — UTF-8 の文字列を幅に収まるよう末尾「…」で省略する（W1-A5 / RM-A3）=====
//
//   const std::string shown = text::EllipsizeEnd(name, maxW, ImGui::GetFont(), ImGui::GetFontSize());
//
// ★バイトではなく「書記素に近い塊（クラスタ）」の境界でしか切らない。
//     ・UTF-8 の多バイト文字の途中で切らない（旧: std::string::pop_back() で日本語が壊れた）。
//     ・結合文字（濁点・アクセント）、異体字選択子（VS）、肌色修飾子、ZWJ 連結（👨‍👩‍👧）、国旗（地域指示子 2 つ）は
//       直前の文字から切り離さない。
//     ・不正な UTF-8 のバイトは 1 バイトずつの塊として扱う（落ちない・無限ループしない）。
// ★計測は O(log n) 回（全文 1 回 + 「…」1 回 + 二分探索）。結果は LRU 256 件でキャッシュする。
// ★「…」は prefix の幅 + 「…」の幅で見積もる（字間の調整は無視。結果の幅は maxW 以下になる側に倒れる）。
// ★「…」だけでも maxW に入らないときは空文字列を返す。
// ★戻りは std::string（PP は string_view だったが、LRU の追い出しで view が無効になるのを避けて値で返す。
//   短い名前のコピーのみで、計測のキャッシュで得をする部分は変わらない）。
// ★フォント（アトラス）を作り直したら text::ClearEllipsizeCache() を呼ぶこと（キーは ImFont::FontId + サイズ + 幅）。
//
// アルゴリズム本体（FitEndBytes）は計測関数を受け取るテンプレートで、ImGui・フォント無しでテストできる。

#include <imgui.h>

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dx12e::text
{

inline constexpr const char* kEllipsis = "\xE2\x80\xA6";   // U+2026 …

namespace detail
{
struct Cp { char32_t cp; unsigned len; };

// UTF-8 を 1 文字デコードする。不正なら U+FFFD・長さ 1（必ず前へ進む）。
inline Cp DecodeUtf8(const char* p, const char* end)
{
    const unsigned char c = static_cast<unsigned char>(*p);
    const auto cont = [&](int i) { return p + i < end && (static_cast<unsigned char>(p[i]) & 0xC0) == 0x80; };
    if (c < 0x80) return { c, 1 };
    if (c >= 0xC2 && c <= 0xDF && cont(1))
        return { static_cast<char32_t>(((c & 0x1Fu) << 6) | (p[1] & 0x3Fu)), 2 };
    if (c >= 0xE0 && c <= 0xEF && cont(1) && cont(2))
    {
        const char32_t cp = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) return { cp, 3 };
    }
    if (c >= 0xF0 && c <= 0xF4 && cont(1) && cont(2) && cont(3))
    {
        const char32_t cp = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        if (cp >= 0x10000 && cp <= 0x10FFFF) return { cp, 4 };
    }
    return { 0xFFFD, 1 };
}

// 直前の文字にくっつく文字（結合文字・選択子・修飾子・ZWJ / ZWNJ）。
inline bool IsExtend(char32_t c)
{
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) || (c >= 0x0591 && c <= 0x05BD)
        || (c >= 0x0610 && c <= 0x061A) || (c >= 0x064B && c <= 0x065F) || c == 0x0E31
        || (c >= 0x0E34 && c <= 0x0E3A) || (c >= 0x0E47 && c <= 0x0E4E)
        || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF)
        || c == 0x200C || c == 0x200D || (c >= 0x20D0 && c <= 0x20FF)
        || c == 0x3099 || c == 0x309A || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F)
        || c == 0xFF9E || c == 0xFF9F
        || (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F) || (c >= 0xE0100 && c <= 0xE01EF);
}
inline bool IsRegionalIndicator(char32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }

// クラスタの先頭バイト位置を並べる（最後に s.size() を足す。結果は「k 個目の境界」= そこまでが k 塊）。
inline void ClusterBoundaries(std::string_view s, std::vector<size_t>& out)
{
    out.clear();
    const char* b = s.data();
    const char* end = b + s.size();
    const char* p = b;
    bool prevZwj = false;
    int riRun = 0;             // 直前のクラスタが（対になっていない）地域指示子 1 つなら 1
    bool haveCluster = false;
    while (p < end)
    {
        const Cp d = DecodeUtf8(p, end);
        const bool ri = IsRegionalIndicator(d.cp);
        bool join = false;
        if (haveCluster)
        {
            if (IsExtend(d.cp) || prevZwj) join = true;
            else if (ri && riRun == 1) join = true;   // 国旗の 2 つ目
        }
        if (!join)
        {
            out.push_back(static_cast<size_t>(p - b));
            haveCluster = true;
            riRun = ri ? 1 : 0;
        }
        else if (ri && riRun == 1)
        {
            riRun = 2;   // 対が完成（次の地域指示子は新しい国旗）
        }
        prevZwj = (d.cp == 0x200D);
        p += d.len;
    }
    out.push_back(s.size());
}
} // namespace detail

struct FitResult
{
    size_t keepBytes = 0;     // 残す先頭のバイト数（クラスタの境界）
    bool   ellipsis = false;  // 末尾に「…」を付ける必要があるか
    bool   fits = true;       // false: 「…」だけでも入らない（呼び出し側は空にする）
    int    measureCalls = 0;  // 計測関数を呼んだ回数（テスト用）
};

// measure(std::string_view) -> 幅（px）。単調（長いほど広い）と仮定する。
template <class Measure>
FitResult FitEndBytes(std::string_view s, float maxW, Measure&& measure)
{
    FitResult r;
    r.keepBytes = s.size();
    if (s.empty()) return r;
    ++r.measureCalls;
    if (measure(s) <= maxW) return r;                       // そのまま入る
    r.ellipsis = true;
    ++r.measureCalls;
    const float ellW = measure(std::string_view(kEllipsis));
    if (ellW > maxW) { r.keepBytes = 0; r.fits = false; return r; }

    std::vector<size_t> bounds;
    detail::ClusterBoundaries(s, bounds);                   // bounds[k] = k 塊目までのバイト数（bounds[0] = 0）
    size_t lo = 0, hi = bounds.size() - 1;                  // lo: 入る（0 塊は「…」だけ）/ hi: 全文（入らないと分かっている）
    while (lo + 1 < hi)
    {
        const size_t mid = (lo + hi) / 2;
        ++r.measureCalls;
        if (measure(s.substr(0, bounds[mid])) + ellW <= maxW) lo = mid; else hi = mid;
    }
    r.keepBytes = bounds[lo];
    return r;
}

namespace detail
{
struct CacheKey
{
    std::string text;
    uint32_t fontId = 0;
    uint32_t sizeBits = 0;
    int32_t  widthQ = 0;     // maxW を 1/4 px に切り捨てた値（結果は常に maxW 以下の側へ倒れる）
    bool operator==(const CacheKey& o) const
    { return fontId == o.fontId && sizeBits == o.sizeBits && widthQ == o.widthQ && text == o.text; }
};
struct CacheKeyHash
{
    size_t operator()(const CacheKey& k) const
    {
        size_t h = std::hash<std::string_view>{}(k.text);
        const auto mix = [&](uint64_t v) { h ^= static_cast<size_t>(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
        mix(k.fontId); mix(k.sizeBits); mix(static_cast<uint32_t>(k.widthQ));
        return h;
    }
};
struct Cache
{
    static constexpr size_t kCapacity = 256;
    using Entry = std::pair<CacheKey, std::string>;
    std::list<Entry> lru;   // 先頭 = 最近使った
    std::unordered_map<CacheKey, std::list<Entry>::iterator, CacheKeyHash> map;
    uint64_t hits = 0, misses = 0;
};
inline Cache& TheCache() { static Cache c; return c; }
} // namespace detail

inline void ClearEllipsizeCache()
{
    detail::Cache& c = detail::TheCache();
    c.lru.clear(); c.map.clear(); c.hits = c.misses = 0;
}
inline size_t EllipsizeCacheSize() { return detail::TheCache().map.size(); }
inline uint64_t EllipsizeCacheHits() { return detail::TheCache().hits; }
inline uint64_t EllipsizeCacheMisses() { return detail::TheCache().misses; }

// font のサイズ size（物理 px）で utf8 を maxW（物理 px）に収める。収まるならそのまま。
inline std::string EllipsizeEnd(std::string_view utf8, float maxW, ImFont* font, float size)
{
    if (utf8.empty()) return std::string();
    if (!font) font = ImGui::GetFont();
    detail::Cache& c = detail::TheCache();

    detail::CacheKey key;
    key.text.assign(utf8.data(), utf8.size());
    key.fontId = font ? static_cast<uint32_t>(font->FontId) : 0u;
    std::memcpy(&key.sizeBits, &size, sizeof(float));
    key.widthQ = static_cast<int32_t>(maxW <= 0.0f ? -1.0f : std::floor(maxW * 4.0f));

    if (const auto it = c.map.find(key); it != c.map.end())
    {
        c.lru.splice(c.lru.begin(), c.lru, it->second);
        ++c.hits;
        return it->second->second;
    }
    ++c.misses;

    // キーの幅（1/4 px 切り捨て）で計算する = キャッシュ命中時と未命中時で結果が一致する。
    const float usableW = static_cast<float>(key.widthQ) * 0.25f;
    const auto measure = [&](std::string_view sv) -> float
    {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, sv.data(), sv.data() + sv.size()).x;
    };
    const FitResult fr = FitEndBytes(utf8, usableW, measure);
    std::string out;
    if (!fr.fits) out.clear();
    else
    {
        out.assign(utf8.data(), fr.keepBytes);
        if (fr.ellipsis) out += kEllipsis;
    }

    c.lru.emplace_front(std::move(key), out);
    c.map[c.lru.front().first] = c.lru.begin();
    if (c.lru.size() > detail::Cache::kCapacity)
    {
        c.map.erase(c.lru.back().first);
        c.lru.pop_back();
    }
    return out;
}

} // namespace dx12e::text
