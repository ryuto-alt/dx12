// ============================================================================
// ParamPool.h — パラメータプールのレコード割り当て（純ロジック。GPU 非依存）
//
//   グラフ材質 1 インスタンス = プール内の連続した float4 の並び（レコード）。CompileResult::slotCount が長さ。
//   先頭の float4 番号（recordBase）が b2 の DWORD 0 に載る（ForwardGraph.hlsl の cbuffer GraphMaterial）。
//   割り当ては first-fit + 解放時の隣接マージ。決定論（同じ操作列 → 同じオフセット）。
//   規則（テスト: tests/matgraph_runtime_test.cpp）:
//     ・長さ 0 は割り当てない（false）。
//     ・最初の割り当ては 0 から詰める。解放した穴は次の割り当てが先頭から探して再利用する。
//     ・容量を超えたら false（呼び出し側はグラフ材質を使わない = 従来材質へ縮退）。
// ============================================================================
#pragma once

#include <cstdint>
#include <iterator>
#include <map>

namespace dx12e::matgraph
{

class ParamPoolAllocator
{
public:
    explicit ParamPoolAllocator(uint32_t capacity = 0) { Reset(capacity); }

    void Reset(uint32_t capacity)
    {
        m_capacity = capacity;
        m_used = 0;
        m_highWater = 0;
        m_free.clear();
        if (capacity > 0) m_free[0] = capacity;
    }

    // count 個の連続スロットを確保して先頭を base に返す
    bool Alloc(uint32_t count, uint32_t& base)
    {
        if (count == 0) return false;
        for (auto it = m_free.begin(); it != m_free.end(); ++it)
        {
            if (it->second < count) continue;
            base = it->first;
            const uint32_t restStart = it->first + count;
            const uint32_t restLen   = it->second - count;
            m_free.erase(it);
            if (restLen > 0) m_free[restStart] = restLen;
            m_used += count;
            if (base + count > m_highWater) m_highWater = base + count;
            return true;
        }
        return false;
    }

    // 解放（隣接する空きとマージ）。二重解放・範囲外は false
    bool Free(uint32_t base, uint32_t count)
    {
        if (count == 0 || base + count > m_capacity) return false;
        // 既存の空きと重なっていないか
        auto next = m_free.lower_bound(base);
        if (next != m_free.end() && next->first < base + count) return false;
        if (next != m_free.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second > base) return false;
        }
        uint32_t nb = base, nc = count;
        if (next != m_free.end() && next->first == base + count)
        {
            nc += next->second;
            next = m_free.erase(next);
        }
        if (next != m_free.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == nb)
            {
                nb = prev->first;
                nc += prev->second;
                m_free.erase(prev);
            }
        }
        m_free[nb] = nc;
        m_used -= count;
        return true;
    }

    uint32_t Capacity()  const { return m_capacity; }
    uint32_t Used()      const { return m_used; }
    uint32_t HighWater() const { return m_highWater; }   // これまでに使った最大の末尾（GPU へ転送する範囲の上限）
    size_t   FreeRanges() const { return m_free.size(); }

private:
    uint32_t m_capacity = 0;
    uint32_t m_used = 0;
    uint32_t m_highWater = 0;
    std::map<uint32_t, uint32_t> m_free;   // 先頭 → 長さ
};

} // namespace dx12e::matgraph
