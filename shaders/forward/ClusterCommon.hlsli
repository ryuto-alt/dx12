// ClusterCommon.hlsli - クラスタードライティング（Forward+）の共有定義。
//
// CS 側（ClusterBuild.hlsl / ClusterCull.hlsl）と PS 側（Lighting.hlsli）の両方から include する。
// ★C++ 側の実体は src/renderer/ClusterMath.h。static_assert で突き合わせる手段が無いので
//   片方を変えたら必ず両方直すこと（グリッド寸法・1クラスタ最大灯数・シーン最大灯数）。
//
// CS 専用の cbuffer(b0) は PS 側の PerObjectConstants(b0) と衝突するため、
// CLUSTER_CS を define したときだけ宣言する。

#ifndef CLUSTER_COMMON_HLSLI
#define CLUSTER_COMMON_HLSLI

// ---- グリッド寸法（src/renderer/ClusterMath.h の kGrid* と一致させること）----
#define CLUSTER_GRID_X 16
#define CLUSTER_GRID_Y 9
#define CLUSTER_GRID_Z 24
#define CLUSTER_COUNT  (CLUSTER_GRID_X * CLUSTER_GRID_Y * CLUSTER_GRID_Z)   // 3456
#define CLUSTER_MAX_LIGHTS_PER_CLUSTER 128
#define CLUSTER_MAX_SCENE_LIGHTS       1024

#define CLUSTER_TYPE_POINT 0.0
#define CLUSTER_TYPE_SPOT  1.0

// 64B。C++ 側は ClusteredLightCulling::LightGPU（static_assert(sizeof == 64) つき）。
// color は intensity 乗算済み。shadowIndex は -1=影なし、それ以外は既存の影スロット index。
struct ClusterLight
{
    float3 position;   float range;
    float3 color;      float type;         // 0=point / 1=spot
    float3 direction;  float cosOuter;     // スポット軸(正規化済) / cos(外側半角)
    float  cosInner;   float sinOuter;     // cos(内側半角) / sin(外側半角)
    float  shadowIndex; float _pad;
};

// 32B。CS 専用（PS からは読まない）。
struct ClusterAABB
{
    float4 minPt;
    float4 maxPt;
};

// クラスタ線形 index（C++ 側 cluster::LinearIndex と同じ並び）。
uint ClusterLinearIndex(uint x, uint y, uint slice, uint gridX, uint gridY)
{
    return (slice * gridY + y) * gridX + x;
}

// 指数分割（Tiago Sousa / DOOM 2016）。scale/bias は CPU 側で一度だけ計算して渡す。
//   sliceScale =  gridZ / log2(zFar / zNear)
//   sliceBias  = -gridZ * log2(zNear) / log2(zFar / zNear)
// viewZ <= 0（カメラ背面）で log2 が NaN になるので zNear で下限を切る。
uint ClusterSliceFromViewZ(float viewZ, float zNear, float sliceScale, float sliceBias, float gridZ)
{
    float fs = log2(max(viewZ, zNear)) * sliceScale + sliceBias;
    return (uint)clamp(fs, 0.0, gridZ - 1.0);
}

// 球 vs AABB。https://turanszkij.wordpress.com/2018/01/10/optimizing-tile-based-light-culling
bool ClusterSphereVsAabb(float3 c, float r, float3 bmin, float3 bmax)
{
    float3 d = max(0.0.xxx, max(bmin - c, c - bmax));
    return dot(d, d) <= r * r;
}

// 円錐 vs 球。https://bartwronski.com/2017/04/13/cull-that-cone/
// size = 円錐の高さ（= ライトの range）、cosHalf/sinHalf = 外側半角の cos/sin。
bool ClusterConeVsSphere(float3 origin, float3 fwd, float size,
                         float cosHalf, float sinHalf, float3 sc, float sr)
{
    float3 v      = sc - origin;
    float  vLenSq = dot(v, v);
    float  v1Len  = dot(v, fwd);
    float  dClosest = cosHalf * sqrt(max(vLenSq - v1Len * v1Len, 0.0)) - v1Len * sinHalf;
    return !(dClosest > sr || v1Len > sr + size || v1Len < -sr);
}

#ifdef CLUSTER_CS
// ---- compute 専用のルート定数 (b0, 28 DWORD) ----
// ClusterBuild.hlsl と ClusterCull.hlsl で同じ器を共有する（中身の使う所だけが違う）。
// C++ 側は ClusteredLightCulling.cpp の ClusterCB（static_assert(28 DWORD) つき）。
cbuffer ClusterCB : register(b0)
{
    float4x4 gViewMat;     //  0..15  view 行列（転置済み。cull 専用。build では未使用）
    uint4    gGridDims;    // 16..19  .xyz = gridX,gridY,gridZ / .w = clusterCount
    float4   gZParams;     // 20..23  .x=zNear .y=zFarCluster .z=zFarCamera .w=未使用
    float4   gMisc;        // 24..27  .x=1/proj_11 .y=1/proj_22 .z=灯数 .w=1クラスタ最大灯数
};
#endif // CLUSTER_CS

#endif // CLUSTER_COMMON_HLSLI
