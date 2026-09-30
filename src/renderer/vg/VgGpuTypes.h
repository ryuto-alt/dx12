#pragma once
//
// VgGpuTypes.h ― GPU カリングが読む / 書く構造体（HLSL の shaders/vg/VgCommon.hlsli と 1 バイトずつ一致させる）。
//
//   ★ヘッダオンリー・標準ライブラリのみ。サイズとオフセットは static_assert で固定（HLSL 側は手で写している。
//     変えるときは VgCommon.hlsli を必ず一緒に直す）。
//
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "renderer/vg/VgLodMath.h"
#include "renderer/vg/VgShared.h"

namespace dx12e::vg
{

// ── インスタンス（128 B）: StructuredBuffer<VgInstance>。毎フレーム UPLOAD リングへ詰める ────
struct VgInstance
{
    float    world[12];       //   0  3x4（行 i = 4x4 行列の列 i。TransformPoint34）
    float    prevWorld[12];   //  48  前フレーム（無ければ world と同値）
    uint32_t assetIndex;      //  96  VgAssets の添字
    uint32_t flags;           // 100  VG_INST_*
    uint32_t packedTint;      // 104  RGB888 + opacity8（MeshRenderer::colorTint。白 = 0xFFFFFFFF）
    uint32_t packedEmissive;  // 108  P4: 自己発光の上書き（RGB888 = 色 / 上位 8bit = 強度。どちらが有効かは flags の VG_INST_EMIS_*）
    float    maxScale;        // 112  ワールド行列の最大軸スケール（誤差 / 球半径の上界）
    uint32_t entityId;        // 116  デバッグ / ピッキング
    float    overrideMetallic;  // 120  P4: MeshRenderer::overrideMetallic（< 0 = 材質の値）
    float    overrideRoughness; // 124  P4: 同 overrideRoughness
};
static_assert(sizeof(VgInstance) == 128, "VgInstance size");
static_assert(offsetof(VgInstance, prevWorld) == 48 && offsetof(VgInstance, assetIndex) == 96 &&
              offsetof(VgInstance, maxScale) == 112 && offsetof(VgInstance, overrideMetallic) == 120, "VgInstance layout");

// アプリ側が渡す 1 インスタンスぶんの入力（行列は行優先 4x4 = XMFLOAT4X4 と同じ並び）。
struct VgInstanceInput
{
    float    world[16]{};
    float    prevWorld[16]{};      // hasPrev == false のとき world と同値として扱う
    bool     hasPrev   = false;
    uint32_t assetId   = 0;        // VirtualGeometrySystem が返したアセット ID
    uint32_t entityId  = 0;
    uint32_t packedTint = 0xFFFFFFFFu;
    uint32_t packedEmissive = 0;
    uint32_t emissiveFlags = 0;          // P4: VG_INST_EMIS_COLOR_OV | VG_INST_EMIS_INT_OV
    float    overrideMetallic  = -1.0f;  // P4: < 0 = 材質の値
    float    overrideRoughness = -1.0f;
};

inline VgInstance MakeInstance(const VgInstanceInput& in, uint32_t assetIndexOnGpu)
{
    VgInstance o{};
    PackWorld34(in.world, o.world);
    PackWorld34(in.hasPrev ? in.prevWorld : in.world, o.prevWorld);
    o.assetIndex = assetIndexOnGpu;
    o.flags = (Det3(in.world) < 0.0f ? VG_INST_MIRRORED : 0u) | (in.hasPrev ? VG_INST_HAS_PREV : 0u)
            | (in.emissiveFlags & (VG_INST_EMIS_COLOR_OV | VG_INST_EMIS_INT_OV));
    o.packedTint = in.packedTint;
    o.packedEmissive = in.packedEmissive;
    o.maxScale = MaxAxisScale(in.world);
    o.entityId = in.entityId;
    o.overrideMetallic = in.overrideMetallic;
    o.overrideRoughness = in.overrideRoughness;
    return o;
}

// ── P4: 材質（64 B）: StructuredBuffer<VgMaterialGpu>。アセットごとの連続区間（VgAssetGpu::materialBase から materialCount 個）──
//   テクスチャはアプリの SRV ヒープの添字（ResourceDescriptorHeap[]）。VG_NONE = 無し（アルベドは白、他は flags で読まない）。
//   自己発光は Material.h の ResolveEmissiveParams の入力（色 / 強度の float）。インスタンスの上書きと合成してから
//   PackEmissive と同じ 8bit 量子化を通す（フォワードの b2 と同じ値になる）。
struct VgMaterialGpu
{
    uint32_t albedoSrv;        //  0
    uint32_t normalSrv;        //  4
    uint32_t metalRoughSrv;    //  8
    uint32_t emissiveSrv;      // 12
    uint32_t flags;            // 16  VG_MAT_*（bit0 法線 / bit1 MR / bit3 自己発光テクスチャ / bit4 両面）
    float    metallic;         // 20  Material::defaultMetallic 相当
    float    roughness;        // 24  同 defaultRoughness
    uint32_t reserved0;        // 28
    float    uvScaleOffset[4]; // 32  xy = スケール / zw = オフセット（MaterialRecord.uvScaleOffset）
    float    emissiveColor[3]; // 48
    float    emissiveIntensity;// 60
};
static_assert(sizeof(VgMaterialGpu) == VG_MAT_STRIDE, "VgMaterialGpu size");
static_assert(offsetof(VgMaterialGpu, uvScaleOffset) == 32 && offsetof(VgMaterialGpu, emissiveColor) == 48, "VgMaterialGpu layout");

// ── アセット表（96 B）: StructuredBuffer<VgAssetGpu> ───────────────────────────
struct VgAssetGpu
{
    float    boundingSphere[4];  //  0
    float    aabbMin[3];         // 16
    uint32_t nodesSrv;           // 28  この VG システム専用ヒープ内の StructuredBuffer<HierNode> の添字
    float    aabbMax[3];         // 32
    uint32_t poolSrvBase;        // 44  ページプールのチャンク SRV の先頭添字（ByteAddressBuffer × chunkCount）
    float    posOrigin[3];       // 48
    float    posStep;            // 60
    uint32_t rootNode;           // 64
    uint32_t pageCount;          // 68
    uint32_t sourceTriangles;    // 72  LOD0 の三角形数（u32 に丸める。4G 超は飽和）
    uint32_t levelCount;         // 76
    uint32_t clusterCount;       // 80
    uint32_t materialCount;      // 84
    uint32_t materialBase;       // 88  P4: 材質表（VgMaterialGpu）でのこのアセットの先頭。VG_NONE = 未登録（既定材質で描く）
    uint32_t reserved;           // 92
};
static_assert(sizeof(VgAssetGpu) == 96, "VgAssetGpu size");
static_assert(offsetof(VgAssetGpu, nodesSrv) == 28 && offsetof(VgAssetGpu, poolSrvBase) == 44 &&
              offsetof(VgAssetGpu, rootNode) == 64 && offsetof(VgAssetGpu, materialBase) == 88, "VgAssetGpu layout");

// ページプールのチャンク: 1 本の ByteAddressBuffer が持つページ数。512 MiB = 4096 ページ。
// ★D3D12 の 1 リソースの上限（実装依存。仕様の保証は max(128MB, VRAM/4) を 2GB で頭打ち）に余裕を持って収める。
//   ページはチャンクを跨がない（131072 B が 512 MiB を割り切る）。シェーダ側の定数は VgCommon.hlsli の VG_CHUNK_PAGES_LOG2。
inline constexpr uint32_t kVgChunkPagesLog2 = 12;
inline constexpr uint32_t kVgChunkPages     = 1u << kVgChunkPagesLog2;
inline constexpr uint64_t kVgPageBytes      = 131072ull;

// ── カリングの定数バッファ（b0。384 B → 512 B 刻みでリングに置く）─────────────────
struct VgCullConstants
{
    float    viewProj[16];       //   0  転置して格納（HLSL の float4x4 = 元の行列。HiZCull と同じ作法）
    float    prevViewProj[16];   //  64  前フレーム（HZB 一相目の投影用）
    float    planes[6][4];       // 128  錐台 6 平面（今フレーム）
    float    camPos[4];          // 224  xyz, w = zNear
    float    viewport[4];        // 240  x, y, w, h（px）
    float    lodParams[4];       // 256  x = τ（px）, y = projScale, z = instanceMinPx, w = 0
    float    hzbParams[4];       // 272  x, y = mip0 の解像度, z = ミップ数, w = 0
    uint32_t counts[4];          // 288  x = instanceCount, y = hzbPrevValid, z = hzbCurValid, w = coneCulling
    uint32_t heap0[4];           // 304  x = assetsSrv, y = instancesSrv, z = workUav, w = instExtraUav
    uint32_t heap1[4];           // 320  x = hzbPrevSrv, y = hzbCurSrv, z = argsUav, w = materialsSrv（P4）
    uint32_t caps[4];            // 336  x = nodeQCap, y = groupQCap, z = visibleCap, w = deferredCap
    uint32_t offs0[4];           // 352  x = nodeQ0, y = nodeQ1, z = deferred, w = groupQ0（作業バッファ内のバイト位置）
    uint32_t offs1[4];           // 368  x = groupQ1, y = visible, z = counters, w = stats
    // ── P3（ラスタ / デバッグ描画）──
    uint32_t heap2[4];           // 384  x = depthSrv(R32_FLOAT), y = visSrv(R32_UINT), z = overdrawUav(R32_UINT), w = visUav
    uint32_t rast[4];            // 400  x = ラスタのフラグ(VG_RF_*), y = 可視性バッファの幅, z = 高さ, w = 0
    float    dbg[4];             // 416  x = 深度の表示レンジ(m), y = 過剰描画の表示上限, z = zFar, w = 0
    // ── P4（G-Buffer / 速度パス・材質の可視化）──
    float    viewProjNJ[16];     // 432  今フレームのジッタ無し VP（速度用。転置格納）
    float    prevViewProjNJ[16]; // 496  前フレームのジッタ無し VP（速度用。転置格納）
};
static_assert(sizeof(VgCullConstants) == 560, "VgCullConstants size");
static_assert(offsetof(VgCullConstants, planes) == 128 && offsetof(VgCullConstants, camPos) == 224 &&
              offsetof(VgCullConstants, counts) == 288 && offsetof(VgCullConstants, offs1) == 368 &&
              offsetof(VgCullConstants, heap2) == 384 && offsetof(VgCullConstants, dbg) == 416 &&
              offsetof(VgCullConstants, viewProjNJ) == 432 && offsetof(VgCullConstants, prevViewProjNJ) == 496,
              "VgCullConstants layout");

// ── P4: resolve のルート定数（メイン RS の b0 = 40 DWORD を読み替える。28 DWORD 使う）──
//   shaders/vg/VgResolve.hlsl の cbuffer VgResolveCB と 1 バイトずつ一致させる。
struct VgResolveConstants
{
    float    viewProjJ[16];      //  0  ジッタ付き VP（ラスタと同じ。転置格納）
    uint32_t heap0[4];           // 64  x = visSrv, y = workUav, z = instancesSrv, w = assetsSrv（アプリの SRV ヒープの添字）
    uint32_t heap1[4];           // 80  x = materialsSrv, y = 可視リストのバイト位置, z = デバッグモード（VG_DBG_* / 0 = 通常）, w = フラグ
    float    viewport[4];        // 96  x = 幅, y = 高さ, z = 1/幅, w = 1/高さ（レンダー解像度 px）
};
static_assert(sizeof(VgResolveConstants) == 112 && sizeof(VgResolveConstants) / 4 <= 40, "VgResolveConstants size");

// パスごとに変わる値（ルート定数 8 DWORD）。
struct VgPassConstants
{
    uint32_t pass0[4];
    uint32_t pass1[4];
};
static_assert(sizeof(VgPassConstants) == 32, "VgPassConstants size");

// 作業バッファ（VgWork）のレイアウトを容量から決める。
struct VgWorkLayout
{
    uint32_t nodeQCap = 0, groupQCap = 0, visibleCap = 0, deferredCap = 0;
    uint32_t nodeQ0 = 0, nodeQ1 = 0, deferred = 0, groupQ0 = 0, groupQ1 = 0, visible = 0;
    uint32_t counters = VG_WORK_COUNTERS_OFFSET, stats = VG_WORK_STATS_OFFSET;
    uint64_t totalBytes = 0;
};

inline VgWorkLayout MakeWorkLayout(uint32_t nodeQCap, uint32_t groupQCap, uint32_t visibleCap, uint32_t deferredCap)
{
    VgWorkLayout l;
    l.nodeQCap = nodeQCap; l.groupQCap = groupQCap; l.visibleCap = visibleCap; l.deferredCap = deferredCap;
    uint64_t at = VG_WORK_QUEUES_OFFSET;
    auto take = [&](uint32_t cap) { const uint32_t o = static_cast<uint32_t>(at); at += static_cast<uint64_t>(cap) * 8ull; return o; };
    l.nodeQ0   = take(nodeQCap);
    l.nodeQ1   = take(nodeQCap);
    l.deferred = take(deferredCap);
    l.groupQ0  = take(groupQCap);
    l.groupQ1  = take(groupQCap);
    l.visible  = take(visibleCap);
    l.totalBytes = at;
    return l;
}

} // namespace dx12e::vg
