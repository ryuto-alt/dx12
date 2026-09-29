#pragma once
//
// CookGroup.h ― クラスタのグループ化（DAG の 1 レベル分）。METIS 依存なし。4 方式を持ち、品質を比較して既定を決めた。
//
//   Morton  : クラスタ中心の Morton 順に target 個ずつ区切る（空間ソートのみ。隣接を見ない。基準線）。
//   Greedy  : Morton 順の種から、共有境界エッジ数が最大の隣接クラスタを貪欲に足す（エッジ隣接の貪欲グラフ成長）。
//   Bisect  : 中心の最長軸メディアンで再帰二分割（グループ数を k = round(n / target) に配る。隣接を見ない）。
//   Meshopt : meshopt_partitionClusters（頂点共有 + 近接。設計書 §2.3.1 2a の想定実装）。
//
//   品質の指標 = 「異なるグループにまたがる共有境界エッジの総数」（CutWeight。小さいほど簡略化でロックされる頂点が減る）。
//   全方式とも決定的（同じ入力 → 同じ分割。乱数・スレッド・ポインタ値に依らない）。
//
#include "CookUtil.h"

#include <meshoptimizer.h>

#include <cmath>
#include <cstring>
#include <string>

namespace dx12e::vg::cook
{

enum class GroupMethod : u32 { Morton = 0, Greedy = 1, Bisect = 2, Meshopt = 3 };

inline const char* GroupMethodName(GroupMethod m)
{
    switch (m)
    {
    case GroupMethod::Morton: return "morton";
    case GroupMethod::Greedy: return "greedy";
    case GroupMethod::Bisect: return "bisect";
    case GroupMethod::Meshopt: return "meshopt";
    }
    return "?";
}
inline bool ParseGroupMethod(const std::string& s, GroupMethod& out)
{
    if (s == "morton") out = GroupMethod::Morton;
    else if (s == "greedy") out = GroupMethod::Greedy;
    else if (s == "bisect") out = GroupMethod::Bisect;
    else if (s == "meshopt") out = GroupMethod::Meshopt;
    else return false;
    return true;
}

// 1 レベルのクラスタ隣接グラフ（無向・重み = 共有する境界エッジの本数）。CSR で両方向を持つ。
struct ClusterGraph
{
    u32 n = 0;
    std::vector<u32> off;   // n + 1
    std::vector<u32> nbr;   // 隣接クラスタ
    std::vector<u32> w;     // 共有エッジ数
};

struct GroupingInput
{
    u32 n = 0;
    const f32* centers = nullptr;        // 3 * n（クラスタ中心）
    const ClusterGraph* graph = nullptr; // Greedy / 後処理で使う（null なら隣接なし扱い）
    // Meshopt 方式の入力（各クラスタが参照する「溶接済み頂点 ID」の連結と個数、位置は溶接 ID で引ける全体配列）
    const u32* mIndices = nullptr;
    const u32* mCounts = nullptr;
    size_t mTotal = 0;
    const f32* mPositions = nullptr;
    size_t mVertexCount = 0;
};

// 異なるグループにまたがる共有境界エッジの総数（無向で 1 回ずつ）。
inline u64 CutWeight(const ClusterGraph& g, const std::vector<u32>& groupOf)
{
    u64 cut = 0;
    for (u32 a = 0; a < g.n; ++a)
        for (u32 e = g.off[a]; e < g.off[a + 1]; ++e)
            if (g.nbr[e] > a && groupOf[a] != groupOf[g.nbr[e]]) cut += g.w[e];
    return cut;
}

namespace gdetail
{
inline f32 Dist2(const f32* c, u32 a, u32 b)
{
    const f32 dx = c[a * 3] - c[b * 3], dy = c[a * 3 + 1] - c[b * 3 + 1], dz = c[a * 3 + 2] - c[b * 3 + 2];
    return dx * dx + dy * dy + dz * dz;
}

inline std::vector<u32> MortonOrder(const GroupingInput& in)
{
    f64 lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (u32 i = 0; i < in.n; ++i)
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], static_cast<f64>(in.centers[i * 3 + a])); hi[a] = std::max(hi[a], static_cast<f64>(in.centers[i * 3 + a])); }
    f64 inv[3];
    for (int a = 0; a < 3; ++a) inv[a] = (hi[a] > lo[a]) ? 1.0 / (hi[a] - lo[a]) : 0.0;
    std::vector<std::pair<u64, u32>> keys(in.n);
    for (u32 i = 0; i < in.n; ++i) keys[i] = {MortonOfPoint(&in.centers[i * 3], lo, inv), i};
    std::sort(keys.begin(), keys.end());
    std::vector<u32> order(in.n);
    for (u32 i = 0; i < in.n; ++i) order[i] = keys[i].second;
    return order;
}

