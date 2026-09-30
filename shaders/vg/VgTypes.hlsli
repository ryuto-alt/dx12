// VgTypes.hlsli ― 仮想ジオメトリの GPU 構造体（cbuffer もリソース宣言も置かない）。
//
// ★C++ の src/renderer/vg/VgGpuTypes.h と 1 バイトずつ一致させる（C++ 側は static_assert でサイズ固定）。
// ★ここに cbuffer を置かない理由: P4 の resolve（VgResolve.hlsl）はメイン RS で動き、b0 / b1 の意味が VG の RS と違う。
//   VgCommon.hlsli（VG の RS 用の cbuffer + カリング部品）と VgResolve.hlsl の両方がこのファイルを include する。
#ifndef VG_TYPES_HLSLI
#define VG_TYPES_HLSLI

#include "../../src/renderer/vg/VgShared.h"

#define VG_CHUNK_PAGES_LOG2 12u      // VgGpuTypes.h の kVgChunkPagesLog2 と一致させること
#define VG_PAGE_BYTES       131072u

struct VgInstance
{
    float4 w0; float4 w1; float4 w2;         // 行 i = 4x4 行列の列 i
    float4 p0; float4 p1; float4 p2;         // prevWorld
    uint assetIndex; uint flags; uint packedTint; uint packedEmissive;
    float maxScale; uint entityId; float overrideMetallic; float overrideRoughness;
};

struct VgAssetGpu
{
    float4 boundingSphere;
    float3 aabbMin;   uint nodesSrv;
    float3 aabbMax;   uint poolSrvBase;
    float3 posOrigin; float posStep;
    uint rootNode; uint pageCount; uint sourceTriangles; uint levelCount;
    uint clusterCount; uint materialCount; uint materialBase; uint reserved;
};

// P4: 材質（VgMaterialGpu。64 B）
struct VgMaterialGpu
{
    uint   albedoSrv;
    uint   normalSrv;
    uint   metalRoughSrv;
    uint   emissiveSrv;
    uint   flags;              // VG_MAT_*
    float  metallic;
    float  roughness;
    uint   reserved0;
    float4 uvScaleOffset;      // xy = スケール / zw = オフセット
    float3 emissiveColor;
    float  emissiveIntensity;
};

struct HierChild
{
    float4 cullSphere;
    float4 lodSphere;
    float  minOwnError;
    float  maxParentError;
    uint   ref;
    uint   groupPacked;
};
struct HierNode { HierChild c[4]; };

// インスタンスの 3x4 行列（行 i = 4x4 行列の列 i）で点を変換する。
float3 VgXform(float4 r0, float4 r1, float4 r2, float3 p)
{
    const float4 q = float4(p, 1.0);
    return float3(dot(q, r0), dot(q, r1), dot(q, r2));
}

#endif // VG_TYPES_HLSLI
