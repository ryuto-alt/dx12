#pragma once

// シーンの描画メッシュから「ナビメッシュの入力になる三角形スープ」を集める。
//
// ★ AABB ではなく **実際の三角形** を渡すのが肝。ノード変換 × ワールド行列まで掛けて
//   ワールド空間へ落とすので、坂道・階段・斜めの壁がそのままの形でボクセル化される。
//
// 除外するもの:
//   - GridPlane（エディタの目安グリッド）
//   - Sprite2D / パーティクル等（メッシュを持たないので自然に外れる）
//   - スキンメッシュ（CPU 側はバインドポーズなので、動く敵の形を焼いても意味が無い）
//   - navMeshIgnore タグの付いたエンティティ（利用者が個別に外せる逃げ道）

#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "core/Types.h"
#include "nav/NavTypes.h"

namespace dx12e
{

struct NavGatherStats
{
    i32 entityCount = 0;
    i32 meshCount   = 0;
    i32 triCount    = 0;
    i32 skippedSkinned = 0;
    i32 skippedTagged  = 0;
};

// 除外用タグ。エンティティに Tag として付けると、そのメッシュはナビメッシュに含まれない。
inline constexpr const char* kNavIgnoreTag = "navMeshIgnore";

// 戻り値 false = 集められる三角形が 1 枚も無かった。
bool GatherNavGeometry(entt::registry& reg, nav::NavInputGeometry& out, NavGatherStats& stats);

} // namespace dx12e