// group id を「最小メンバー番号の昇順」に振り直す。戻り値 = グループ数。
inline u32 Renumber(std::vector<u32>& groupOf)
{
    std::vector<u32> map;
    u32 G = 0;
    u32 maxId = 0;
    for (u32 g : groupOf) maxId = std::max(maxId, g);
    map.assign(static_cast<size_t>(maxId) + 1, kNone);
    for (u32& g : groupOf)
    {
        if (map[g] == kNone) map[g] = G++;
        g = map[g];
    }
    return G;
}

// 1 クラスタだけのグループを、共有エッジ最多（無ければ Morton 順で近い）の隣のグループへ併合する。
inline u32 MergeSingletons(std::vector<u32>& groupOf, u32 G, const GroupingInput& in, const std::vector<u32>& order, u32 maxSize)
{
    if (in.n < 2 || G < 2) return G;
    std::vector<u32> size(G, 0);
    for (u32 g : groupOf) ++size[g];
    std::vector<u32> rank(in.n);
    for (u32 i = 0; i < in.n; ++i) rank[order[i]] = i;
    bool any = false;
    for (u32 i = 0; i < in.n; ++i)
    {
        const u32 c = i;
        if (size[groupOf[c]] != 1) continue;
        u32 bestG = kNone, bestW = 0;
        if (in.graph)
            for (u32 e = in.graph->off[c]; e < in.graph->off[c + 1]; ++e)
            {
                const u32 g2 = groupOf[in.graph->nbr[e]];
                if (g2 == groupOf[c] || size[g2] >= maxSize) continue;
                if (in.graph->w[e] > bestW || (in.graph->w[e] == bestW && g2 < bestG)) { bestW = in.graph->w[e]; bestG = g2; }
            }
        if (bestG == kNone)
        {
            f32 bestD = 1e30f;
            const i64 r0 = rank[c];
            for (i64 d = -32; d <= 32; ++d)
            {
                const i64 r = r0 + d;
                if (d == 0 || r < 0 || r >= static_cast<i64>(in.n)) continue;
                const u32 o = order[static_cast<size_t>(r)];
                const u32 g2 = groupOf[o];
                if (g2 == groupOf[c] || size[g2] >= maxSize) continue;
                const f32 dd = Dist2(in.centers, c, o);
                if (dd < bestD || (dd == bestD && g2 < bestG)) { bestD = dd; bestG = g2; }
            }
        }
        if (bestG == kNone) continue;
        --size[groupOf[c]];
        groupOf[c] = bestG;
        ++size[bestG];
        any = true;
    }
    return any ? Renumber(groupOf) : G;
}
} // namespace gdetail

