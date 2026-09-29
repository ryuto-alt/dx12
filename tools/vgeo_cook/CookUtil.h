#pragma once
//
// CookUtil.h ― cooker 共通の小さな道具（スレッド / 並列ソート / 基数ソート / Morton / メモリ計測）。標準ライブラリのみ。
//
//   ★決定論: ParallelFor は「作業項目の添字」で結果を書く前提（スレッド数や実行順に依存しない）。ParallelSort は
//     比較関数が全順序（同値を作らない）であれば、ブロック数によらず同じ結果になる。
//   ★ユーザーの PC を占有しない: 既定スレッド数は論理コア / 4。ワーカーは BelowNormal 優先度で走らせる。
//
#include "renderer/vg/VgeoFormat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
#endif

namespace dx12e::vg::cook
{

inline u32 HardwareThreads()
{
    const u32 n = std::thread::hardware_concurrency();
    return n ? n : 4u;
}
// 既定 = 論理コア / 4（28 論理コアなら 7）。PC を占有しない。
inline u32 DefaultThreadCount() { return std::max(1u, HardwareThreads() / 4u); }

inline double MsSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ── メモリ計測（Windows）────────────────────────────────────────────────────────
inline u64 ProcessWorkingSetBytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return static_cast<u64>(pmc.WorkingSetSize);
#endif
    return 0;
}
inline u64 ProcessPeakWorkingSetBytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return static_cast<u64>(pmc.PeakWorkingSetSize);
#endif
    return 0;
}
inline u64 ProcessPeakPrivateBytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) return static_cast<u64>(pmc.PeakPagefileUsage);
#endif
    return 0;
}
inline void SetProcessBelowNormal()
{
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif
}
inline void SetThreadBelowNormal()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

// ── 並列ループ ────────────────────────────────────────────────────────────────
// fn(begin, end, threadIndex) を [0, count) の連続範囲（grain 個ずつ）に対して動的に割り当てて呼ぶ。
// 例外はワーカー内で捕まえ、最初の 1 つを join 後に呼び出し元へ再送出する。
template <class F>
void ParallelFor(size_t count, u32 threads, size_t grain, bool lowPriority, F&& fn)
{
    if (count == 0) return;
    grain = std::max<size_t>(grain, 1);
    const size_t chunks = (count + grain - 1) / grain;
    threads = static_cast<u32>(std::min<size_t>(std::max(threads, 1u), chunks));
    if (threads <= 1) { fn(size_t{0}, count, 0u); return; }
    std::atomic<size_t> next{0};
    std::exception_ptr err;
    std::mutex errMutex;
    auto worker = [&](u32 tid, bool setPrio)
    {
        if (setPrio && lowPriority) SetThreadBelowNormal();
        try
        {
            for (;;)
            {
                const size_t b = next.fetch_add(grain, std::memory_order_relaxed);
                if (b >= count) break;
                fn(b, std::min(count, b + grain), tid);
            }
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lk(errMutex);
            if (!err) err = std::current_exception();
            next.store(count);
        }
    };
    std::vector<std::thread> ts;
    ts.reserve(threads - 1);
    for (u32 t = 1; t < threads; ++t) ts.emplace_back(worker, t, true);
    worker(0, false);
    for (auto& t : ts) t.join();
    if (err) std::rethrow_exception(err);
}

// 比較関数が全順序ならスレッド数に依らず同じ結果。ブロックごとに std::sort → 2 冪幅で併合。
template <class T, class Cmp>
void ParallelSort(std::vector<T>& v, Cmp cmp, u32 threads, bool lowPriority)
{
    const size_t n = v.size();
    if (n < 100000 || threads <= 1) { std::sort(v.begin(), v.end(), cmp); return; }
    size_t blocks = 1;
    while (blocks < static_cast<size_t>(threads) * 2) blocks <<= 1;
    const size_t per = (n + blocks - 1) / blocks;
    ParallelFor(blocks, threads, 1, lowPriority, [&](size_t b, size_t e, u32)
    {
        for (size_t k = b; k < e; ++k)
        {
            const size_t lo = std::min(n, k * per), hi = std::min(n, lo + per);
            std::sort(v.begin() + static_cast<std::ptrdiff_t>(lo), v.begin() + static_cast<std::ptrdiff_t>(hi), cmp);
        }
    });
    for (size_t width = per; width < n; width *= 2)
    {
        const size_t merges = (n + 2 * width - 1) / (2 * width);
        ParallelFor(merges, threads, 1, lowPriority, [&](size_t b, size_t e, u32)
        {
            for (size_t k = b; k < e; ++k)
            {
                const size_t lo = k * 2 * width, mid = std::min(n, lo + width), hi = std::min(n, lo + 2 * width);
                if (mid < hi) std::inplace_merge(v.begin() + static_cast<std::ptrdiff_t>(lo), v.begin() + static_cast<std::ptrdiff_t>(mid), v.begin() + static_cast<std::ptrdiff_t>(hi), cmp);
            }
        });
    }
}

