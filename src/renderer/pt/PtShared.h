#pragma once
// ===========================================================================
// パストレーサー(地上真値レンダラ)の C++ / HLSL 共有レイアウト。
// ---------------------------------------------------------------------------
// ★shaders/pt/PtCommon.hlsli の構造体・定数バッファと【バイト単位で一致】させること(static_assert で番人)。
//   HLSL 側を変えたらここも、ここを変えたら HLSL も直す。
// ===========================================================================
#include <cstdint>

namespace dx12e::pt
{

constexpr uint32_t kNoIndex = 0xFFFFFFFFu;

// ---- ジョブ定数のフラグ(PtCommon.hlsli の PT_FLAG_*)----
constexpr uint32_t kFlagRR            = 1u;
constexpr uint32_t kFlagPhysFalloff   = 2u;
constexpr uint32_t kFlagEnvNee        = 4u;
constexpr uint32_t kFlagCullBackface  = 8u;
constexpr uint32_t kFlagEmissiveBoth  = 16u;
constexpr uint32_t kFlagForceLambert  = 32u;
constexpr uint32_t kFlagNoNormalMap   = 64u;
constexpr uint32_t kFlagAtmosphere    = 128u;   // 環境 = 物理大気(A1)。atmosSrvBase の SRV ブロックを引く

// ---- 材質フラグ(PT_MAT_*)----
constexpr uint32_t kMatNormalMap   = 1u;
constexpr uint32_t kMatMrTex       = 2u;
constexpr uint32_t kMatEmissiveTex = 8u;
constexpr uint32_t kMatLambert     = 256u;
constexpr uint32_t kMatEmitBoth    = 512u;

// ---- インスタンスフラグ(PT_INST_*)----
constexpr uint32_t kInstSkinned = 1u;

// 128B(PtInstance)
struct InstanceGpu
{
    float o2w[12];   // 3 行 × (x,y,z,w=平行移動)
    float w2o[12];
    uint32_t vbSrv, ibSrv, materialIndex, flags;
    uint32_t emissiveBase;
    uint32_t defVbSrv;
    uint32_t pad0, pad1;
};
static_assert(sizeof(InstanceGpu) == 128, "PtInstance と一致させること");

// 80B(PtMaterial)
struct MaterialGpu
{
    float tint[3];     float opacity;
    float metallic;    float roughness;  float alphaCutoff;  uint32_t flags;
    float emissive[3]; uint32_t albedoSrv;
    uint32_t normalSrv, mrSrv, emissiveSrv, alphaMode;   // 0=Opaque 1=Mask 2=Blend
    float uvScaleOffset[4];
};
static_assert(sizeof(MaterialGpu) == 80, "PtMaterial と一致させること");

// 64B(PtLight)。ClusterLight と同じ並び(shadowIndex の枠は未使用)。
struct LightGpu
{
    float position[3];  float range;
    float color[3];     float type;        // 0=point / 1=spot
    float direction[3]; float cosOuter;
    float cosInner;     float sinOuter;    float pad0, pad1;
};
static_assert(sizeof(LightGpu) == 64, "PtLight と一致させること");

// 32B(PtEmissiveTri)。Walker alias 表の 1 要素。
struct EmissiveTriGpu
{
    uint32_t instance, prim;
    float pmf, thresh;
    uint32_t alias;
    float area;
    uint32_t pad0, pad1;
};
static_assert(sizeof(EmissiveTriGpu) == 32, "PtEmissiveTri と一致させること");

// ジョブ定数(b0)。HLSL の cbuffer JobCB と行ごとに一致(各行 16B)。
struct JobConstants
{
    uint32_t width, height, bounces, seed;
    uint32_t flags; float maxRadiance; uint32_t envCubeSrv; uint32_t lightCount;
    float camPos[3];     float tanHalfFovY;
    float camRight[3];   float aspect;
    float camUp[3];      uint32_t emissiveCount;
    float camFwd[3];     uint32_t instanceCount;
    float sunToLight[3]; float sunTanRadius;
    float sunE[3];       float sunEnabled;
    float envConst[3];   float envLightScale;
    float envBgScale;    uint32_t rrStart; float lensRadius; float focusDist;
    float pixelFilter;   uint32_t countRays; uint32_t atmosSrvBase, pad1;   // atmosSrvBase = 大気 SRV ブロック先頭(kFlagAtmosphere のとき)
};
static_assert(sizeof(JobConstants) == 16 * 11, "cbuffer JobCB と一致させること");

// タイルごとのルート定数(b1、8 DWORD)
struct TileConstants
{
    uint32_t tileX, tileY, sampleBase, sampleCount;
    uint32_t tileW, tileH, pad0, pad1;
};
static_assert(sizeof(TileConstants) == 32, "cbuffer TileCB と一致させること");

} // namespace dx12e::pt
