#pragma once
// b1（PerFrameConstants）の CPU 側の定義。唯一の正本（W05-23）。
//
// ★レイアウトは shaders/forward/Lighting.hlsli の PerFrameConstants と完全一致させること。
//   以前は Application.cpp / ApplicationRender.cpp / ModelThumbnailRenderer.cpp / GraphPreviewRenderer.cpp
//   に写しが 4 つあった。ここへ一本化したので、メンバの追加・並べ替えはこのファイルだけで行う。
// ★HLSL 側の配列名（_clusterReserved）と名前付き欄の切り出し方は変えない（V1 の DXIL を変えないため）。
//
// Application::FrameConstants は `struct Application::FrameConstants : FrameConstantsData {};`
// （ApplicationRender.cpp）。Application.h は変えない。

#include "core/Types.h"

#include <DirectXMath.h>
#include <cstddef>

namespace dx12e
{

inline constexpr u32 kFrameConstantsCascades   = 4;   // = Application::kNumCascades / CASCADE_COUNT (Lighting.hlsli)
inline constexpr u32 kFrameConstantsSpotShadow = 4;   // = Application::kMaxShadowSpot / MAX_SHADOW_SPOT (Lighting.hlsli)

struct FrameConstantsData
{
    DirectX::XMFLOAT4X4 view;                                       // 64B  (offset   0)
    DirectX::XMFLOAT4X4 proj;                                       // 64B  (offset  64)
    DirectX::XMFLOAT3   lightDir;                                   // 12B
    float               time;                                       // 4B  → 16B (offset 128)
    DirectX::XMFLOAT3   lightColor;                                 // 12B
    float               ambientStrength;                            // 4B  → 16B (offset 144)
    DirectX::XMFLOAT4X4 cascadeViewProj[kFrameConstantsCascades];   // 256B (offset 160)
    DirectX::XMFLOAT4   cascadeSplitsView;                          // 16B (offset 416)
    DirectX::XMFLOAT4   shadowParams;                               // 16B (offset 432)
    DirectX::XMFLOAT3   cameraPos;                                  // 12B
    float               aoEnabled;                                  // 4B  → 16B (offset 448) 1=実 AO を読む / 0=読まず ao=1
    u32                 numPointLights;                             // 統計/デバッグ用（シェーダは読まない）
    u32                 numSpotLights;
    float               spotShadowTexel;                            // 1/kSpotShadowMapSize
    float               pointShadowNear;                            // → 16B (offset 464)
    // ▼ クラスタードライティング 64B (offset 480)。旧 pointLights[8]/spotLights[8] の跡地。
    DirectX::XMFLOAT4   clusterParams;                              // .x=zNear .y=zFar(クラスタ用) .z=sliceScale .w=sliceBias
    DirectX::XMFLOAT4   clusterGrid;                                // .x=gridX .y=gridY .z=gridZ .w=クラスタード有効(1/0)
    DirectX::XMFLOAT4   clusterViewport;                            // .xy=ビューポート原点(RT px) .zw=(gridX/vpW, gridY/vpH)
    DirectX::XMFLOAT4   clusterExtra;                               // .x=総灯数 .y=maxLightsPerCluster .z=デバッグ表示 .w=予約
    DirectX::XMFLOAT4   pcssParams;                                 // 16B (offset 544) PCSS: .x=tanTheta(0で無効) .y=maxPenumbraTexels .z=時間ディザ位相 .w=探索半径texel
    // ▼ DDGI 48B (offset 560)。ddgiOrigin.w=0 なら PS は t22 を一切読まない
    DirectX::XMFLOAT4   ddgiOrigin;                                 // 16B (offset 560) .xyz=格子の原点 .w=強さ(0=無効)
    DirectX::XMFLOAT4   ddgiSpacing;                                // 16B (offset 576) .xyz=プローブ間隔 .w=法線バイアス(m)
    DirectX::XMFLOAT4   ddgiCounts;                                 // 16B (offset 592) .xyz=各軸のプローブ数
    DirectX::XMFLOAT4   _clusterReserved[35];                       // 560B (offset 608..1167)。番号は PfSlot
    DirectX::XMFLOAT4   ddgiC1;                                     // 16B (offset 1168) GI S4: カスケード 1（.xyz=原点 .w=間隔）
    DirectX::XMFLOAT4   ddgiScroll0;                                // 16B (offset 1184) カスケード 0 の記憶領域のずらし
    DirectX::XMFLOAT4   ddgiScroll1;                                // 16B (offset 1200)
    // ▼ GI モード 32B (offset 1216)。Legacy は全部 0 ＝従来経路
    DirectX::XMFLOAT4   giParams;                                   // 16B .x=New(1/0) .y=視線バイアス(m) .z=法線バイアス(m) .w=鏡面の空遮蔽の強さ
    DirectX::XMFLOAT4   giParams2;                                  // 16B 予約
    DirectX::XMFLOAT4X4 spotShadowMatrix[kFrameConstantsSpotShadow];// 256B (offset 1248)
    // ▼ IBL 制御 16B (offset 1504)
    float               iblIntensity;
    float               maxPrefilterMip;
    u32                 hasIBL;
    float               skyboxIntensity;
    // ▼ コンタクトシャドウ制御 16B (offset 1520)
    float               contactShadowEnabled;                       // 1=実テクスチャ(t11)を読む / 0=読まず 1.0
    // 法線マップフィルタリング（旧 _csPad の 12B を流用）.x=強さ(0 で恒等) .y=α に足せる量の上限 .z=幾何法線へ寄せる強さ
    DirectX::XMFLOAT3   normalFilterParams;
    // ▼ DDGI のカスケード 2 / 3（末尾追記 64B。既存のオフセットは動かない）
    DirectX::XMFLOAT4   ddgiC2;                                     // (offset 1536)
    DirectX::XMFLOAT4   ddgiScroll2;                                // (offset 1552)
    DirectX::XMFLOAT4   ddgiC3;                                     // (offset 1568)
    DirectX::XMFLOAT4   ddgiScroll3;                                // (offset 1584)
};

static_assert(sizeof(FrameConstantsData) == 1600, "FrameConstants must be 1600 bytes (Lighting.hlsli PerFrameConstants)");
static_assert(offsetof(FrameConstantsData, cascadeViewProj) == 160, "cascadeViewProj offset");
static_assert(offsetof(FrameConstantsData, clusterParams) == 480, "clusterParams offset");
static_assert(offsetof(FrameConstantsData, pcssParams) == 544, "pcssParams offset");
static_assert(offsetof(FrameConstantsData, ddgiOrigin) == 560, "ddgiOrigin offset");
static_assert(offsetof(FrameConstantsData, _clusterReserved) == 608, "_clusterReserved offset");
static_assert(offsetof(FrameConstantsData, ddgiC1) == 1168, "ddgiC1 offset");
static_assert(offsetof(FrameConstantsData, giParams) == 1216, "giParams offset");
static_assert(offsetof(FrameConstantsData, spotShadowMatrix) == 1248, "spotShadowMatrix offset");
static_assert(offsetof(FrameConstantsData, iblIntensity) == 1504, "iblIntensity offset");
static_assert(offsetof(FrameConstantsData, contactShadowEnabled) == 1520, "contactShadowEnabled offset");
static_assert(offsetof(FrameConstantsData, ddgiC2) == 1536, "ddgiC2 offset");
static_assert(offsetof(FrameConstantsData, ddgiScroll3) == 1584, "ddgiScroll3 offset");

// _clusterReserved の float4 番号。1 つの float4 は 1 つの所有者だけが使う。
enum class PfSlot : u32
{
    FoliageWind0 = 0, FoliageWind1 = 1,   // 既存
    ShadingV2    = 2,   // C: x=shadingFlags(asuint) y=microShadowK z=horizonK w=sunAngularRadius
    CsmTexel     = 3,   // C: csmTexelWorld[4]
    Weather      = 4,   // C: wetness, puddle, rain, snow
    PostView     = 5,   // C が書く口を持つ。x=preExposure（値は BD）、y=debugViewMode（値は A）
    LocalShadow  = 6,   // C: アトラスと RT のローカル影
    ReservedC7   = 7,   // C: LS-14 / LS-16 用
    ContactMip   = 8, MultiScatter = 9,   // 既存
    VfxFrame     = 10,  // BD: VFX の共通値
    // 11..33 は W8 以降のために空けておく
    ReflectionProbe = 34  // 既存（C++ 側だけ。HLSL では名前付きの欄）
};

inline DirectX::XMFLOAT4& Pf(FrameConstantsData& fc, PfSlot s) { return fc._clusterReserved[static_cast<u32>(s)]; }
inline const DirectX::XMFLOAT4& Pf(const FrameConstantsData& fc, PfSlot s) { return fc._clusterReserved[static_cast<u32>(s)]; }

} // namespace dx12e