// (key32 << 32 | value32) の配列を key で安定に基数ソート（LSD・11bit×3。key は 32bit のうち下位 keyBits ビット）。
inline void RadixSortByKey32(std::vector<u64>& a, u32 keyBits)
{
    const size_t n = a.size();
    if (n < 2) return;
    std::vector<u64> tmp(n);
    u64* src = a.data();
    u64* dst = tmp.data();
    for (u32 shift = 0; shift < keyBits; shift += 11)
    {
        size_t hist[2048] = {};
        for (size_t i = 0; i < n; ++i) ++hist[((src[i] >> 32) >> shift) & 2047u];
        size_t sum = 0;
        for (size_t d = 0; d < 2048; ++d) { const size_t c = hist[d]; hist[d] = sum; sum += c; }
        for (size_t i = 0; i < n; ++i) dst[hist[((src[i] >> 32) >> shift) & 2047u]++] = src[i];
        std::swap(src, dst);
    }
    if (src != a.data()) std::memcpy(a.data(), src, n * sizeof(u64));
}

// ── Morton ────────────────────────────────────────────────────────────────────
inline u32 Spread10(u32 x)
{
    x &= 0x3FFu;
    x = (x | (x << 16)) & 0x030000FFu;
    x = (x | (x << 8)) & 0x0300F00Fu;
    x = (x | (x << 4)) & 0x030C30C3u;
    x = (x | (x << 2)) & 0x09249249u;
    return x;
}
inline u32 Morton30(u32 x, u32 y, u32 z) { return Spread10(x) | (Spread10(y) << 1) | (Spread10(z) << 2); }
inline u64 Spread21(u64 x)
{
    x &= 0x1FFFFFull;
    x = (x | x << 32) & 0x1F00000000FFFFull;
    x = (x | x << 16) & 0x1F0000FF0000FFull;
    x = (x | x << 8) & 0x100F00F00F00F00Full;
    x = (x | x << 4) & 0x10C30C30C30C30C3ull;
    x = (x | x << 2) & 0x1249249249249249ull;
    return x;
}
inline u64 Morton63(u32 x, u32 y, u32 z) { return Spread21(x) | (Spread21(y) << 1) | (Spread21(z) << 2); }

// 点を [lo, hi] の箱に正規化して 21bit/軸 の Morton コードにする（箱が縮退していれば 0）。
inline u64 MortonOfPoint(const f32 p[3], const f64 lo[3], const f64 inv[3])
{
    u32 c[3];
    for (int a = 0; a < 3; ++a)
    {
        f64 t = (static_cast<f64>(p[a]) - lo[a]) * inv[a];
        t = std::min(1.0, std::max(0.0, t));
        c[a] = static_cast<u32>(t * 2097151.0);
    }
    return Morton63(c[0], c[1], c[2]);
}

// ── 小さな開番地ハッシュ（グローバル頂点 ID → ローカル ID。世代スタンプで再利用）────────────────
class LocalMap
{
public:
    void Reset(size_t expected)
    {
        size_t want = 16;
        while (want < expected * 2) want <<= 1;
        if (want > m_keys.size()) { m_keys.assign(want, 0); m_vals.assign(want, 0); m_stamp.assign(want, 0); m_gen = 0; }
        m_mask = m_keys.size() - 1;
        if (++m_gen == 0) { std::fill(m_stamp.begin(), m_stamp.end(), 0u); m_gen = 1; }
    }
    // key が無ければ nextVal を登録して added = true。
    u32 GetOrAdd(u32 key, u32 nextVal, bool& added)
    {
        size_t h = (static_cast<u64>(key) * 0x9E3779B97F4A7C15ull) >> 20;
        for (;; ++h)
        {
            const size_t s = h & m_mask;
            if (m_stamp[s] != m_gen) { m_stamp[s] = m_gen; m_keys[s] = key; m_vals[s] = nextVal; added = true; return nextVal; }
            if (m_keys[s] == key) { added = false; return m_vals[s]; }
        }
    }
private:
    std::vector<u32> m_keys, m_vals, m_stamp;
    size_t m_mask = 0;
    u32 m_gen = 0;
};

} // namespace dx12e::vg::cook
