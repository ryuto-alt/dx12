#pragma once
//
// VgeoBvh.h ― `.vgeo` の NODES セクション（4 分木 BVH）を作る汎用ビルダ。ヘッダオンリー・標準ライブラリのみ。
//
//   入力  : 葉（= グループ）の並び。「空間的にまとまった順」で渡すこと（Morton 順など）。
//   出力  : HierNode の配列。ノード 0 が根、子ノードの番号は必ず親より大きい（幅優先）。
//   方式  : 隣り合う 4 個ずつ束ねて下から作る（決定的）。集約値は仕様 §7 のとおり
//            cullSphere = 部分木のジオメトリを包む球 / lodSphere = メンバーの parentLodSphere を包む球 /
//            minOwnError = メンバー lodError の最小 / maxParentError = メンバー parentLodError の最大。
//
//   ★実 cooker（P1）へ: LOD 枝刈り（minOwnError / maxParentError）を効かせるには「レベル帯」ごとに葉を並べてから
//     この関数へ渡す（帯の境界を 4 の倍数に揃えると、下位のノードが帯を跨がない）。docs/VGEO_SPEC.md §7.4 参照。
//
#include "renderer/vg/VgeoFormat.h"

namespace dx12e::vg
{

struct BvhLeaf
{
    f32 cullSphere[4]  = {0, 0, 0, 0};
    f32 lodSphere[4]   = {0, 0, 0, 0};
    f32 minOwnError    = 0.0f;
    f32 maxParentError = std::numeric_limits<f32>::infinity();
    u32 groupId        = 0;           // ★(page, firstCluster) 昇順の順位であること（仕様 §7.2）
    u32 groupPacked    = 0;           // MakeGroupPacked(page, firstCluster, clusterCount)
};

inline std::vector<HierNode> BuildBvh4(const std::vector<BvhLeaf>& leaves)
{
    std::vector<HierNode> out;
    if (leaves.empty()) return out;

    // 作業用: 1 段ぶんの「子になる要素」
    std::vector<HierChild> items(leaves.size());
    for (size_t i = 0; i < leaves.size(); ++i)
    {
        HierChild c{};
        std::memcpy(c.cullSphere, leaves[i].cullSphere, 16);
        std::memcpy(c.lodSphere, leaves[i].lodSphere, 16);
        c.minOwnError = leaves[i].minOwnError; c.maxParentError = leaves[i].maxParentError;
        c.ref = MakeLeafRef(leaves[i].groupId); c.groupPacked = leaves[i].groupPacked;
        items[i] = c;
    }
    auto emptyChild = []() { HierChild c{}; c.ref = kNone; return c; };

    // 下から: 4 個ずつ束ねて（作成順の）ノードにする。最後に 1 ノードになるまで繰り返す。
    std::vector<HierNode> tmp;               // 作成順（根が最後）
    for (;;)
    {
        std::vector<HierChild> next;
        for (size_t i = 0; i < items.size(); i += 4)
        {
            HierNode n{};
            f32 cs[16], ls[16];
            u32 k = 0;
            f32 minOwn = 0, maxPar = 0;
            for (u32 j = 0; j < 4; ++j)
            {
                if (i + j < items.size())
                {
                    n.child[j] = items[i + j];
                    std::memcpy(cs + k * 4, items[i + j].cullSphere, 16);
                    std::memcpy(ls + k * 4, items[i + j].lodSphere, 16);
                    minOwn = (k == 0) ? items[i + j].minOwnError : std::min(minOwn, items[i + j].minOwnError);
                    maxPar = (k == 0) ? items[i + j].maxParentError : std::max(maxPar, items[i + j].maxParentError);
                    ++k;
                }
                else n.child[j] = emptyChild();
            }
            HierChild parent{};
            EnclosingSphere(cs, k, parent.cullSphere);
            EnclosingSphere(ls, k, parent.lodSphere);
            parent.minOwnError = minOwn; parent.maxParentError = maxPar;
            parent.ref = static_cast<u32>(tmp.size());   // 作成順の番号（あとで幅優先番号へ張り替える）
            parent.groupPacked = 0;
            tmp.push_back(n);
            next.push_back(parent);
        }
        if (next.size() == 1) break;
        items.swap(next);
    }

    // 根（tmp.back()）から幅優先で番号を振り直す（子 > 親 を保証）
    std::vector<u32> newId(tmp.size(), kNone);
    std::vector<u32> queue;
    queue.push_back(static_cast<u32>(tmp.size() - 1));
    newId[tmp.size() - 1] = 0;
    for (size_t qi = 0; qi < queue.size(); ++qi)
        for (const HierChild& c : tmp[queue[qi]].child)
            if (c.ref != kNone && !NodeRefIsLeaf(c.ref))
            {
                newId[c.ref] = static_cast<u32>(queue.size());
                queue.push_back(c.ref);
            }
    out.resize(tmp.size());
    for (size_t qi = 0; qi < queue.size(); ++qi)
    {
        HierNode n = tmp[queue[qi]];
        for (HierChild& c : n.child)
            if (c.ref != kNone && !NodeRefIsLeaf(c.ref)) c.ref = newId[c.ref];
        out[qi] = n;
    }
    return out;
}

} // namespace dx12e::vg
