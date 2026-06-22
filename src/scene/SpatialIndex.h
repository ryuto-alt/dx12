#pragma once
//
// 一様グリッドの空間インデックス（XZ平面）— Phase 5: RTS/大量エンティティ能力。
//
// RTS の矩形選択・近接検索・大量エンティティ列挙を、Jolt 物理に依存せず行うための索引。
// Jolt の body 上限と無関係に数千〜のエンティティを索引できる（非物理の群も索引可能）。
// ヘッダオンリー・GPU 非依存。Scene が Transform から毎フレーム（or ダーティ時）構築して使う想定。
//
#include <cstdint>
#include <cmath>
#include <vector>
#include <unordered_map>

#include <entt/entity/entity.hpp>   // entt::entity

namespace dx12e
{

class SpatialIndex
{
public:
    explicit SpatialIndex(float cellSize = 4.0f)
        : m_cell(cellSize > 1e-4f ? cellSize : 4.0f) {}

    void Clear() { m_cells.clear(); }

    void Insert(entt::entity e, float x, float z)
    {
        m_cells[Key(CellOf(x), CellOf(z))].push_back(Entry{e, x, z});
    }

    // 矩形 [minX,maxX]×[minZ,maxZ] 内のエンティティを out へ追記（厳密判定）。
    void QueryBox(float minX, float minZ, float maxX, float maxZ,
                  std::vector<entt::entity>& out) const
    {
        const int cx0 = CellOf(minX), cx1 = CellOf(maxX);
        const int cz0 = CellOf(minZ), cz1 = CellOf(maxZ);
        for (int cx = cx0; cx <= cx1; ++cx)
            for (int cz = cz0; cz <= cz1; ++cz)
            {
                auto it = m_cells.find(Key(cx, cz));
                if (it == m_cells.end()) continue;
                for (const auto& en : it->second)
                    if (en.x >= minX && en.x <= maxX && en.z >= minZ && en.z <= maxZ)
                        out.push_back(en.e);
            }
    }

    // 中心 (cx,cz) 半径 r の円内のエンティティを out へ追記（厳密判定）。
    void QueryRadius(float cx, float cz, float r,
                     std::vector<entt::entity>& out) const
    {
        const float r2 = r * r;
        const int x0 = CellOf(cx - r), x1 = CellOf(cx + r);
        const int z0 = CellOf(cz - r), z1 = CellOf(cz + r);
        for (int gx = x0; gx <= x1; ++gx)
            for (int gz = z0; gz <= z1; ++gz)
            {
                auto it = m_cells.find(Key(gx, gz));
                if (it == m_cells.end()) continue;
                for (const auto& en : it->second)
                {
                    const float dx = en.x - cx, dz = en.z - cz;
                    if (dx * dx + dz * dz <= r2) out.push_back(en.e);
                }
            }
    }

    std::size_t Size() const
    {
        std::size_t n = 0;
        for (const auto& kv : m_cells) n += kv.second.size();
        return n;
    }

private:
    struct Entry { entt::entity e; float x; float z; };

    int CellOf(float v) const { return static_cast<int>(std::floor(v / m_cell)); }

    static std::uint64_t Key(int cx, int cz)
    {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32)
             |  static_cast<std::uint64_t>(static_cast<std::uint32_t>(cz));
    }

    float m_cell;
    std::unordered_map<std::uint64_t, std::vector<Entry>> m_cells;
};

} // namespace dx12e
