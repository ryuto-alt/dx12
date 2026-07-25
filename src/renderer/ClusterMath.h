#pragma once

// ClusterMath.h — クラスタードライティング（Forward+）の純粋な数式とグリッド定数。
//
// ★ここと shaders/forward/ClusterCommon.hlsli は「手で」同期させるしかない
//   （HLSL と C++ を static_assert で突き合わせる手段が無い）。
//   片方を変えたら必ず両方直すこと。tests/cluster_math_test.cpp が
//   Z 分割の逆写像とカリング判定の不変条件を検証している。
//
// 参考:
//   指数分割       : http://www.aortiz.me/2018/12/21/CG.html (Tiago Sousa / DOOM 2016 由来)
//   球 vs AABB     : https://turanszkij.wordpress.com/2018/01/10/optimizing-tile-based-light-culling
//   円錐 vs 球     : https://bartwronski.com/2017/04/13/cull-that-cone/

#include "core/Types.h"

#include <algorithm>
#include <cmath>

namespace dx12e
{
namespace cluster
{

// ---------------------------------------------------------------------------
// グリッド定数（HLSL 側 CLUSTER_GRID_* と一致させること）
// ---------------------------------------------------------------------------
// 16x9x24 = 3456。DOOM 2016 が 16x8x24=3072、aortiz の primer が 16x9x24。
// ビューポート（エディタの ImGui パネル）は毎フレーム寸法が変わるので、
// 「クラスタ数を固定してタイルの画素サイズを可変にする」方針にしてある。
// これならバッファ/SRV/ディスクリプタの再確保が要らない。
inline constexpr u32 kGridX = 16;
inline constexpr u32 kGridY = 9;
inline constexpr u32 kGridZ = 24;
inline constexpr u32 kClusterCount = kGridX * kGridY * kGridZ;   // 3456

// 1 クラスタで評価できる灯数（固定ストライド。アトミック不要・グローバル溢れ無し）。
inline constexpr u32 kMaxLightsPerCluster = 128;
// シーン全体で GPU へ送れる灯数（point + spot の合計）。
inline constexpr u32 kMaxSceneLights = 1024;
// クラスタード無効時（正射カメラ / カメラプレビュー / render_clustered=0）の総当たり上限。
// 旧 8 灯経路より緩いのでフォールバックしても機能後退にならない。
inline constexpr u32 kFallbackMaxLights = 64;

// クラスタ分割に使う far のクランプ。カメラ far は既定 1000 だが、そこまで対数分割すると
// 近距離のスライスが無駄に細かくなる（CSM が camFar を 200 に切っているのと同じ発想）。
// 最終スライスの AABB だけは実 camFar まで伸ばすので、遠景のライトも消えない。
inline constexpr float kClusterFarLimit = 500.0f;

// ---------------------------------------------------------------------------
// Z の指数分割（Tiago Sousa / DOOM 2016）
//   slice(z)  = floor(log2(z) * sliceScale + sliceBias)
//   sliceScale =  gridZ / log2(zFar / zNear)
//   sliceBias  = -gridZ * log2(zNear) / log2(zFar / zNear)
//   逆写像     : zSlice(k) = zNear * pow(zFar / zNear, k / gridZ)
// ---------------------------------------------------------------------------
inline float SliceScale(float zNear, float zFar)
{
    return static_cast<float>(kGridZ) / std::log2(zFar / zNear);
}

inline float SliceBias(float zNear, float zFar)
{
    return -static_cast<float>(kGridZ) * std::log2(zNear) / std::log2(zFar / zNear);
}

// view 空間深度 → スライス番号（0..kGridZ-1 にクランプ）。
// viewZ <= 0（カメラ背面）は log2 が NaN/-inf になるので zNear で下限を切る。
inline u32 SliceFromViewZ(float viewZ, float zNear, float zFar)
{
    const float z  = (std::max)(viewZ, zNear);
    const float fs = std::log2(z) * SliceScale(zNear, zFar) + SliceBias(zNear, zFar);
    const float c  = (std::min)((std::max)(fs, 0.0f), static_cast<float>(kGridZ) - 1.0f);
    return static_cast<u32>(c);
}

// スライス k の手前側 view 空間 Z（k==kGridZ のとき zFar そのもの）。
inline float SliceNearZ(u32 k, float zNear, float zFar)
{
    return zNear * std::pow(zFar / zNear, static_cast<float>(k) / static_cast<float>(kGridZ));
}

// クラスタ線形 index（HLSL の並びと一致: (slice * gridY + y) * gridX + x）。
inline u32 LinearIndex(u32 x, u32 y, u32 slice)
{
    return (slice * kGridY + y) * kGridX + x;
}

// ---------------------------------------------------------------------------
// カリング判定
// ---------------------------------------------------------------------------

// 球 vs AABB（AABB へ最近接点を落として距離二乗で比較）。
inline bool SphereVsAabb(const float c[3], float r,
                         const float bmin[3], const float bmax[3])
{
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float d = (std::max)(0.0f, (std::max)(bmin[i] - c[i], c[i] - bmax[i]));
        d2 += d * d;
    }
    return d2 <= r * r;
}

// 円錐 vs 球（Bart Wronski "Cull that cone!"）。
//   origin  : 円錐の頂点
//   fwd     : 円錐の軸（正規化済み）
//   size    : 円錐の高さ（＝ライトの range）
//   cosHalf / sinHalf : 外側半角の cos / sin
//   sc / sr : 球の中心 / 半径
inline bool ConeVsSphere(const float origin[3], const float fwd[3], float size,
                         float cosHalf, float sinHalf,
                         const float sc[3], float sr)
{
    const float v[3]   = {sc[0] - origin[0], sc[1] - origin[1], sc[2] - origin[2]};
    const float vLenSq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    const float v1Len  = v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2];
    const float dClosest =
        cosHalf * std::sqrt((std::max)(vLenSq - v1Len * v1Len, 0.0f)) - v1Len * sinHalf;

    const bool angleCull = dClosest > sr;
    const bool frontCull = v1Len > sr + size;
    const bool backCull  = v1Len < -sr;
    return !(angleCull || frontCull || backCull);
}

} // namespace cluster
} // namespace dx12e
