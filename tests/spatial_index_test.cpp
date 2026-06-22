// SpatialIndex（一様グリッド空間インデックス）の単体テスト — Phase 5: RTS 能力。
//
// 矩形選択・近接検索・列挙が正しく厳密判定されることを確認する。GPU/物理 非依存。
//
// 実行: ctest --output-on-failure

#include "scene/SpatialIndex.h"

#include <entt/entt.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

entt::entity E(std::uint32_t i) { return static_cast<entt::entity>(i); }

bool Has(const std::vector<entt::entity>& v, entt::entity e)
{
    return std::find(v.begin(), v.end(), e) != v.end();
}
} // namespace

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

int main()
{
    SpatialIndex idx(4.0f);
    idx.Insert(E(1),  0.0f,  0.0f);
    idx.Insert(E(2),  3.0f,  3.0f);
    idx.Insert(E(3), 10.0f, 10.0f);
    idx.Insert(E(4), -5.0f, -5.0f);
    CHECK(idx.Size() == 4);

    // 矩形 [-1,5]×[-1,5] → e1,e2（セル境界をまたいでも厳密判定で拾える）
    {
        std::vector<entt::entity> out;
        idx.QueryBox(-1.0f, -1.0f, 5.0f, 5.0f, out);
        CHECK(out.size() == 2);
        CHECK(Has(out, E(1)));
        CHECK(Has(out, E(2)));
        CHECK(!Has(out, E(3)));
        CHECK(!Has(out, E(4)));
    }

    // 円 中心(0,0) r=4.5 → e1(d=0), e2(d≈4.24<4.5)。e4(d≈7.07>4.5)は除外
    {
        std::vector<entt::entity> out;
        idx.QueryRadius(0.0f, 0.0f, 4.5f, out);
        CHECK(out.size() == 2);
        CHECK(Has(out, E(1)));
        CHECK(Has(out, E(2)));
        CHECK(!Has(out, E(4)));
    }

    // 遠方矩形 → e3 のみ
    {
        std::vector<entt::entity> out;
        idx.QueryBox(8.0f, 8.0f, 12.0f, 12.0f, out);
        CHECK(out.size() == 1);
        CHECK(Has(out, E(3)));
    }

    // 空領域
    {
        std::vector<entt::entity> out;
        idx.QueryBox(100.0f, 100.0f, 200.0f, 200.0f, out);
        CHECK(out.empty());
    }

    // Clear
    idx.Clear();
    CHECK(idx.Size() == 0);
    {
        std::vector<entt::entity> out;
        idx.QueryBox(-1000.0f, -1000.0f, 1000.0f, 1000.0f, out);
        CHECK(out.empty());
    }

    // 多数挿入してもセル分割で取りこぼさない（格子状 100 体、中央 3x3 セル相当を矩形抽出）
    {
        SpatialIndex grid(2.0f);
        std::uint32_t id = 100;
        for (int x = -10; x <= 9; ++x)
            for (int z = -10; z <= 9; ++z)
                grid.Insert(E(id++), static_cast<float>(x), static_cast<float>(z));
        CHECK(grid.Size() == 400);
        std::vector<entt::entity> out;
        grid.QueryBox(-0.5f, -0.5f, 2.5f, 2.5f, out);   // x,z ∈ {0,1,2} → 3x3 = 9
        CHECK(out.size() == 9);
    }

    std::printf("spatial_index: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
