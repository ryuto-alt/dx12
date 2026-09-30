#pragma once
// ===========================================================================
// 植生 F1: 散布（ポアソンディスク + 傾斜 / 高度 / スプラット層による制限）とブラシ。
// ---------------------------------------------------------------------------
// GPU にも entt にも依存しない純ロジック。表面（地形 / メッシュ）の情報は SurfaceFn（(x,z) → 高さ・法線・スプラット重み）
// として呼び出し側が渡す。★決定論: 同じ seed / パラメータ / 表面なら毎回まったく同じ結果（OS・プラットフォーム非依存の自前 RNG）。
// ===========================================================================
#include <functional>
#include <vector>

#include "core/Types.h"
#include "renderer/foliage/FoliageTypes.h"

namespace dx12e::foliage
{

struct ScatterParams
{
    u32 seed = 1;
    f32 density = 0.5f;          // 平均個数 / m²（minSpacing があるときは「その最大充填のうち何割か」として間引く）
    f32 minSpacing = 0.0f;       // 最小間隔 m（ポアソンディスク）。0 = 一様ランダム
    f32 scaleMin = 0.8f, scaleMax = 1.2f;
    f32 yawRandom = 1.0f;        // 0..1（1 = 全周ランダム）
    f32 tiltAlign = 0.0f;        // 0..1: 表面の法線へ傾ける割合
    f32 tiltRandomDeg = 0.0f;    // 追加のランダムな傾き（度）
    f32 minSlopeDeg = 0.0f, maxSlopeDeg = 90.0f;   // 表面の傾斜（度）の許容範囲
    f32 minHeight = -1e30f, maxHeight = 1e30f;     // ワールドではなく「SurfaceFn が返す y」の範囲
    i32 splatLayer = -1;         // 0..3 = そのスプラット層の重みで確率的に採否（-1 = 使わない）
    f32 splatThreshold = 0.3f;   // これ未満の重みは置かない
    f32 splatPower = 1.0f;       // 採用確率 = weight^power
    f32 colorVariation = 0.15f;  // 明るさのばらつき（0..1）
    f32 windVariation = 0.2f;    // 風の強さのばらつき（±この割合）
    u32 variantCount = 1;        // 種別数（1..4）
    f32 variantWeight[kMaxVariants] = {1, 1, 1, 1};
    u32 maxCount = 2000000;      // 1 回の散布で作る上限（暴走防止）
};

struct SurfaceSample
{
    f32 y = 0.0f;
    f32 nx = 0.0f, ny = 1.0f, nz = 0.0f;
    f32 splat[4] = {0, 0, 0, 0};   // 0..1（無ければ 0）
};
// 表面の問い合わせ。false = この (x,z) に表面が無い（置かない）。
using SurfaceFn = std::function<bool(f32 x, f32 z, SurfaceSample& out)>;

struct ScatterRegion
{
    bool circle = false;
    f32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;    // 矩形（レイヤーのローカル XZ）
    f32 cx = 0, cz = 0, radius = 0;        // 円
};

struct ScatterStats
{
    u32 candidates = 0, accepted = 0;
    u32 rejectedSurface = 0, rejectedSlope = 0, rejectedHeight = 0, rejectedSplat = 0, rejectedDensity = 0, rejectedSpacing = 0;
    f32 area = 0.0f;
};

// 散布。out へ追記する。existing != null なら、その位置との最小間隔も守る（ブラシで足すとき）。
ScatterStats ScatterPoisson(const ScatterParams& p, const ScatterRegion& region, const SurfaceFn& surface,
                            std::vector<FoliageInstance>& out, const std::vector<FoliageInstance>* existing = nullptr);

// 円ブラシで消す（半径内の個数を density（0..1）の割合で削除）。削除数を返す。
u32 EraseInCircle(std::vector<FoliageInstance>& inst, f32 cx, f32 cz, f32 radius, f32 fraction, u32 seed);

// 円ブラシの中の最小間隔（テスト用）。out の全ペアの最短距離（O(N) の格子で計算）。
f32 MinPairDistanceXZ(const std::vector<FoliageInstance>& inst, f32 searchRadius);

} // namespace dx12e::foliage