// 目標グループサイズ target（≥ 2）でクラスタ 0..n-1 を分割する。groupOf[i] = グループ番号。戻り値 = グループ数。
// 1 クラスタだけのグループは（n ≥ 2 のとき）隣へ併合する。グループの大きさは最大 maxSize（≤ 15）に収める。
inline u32 PartitionClusters(GroupMethod method, u32 target, const GroupingInput& in, std::vector<u32>& groupOf)
{
    const u32 n = in.n;
    groupOf.assign(n, 0);
    if (n == 0) return 0;
    target = std::max(2u, std::min(target, kMaxGroupClusters));
    const u32 maxSize = std::min(kMaxGroupClusters, target + std::max(2u, target / 2));
    if (n <= target) return 1;
    const u32 k = std::max(1u, (n + target / 2) / target);

    const std::vector<u32> order = gdetail::MortonOrder(in);
    u32 G = 0;
    switch (method)
    {
    case GroupMethod::Morton:
    {
        for (u32 i = 0; i < n; ++i) groupOf[order[i]] = static_cast<u32>((static_cast<u64>(i) * k) / n);
        G = k;
        break;
    }
    case GroupMethod::Greedy:
    {
        std::fill(groupOf.begin(), groupOf.end(), kNone);
        std::vector<u32> rank(n);
        for (u32 i = 0; i < n; ++i) rank[order[i]] = i;
        std::vector<std::pair<u32, u32>> cand;   // (クラスタ, 共有エッジ数の合計)
        std::vector<u32> members;
        for (u32 si = 0; si < n; ++si)
        {
            const u32 s = order[si];
            if (groupOf[s] != kNone) continue;
            const u32 gid = G++;
            members.assign(1, s);
            groupOf[s] = gid;
            f32 cen[3] = {in.centers[s * 3], in.centers[s * 3 + 1], in.centers[s * 3 + 2]};
            while (members.size() < target)
            {
                cand.clear();
                if (in.graph)
                    for (u32 m : members)
                        for (u32 e = in.graph->off[m]; e < in.graph->off[m + 1]; ++e)
                        {
                            const u32 nb = in.graph->nbr[e];
                            if (groupOf[nb] != kNone) continue;
                            bool found = false;
                            for (auto& c : cand) if (c.first == nb) { c.second += in.graph->w[e]; found = true; break; }
                            if (!found) cand.emplace_back(nb, in.graph->w[e]);
                        }
                u32 pick = kNone;
                if (!cand.empty())
                {
                    u32 bestScore = 0;
                    f32 bestD = 1e30f;
                    for (const auto& c : cand)
                    {
                        const f32 dx = in.centers[c.first * 3] - cen[0], dy = in.centers[c.first * 3 + 1] - cen[1], dz = in.centers[c.first * 3 + 2] - cen[2];
                        const f32 d = dx * dx + dy * dy + dz * dz;
                        if (c.second > bestScore || (c.second == bestScore && (d < bestD || (d == bestD && c.first < pick))))
                        { bestScore = c.second; bestD = d; pick = c.first; }
                    }
                }
                else
                {
                    // 隣接が尽きた（島や取り残し）: Morton 順で近い未割り当てを窓の中から選ぶ
                    f32 bestD = 1e30f;
                    for (u32 d = 1; d <= 48 && si + d < n; ++d)
                    {
                        const u32 o = order[si + d];
                        if (groupOf[o] != kNone) continue;
                        const f32 dx = in.centers[o * 3] - cen[0], dy = in.centers[o * 3 + 1] - cen[1], dz = in.centers[o * 3 + 2] - cen[2];
                        const f32 dd = dx * dx + dy * dy + dz * dz;
                        if (dd < bestD) { bestD = dd; pick = o; }
                    }
                }
                if (pick == kNone) break;
                groupOf[pick] = gid;
                members.push_back(pick);
                const f32 inv = 1.0f / static_cast<f32>(members.size());
                for (int a = 0; a < 3; ++a) cen[a] += (in.centers[pick * 3 + a] - cen[a]) * inv;
            }
        }
        break;
    }
    case GroupMethod::Bisect:
    {
        std::vector<u32> idx(n);
        for (u32 i = 0; i < n; ++i) idx[i] = i;
        struct Job { u32 b, e, k, base; };
        std::vector<Job> stack;
        stack.push_back({0, n, k, 0});
        while (!stack.empty())
        {
            const Job j = stack.back();
            stack.pop_back();
            if (j.k <= 1 || j.e - j.b <= 1)
            {
                for (u32 i = j.b; i < j.e; ++i) groupOf[idx[i]] = j.base;
                continue;
            }
            f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (u32 i = j.b; i < j.e; ++i)
                for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], in.centers[idx[i] * 3 + a]); hi[a] = std::max(hi[a], in.centers[idx[i] * 3 + a]); }
            int axis = 0;
            for (int a = 1; a < 3; ++a) if (hi[a] - lo[a] > hi[axis] - lo[axis]) axis = a;
            const u32 k1 = j.k / 2, k2 = j.k - k1;
            const u32 cnt = j.e - j.b;
            u32 n1 = static_cast<u32>((static_cast<u64>(cnt) * k1 + j.k / 2) / j.k);
            n1 = std::max(1u, std::min(n1, cnt - 1));
            std::nth_element(idx.begin() + j.b, idx.begin() + j.b + n1, idx.begin() + j.e, [&](u32 x, u32 y)
            {
                const f32 cx = in.centers[x * 3 + axis], cy = in.centers[y * 3 + axis];
                return cx != cy ? cx < cy : x < y;
            });
            stack.push_back({j.b + n1, j.e, k2, j.base + k1});
            stack.push_back({j.b, j.b + n1, k1, j.base});
        }
        G = k;
        break;
    }
    case GroupMethod::Meshopt:
    {
        if (in.mIndices && in.mCounts && in.mPositions)
        {
            const size_t np = meshopt_partitionClusters(groupOf.data(), in.mIndices, in.mTotal, in.mCounts, n, in.mPositions, in.mVertexCount, 12, target);
            G = static_cast<u32>(np);
        }
        else
        {
            for (u32 i = 0; i < n; ++i) groupOf[order[i]] = static_cast<u32>((static_cast<u64>(i) * k) / n);
            G = k;
        }
        break;
    }
    }
    G = gdetail::Renumber(groupOf);
    G = gdetail::MergeSingletons(groupOf, G, in, order, maxSize);
    return G;
}

} // namespace dx12e::vg::cook
