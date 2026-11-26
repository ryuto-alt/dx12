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
// ★UI スレッド専用（キャッシュは static・ロック無し）。キーは ImFont のポインタ + サイズ + 幅 + 計測方式なので、書体を切り替えても古い結果は返らない（ただし ImFont を解放して同じアドレスに作り直す場合は別。フォントを再構築する箇所では ClearEllipsizeCache() を呼ぶこと）
//   （アトラスを作り直しても同じ書体・同じサイズの字幅は変わらない）。明示的に捨てたいときは text::ClearEllipsizeCache()。
// ★matchCalcTextSize = true は旧アセットブラウザと同じ判定（CalcTextSize の切り上げ幅・最低 2 バイト）。
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

// ImGui::CalcTextSize と同じ丸め（imgui 1.92: IM_TRUNC(x + 0.99999f) = 整数 px へ切り上げ）。
inline float CeilLikeCalcTextSize(float x) { return std::floor(x + 0.99999f); }

// 旧アセットブラウザの省略ループ（CalcTextSize で測り、末尾から 1 バイトずつ削る）と同じ判定をクラスタ境界で行う版。
//   ・全文が収まる（ceil 幅 <= maxW）ならそのまま。
//   ・収まらなければ、(prefix + "…") の幅（結合して測る）が収まる最長の prefix。最低 2 バイトは残す。
// measureJoined(std::string_view prefix, bool withEllipsis) -> 幅（丸め済み）。ASCII では旧ループと結果が一致する。
template <class MeasureJoined>
FitResult FitEndBytesLegacy(std::string_view s, float maxW, MeasureJoined&& measureJoined)
{
    FitResult r;
    r.keepBytes = s.size();
    if (s.empty()) return r;
    ++r.measureCalls;
    if (measureJoined(s, false) <= maxW) return r;
    r.ellipsis = true;
    std::vector<size_t> bounds;
    detail::ClusterBoundaries(s, bounds);
    const size_t n = bounds.size() - 1;                     // 塊の数
    size_t kmin = 1;                                        // 2 バイト以上を残す最小の塊数
    while (kmin < n && bounds[kmin] < 2) ++kmin;
    if (n <= kmin || bounds[kmin] >= s.size()) { r.keepBytes = s.size(); return r; }   // 旧: size<=2 は削らず「…」だけ足す
    ++r.measureCalls;
    if (measureJoined(s.substr(0, bounds[kmin]), true) > maxW) { r.keepBytes = bounds[kmin]; return r; }
    size_t lo = kmin, hi = n;                               // lo: 入る / hi: 入らない（全文 + 「…」は入らない前提）
    while (lo + 1 < hi)
    {
        const size_t mid = (lo + hi) / 2;
        ++r.measureCalls;
        if (measureJoined(s.substr(0, bounds[mid]), true) <= maxW) lo = mid; else hi = mid;
    }
    r.keepBytes = bounds[lo];
    return r;
}

namespace detail
{
struct CacheKey
{
    std::string text;
    uintptr_t fontPtr = 0;
    uint32_t sizeBits = 0;
    int32_t  widthQ = 0;     // 既定: maxW を 1/4 px に切り捨て / legacy: 整数 px に切り捨て（整数幅との比較と等価）
    bool     legacy = false;
};
struct CacheKeyView
{
    std::string_view text;
    uintptr_t fontPtr; uint32_t sizeBits; int32_t widthQ; bool legacy;
};
inline size_t HashKey(std::string_view text, uintptr_t fp, uint32_t sb, int32_t wq, bool lg)
{
    size_t h = std::hash<std::string_view>{}(text);
    const auto mix = [&](uint64_t v) { h ^= static_cast<size_t>(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
    mix(fp); mix(sb); mix(static_cast<uint32_t>(wq)); mix(lg ? 1u : 0u);
    return h;
}
struct CacheKeyHash
{
    using is_transparent = void;
    size_t operator()(const CacheKey& k) const { return HashKey(k.text, k.fontPtr, k.sizeBits, k.widthQ, k.legacy); }
    size_t operator()(const CacheKeyView& k) const { return HashKey(k.text, k.fontPtr, k.sizeBits, k.widthQ, k.legacy); }
};
struct CacheKeyEq
{
    using is_transparent = void;
    template <class A, class B>
    bool operator()(const A& a, const B& b) const
    { return a.fontPtr == b.fontPtr && a.sizeBits == b.sizeBits && a.widthQ == b.widthQ && a.legacy == b.legacy && a.text == b.text; }
};
struct Cache
{
    static constexpr size_t kCapacity = 256;
    using Entry = std::pair<CacheKey, std::string>;
    std::list<Entry> lru;   // 先頭 = 最近使った
    std::unordered_map<CacheKey, std::list<Entry>::iterator, CacheKeyHash, CacheKeyEq> map;
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

// font のサイズ size（物理 px）で utf8 を maxW（物理 px）に収める。収まるならそのまま。UI スレッド専用（ロック無し）。
// matchCalcTextSize = true: 旧 UI（ImGui::CalcTextSize の切り上げ幅 + 末尾バイト削り）と同じ判定にする（アセットブラウザ用）。
inline std::string EllipsizeEnd(std::string_view utf8, float maxW, ImFont* font, float size, bool matchCalcTextSize = false)
{
    if (utf8.empty()) return std::string();
    if (!font) font = ImGui::GetFont();
    detail::Cache& c = detail::TheCache();

    detail::CacheKeyView kv{ utf8, reinterpret_cast<uintptr_t>(font), 0u, 0, matchCalcTextSize };
    std::memcpy(&kv.sizeBits, &size, sizeof(float));
    const float q = matchCalcTextSize ? 1.0f : 4.0f;
    kv.widthQ = static_cast<int32_t>(maxW <= 0.0f ? -1.0f : std::floor(maxW * q));

    if (const auto it = c.map.find(kv); it != c.map.end())
    {
        c.lru.splice(c.lru.begin(), c.lru, it->second);
        ++c.hits;
        return it->second->second;
    }
    ++c.misses;

    // キーの幅（切り捨て）で計算する = キャッシュ命中時と未命中時で結果が一致する。
    const float usableW = static_cast<float>(kv.widthQ) / q;
    std::string out;
    if (matchCalcTextSize)
    {
        std::string tmp;
        const auto measure = [&](std::string_view sv, bool ell) -> float
        {
            tmp.assign(sv.data(), sv.size());
            if (ell) tmp += kEllipsis;
            return CeilLikeCalcTextSize(font->CalcTextSizeA(size, FLT_MAX, 0.0f, tmp.c_str(), tmp.c_str() + tmp.size()).x);
        };
        const FitResult fr = FitEndBytesLegacy(utf8, usableW, measure);
        out.assign(utf8.data(), fr.keepBytes);
        if (fr.ellipsis) out += kEllipsis;
    }
    else
    {
        const auto measure = [&](std::string_view sv) -> float
        {
            return font->CalcTextSizeA(size, FLT_MAX, 0.0f, sv.data(), sv.data() + sv.size()).x;
        };
        const FitResult fr = FitEndBytes(utf8, usableW, measure);
        if (fr.fits)
        {
            out.assign(utf8.data(), fr.keepBytes);
            if (fr.ellipsis) out += kEllipsis;
        }
    }

    detail::CacheKey key{ std::string(utf8), kv.fontPtr, kv.sizeBits, kv.widthQ, kv.legacy };
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
