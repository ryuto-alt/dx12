#pragma once
// ---------------------------------------------------------------------------
// BoneMask — レイヤーごとの「どのボーンに効かせるか」の重み配列。
//
// 「下半身は走り、上半身は構え」のようにレイヤーを部位で分けるための道具。
// 重みは 0..1 の float なので、肩などの境界を滑らかにもできる。
//
// マスク外のボーンは下位レイヤーの値が**そのまま**残る
// （AnimPose の OverrideBlendPose / AdditiveBlendPose が w<=0 のボーンを触らない）。
//
// 依存は Skeleton だけ（GPU/ECS/Jolt 非依存）。tests/ から直接ビルドして検証できる。
// ---------------------------------------------------------------------------
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "core/Types.h"
#include "animation/Skeleton.h"

namespace dx12e
{

// 全ボーン 0 のマスクを作る。
inline void MakeEmptyBoneMask(const Skeleton& skeleton, std::vector<f32>& out)
{
    out.assign(skeleton.GetBoneCount(), 0.0f);
}

// rootBoneIndex とその子孫すべてに weight を書き込む（既存の値は max で合成）。
// ⚠️ 親が子より前に並んでいる前提の 1 パス（Skeleton::AreBonesCorrectlyOrdered）。
inline void AddSubtreeToBoneMask(const Skeleton& skeleton, i32 rootBoneIndex, f32 weight,
                                 std::vector<f32>& out)
{
    const u32 boneCount = skeleton.GetBoneCount();
    if (out.size() < boneCount) out.resize(boneCount, 0.0f);
    if (rootBoneIndex < 0 || static_cast<u32>(rootBoneIndex) >= boneCount) return;

    // どのボーンがサブツリーに属するかを 1 パスで求める
    std::vector<bool> inSubtree(boneCount, false);
    inSubtree[static_cast<size_t>(rootBoneIndex)] = true;
    for (u32 i = static_cast<u32>(rootBoneIndex) + 1; i < boneCount; ++i)
    {
        const i32 p = skeleton.GetBone(i).parentIndex;
        if (p >= 0 && inSubtree[static_cast<size_t>(p)]) inSubtree[i] = true;
    }

    for (u32 i = 0; i < boneCount; ++i)
        if (inSubtree[i]) out[i] = (std::max)(out[i], weight);
}

// 指定ボーン 1 本だけに weight を書き込む（子孫は含めない）。
inline void AddBoneToBoneMask(const Skeleton& skeleton, i32 boneIndex, f32 weight,
                              std::vector<f32>& out)
{
    const u32 boneCount = skeleton.GetBoneCount();
    if (out.size() < boneCount) out.resize(boneCount, 0.0f);
    if (boneIndex < 0 || static_cast<u32>(boneIndex) >= boneCount) return;
    out[static_cast<size_t>(boneIndex)] = (std::max)(out[static_cast<size_t>(boneIndex)], weight);
}

// ボーン名のリストからマスクを作る。
//   names           対象のボーン名。完全一致で引き、無ければ部分一致（大文字小文字無視）で探す
//   includeChildren true ならそのボーンの子孫も含める（「Spine1 以下を全部」）
//   weight          書き込む重み
// 解決できなかった名前は missing へ積む（呼び出し側が警告を出せるように）。
inline void BuildBoneMaskFromNames(const Skeleton& skeleton,
                                   const std::vector<std::string>& names,
                                   bool includeChildren, f32 weight,
                                   std::vector<f32>& out,
                                   std::vector<std::string>* missing = nullptr)
{
    MakeEmptyBoneMask(skeleton, out);
    for (const std::string& n : names)
    {
        i32 idx = skeleton.FindBoneIndex(n);
        if (idx < 0) idx = skeleton.FindBoneIndexContaining(n);
        if (idx < 0)
        {
            if (missing) missing->push_back(n);
            continue;
        }
        if (includeChildren) AddSubtreeToBoneMask(skeleton, idx, weight, out);
        else                 AddBoneToBoneMask(skeleton, idx, weight, out);
    }
}

} // namespace dx12e
