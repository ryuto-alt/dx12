#pragma once

#include <vector>

#include "core/Types.h"

namespace dx12e
{

// 連続レンジ方式のインデックスアロケータ。
//
// [0, capacity) のインデックス空間を管理し、単一/連続ブロックの確保・解放と
// 隣接フリーレンジの自動結合(coalesce)を行う。GPU/D3D12 への依存を一切持たない
// 純粋なデータ構造なので、デバイス無しで単体テストできる。
//
// DescriptorHeap がこれを内部で保持し、線形バンプ確保(=解放不能でリーク)を置き換える。
// 解放を提供することで、実行時のアセット動的ロード/アンロードでヒープが枯渇しなくなる。
class IndexAllocator
{
public:
    // 確保失敗を表すセンチネル。既存コードの UINT32_MAX / 0xFFFFFFFF と一致させてある。
    static constexpr u32 kInvalid = 0xFFFFFFFFu;

    IndexAllocator() = default;
    explicit IndexAllocator(u32 capacity) { Initialize(capacity); }

    void Initialize(u32 capacity)
    {
        m_capacity = capacity;
        m_free.clear();
        if (capacity > 0)
        {
            m_free.push_back(Range{ 0, capacity });
        }
    }

    // count 個の連続インデックスを確保し、先頭インデックスを返す。
    // 連続領域が見つからない(枯渇 or 断片化)場合は kInvalid。
    u32 AllocateBlock(u32 count)
    {
        if (count == 0)
        {
            return kInvalid;
        }

        for (size_t i = 0; i < m_free.size(); ++i)
        {
            if (m_free[i].count >= count)
            {
                const u32 start = m_free[i].start;
                if (m_free[i].count == count)
                {
                    m_free.erase(m_free.begin() + static_cast<ptrdiff_t>(i));
                }
                else
                {
                    m_free[i].start += count;
                    m_free[i].count -= count;
                }
                return start;
            }
        }
        return kInvalid;
    }

    // 単一インデックスを確保。枯渇時は kInvalid。
    u32 Allocate() { return AllocateBlock(1); }

    // AllocateBlock で確保したブロックを返却。隣接フリーレンジと結合する。
    void FreeBlock(u32 start, u32 count)
    {
        if (count == 0 || start == kInvalid)
        {
            return;
        }

        // start 昇順を保つ挿入位置を線形探索(フリーレンジ数は小さい想定)
        size_t i = 0;
        while (i < m_free.size() && m_free[i].start < start)
        {
            ++i;
        }
        m_free.insert(m_free.begin() + static_cast<ptrdiff_t>(i), Range{ start, count });

        // 直後のレンジと結合
        if (i + 1 < m_free.size() &&
            m_free[i].start + m_free[i].count == m_free[i + 1].start)
        {
            m_free[i].count += m_free[i + 1].count;
            m_free.erase(m_free.begin() + static_cast<ptrdiff_t>(i + 1));
        }

        // 直前のレンジと結合
        if (i > 0 &&
            m_free[i - 1].start + m_free[i - 1].count == m_free[i].start)
        {
            m_free[i - 1].count += m_free[i].count;
            m_free.erase(m_free.begin() + static_cast<ptrdiff_t>(i));
        }
    }

    // 単一インデックスを返却。
    void Free(u32 index) { FreeBlock(index, 1); }

    u32  Capacity()  const { return m_capacity; }

    u32 FreeCount() const
    {
        u32 n = 0;
        for (const Range& r : m_free)
        {
            n += r.count;
        }
        return n;
    }

    u32 UsedCount() const { return m_capacity - FreeCount(); }

    // フリーレンジ数。0 か 1 なら断片化していない。テスト/診断用。
    size_t FreeRangeCount() const { return m_free.size(); }

private:
    struct Range
    {
        u32 start;
        u32 count;
    };

    std::vector<Range> m_free;
    u32                m_capacity = 0;
};

} // namespace dx12e
