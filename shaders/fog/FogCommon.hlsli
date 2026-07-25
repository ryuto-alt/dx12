// FogCommon.hlsli - froxel（frustum-aligned voxel）ボリュメトリックフォグの共有定義。
//
// 4 本のシェーダが include する:
//   FogInject.hlsl    … 媒質パラメータ（散乱係数 σ_s / 消散係数 σ_t）を 3D テクスチャへ注入
//   FogScatter.hlsl   … 太陽 + CSM（+ クラスタライト）の in-scattering を計算し時間再投影
//   FogIntegrate.hlsl … Z 方向の解析積分（in-scattering 累積 + transmittance）
//   FogComposite.hlsl … シーン RT へブレンド合成（フルスクリーン三角形）
//
// 原典: Bart Wroński, "Volumetric Fog: Unified Compute Shader Based Solution to
//       Atmospheric Scattering", SIGGRAPH 2014
//       Sébastien Hillaire, "Physically Based and Unified Volumetric Rendering in
//       Frostbite", SIGGRAPH 2015（extinction を時間積分する / 解析積分の式）
//
// ★ FROXEL_X/Y/Z は src/renderer/VolumetricFogPass.h の kFroxelX/Y/Z と一致させること。
//   HLSL と C++ を突き合わせる static_assert が無いので、片方を変えたら必ず両方直す。

#ifndef FOG_COMMON_HLSLI
#define FOG_COMMON_HLSLI

#define FROXEL_X 160
#define FROXEL_Y  90
#define FROXEL_Z  64

#define FOG_PI 3.14159265359

// b0: 全 4 パス共通の定数バッファ。
// C++ 側は src/renderer/VolumetricFogPass.cpp の FogParamsCB（static_assert(sizeof == 624) つき）。
cbuffer FogParams : register(b0)
{
    float4x4 gInvView;            //   0  view → world（転置済み。★transpose(view) の近似はしない）
    float4x4 gPrevViewProj;       //  64  前フレームの world → clip（ジッタなし・転置済み）
    float4x4 gCascadeViewProj[4]; // 128  CSM（Forward.hlsl の cascadeViewProj と同じもの）
    float4   gCascadeSplits;      // 384  view 空間深度（正値）。.x > 1e8 なら CSM 無効（正射）
    float4   gShadowParams;       // 400  .x=1/shadowMapSize .y=depthBias .z=blendBand .w=未使用
    float3   gCameraPos;  float gFogFar;        // 416  froxel ボリュームの far（設定 distance）
    float3   gSunDir;     float gDepthPower;    // 432  gSunDir = 光が進む方向（Lighting の lightDir と同義）
    float3   gSunColor;   float gAnisotropy;    // 448  Henyey-Greenstein の g
    float3   gAlbedo;     float gDensity;       // 464  σ_s = density * albedo / σ_t = density
    float3   gAmbient;    float gHeightFalloff; // 480
    float4   gJitter;     // 496  .xyz = サブ froxel ジッタ [-0.5,0.5] / .w = temporalBlend（1=履歴なし）
    float4   gMisc;       // 512  .x=heightRef .y=extendBeyondRange(0/1) .z=debugMode .w=sunIntensity
    float4   gProjParams; // 528  .xy = (1/proj._11, 1/proj._22) / .zw = 未使用
    float4   gClusterParams; // 544  計画02 と同じ .x=zNear .y=zFarCluster .z=sliceScale .w=sliceBias
    float4   gClusterGrid;   // 560  .xyz = gridX,gridY,gridZ / .w = クラスタード有効(1/0)
    float4   gRect;       // 576  .xy = ビューポート原点(RT px) / .zw = ビューポートサイズ(px)
    float4   gDepthLin;   // 592  .x=nearZ .y=farZ / .zw=未使用
    float4   gExtend;     // 608  .xyz = 遠方フォールバックの in-scatter 色 / .w = 遠方 σ_t
};

// ---------------------------------------------------------------------------
// HDR のサニタイズ。
// ★fp16 の上限を超える値（HDR 環境キューブの太陽等）を掛け合わせると Inf が伝播し、
//   ACES トーンマップで Inf/Inf = NaN になって全ジオメトリが黒く落ちる。実際に踏んだ事故。
//   shaders/screenspace/ScreenSpaceCommon.hlsli の SS_Sanitize と同じ規約（60000 = fp16 上限手前）。
// ---------------------------------------------------------------------------
float  FogSanitizeF(float v) { return isnan(v) ? 0.0 : min(max(v, 0.0), 60000.0); }
float3 FogSanitize(float3 c) { return float3(FogSanitizeF(c.x), FogSanitizeF(c.y), FogSanitizeF(c.z)); }

// ---------------------------------------------------------------------------
// froxel ↔ view 空間の Z 分布 —— ★指数分布ではなく冪分布
//   z(w) = gFogFar * w^k   /   w(z) = (z / gFogFar)^(1/k)
// 指数分布（クラスタライティングが使っているもの）だと 64 スライスのうち十数枚を
// カメラ前 1m 以内に浪費する（zNear=0.1 / zFar=150 で スライス0 の厚さ 1.2cm）。
// 冪分布は式に zNear が現れない ＝ カメラの真ん前 z=0 から連続的にフォグがかかる。
// ---------------------------------------------------------------------------
float FroxelWToViewZ(float w) { return gFogFar * pow(max(w, 0.0), gDepthPower); }
float ViewZToFroxelW(float z) { return pow(saturate(z / max(gFogFar, 1e-4)), 1.0 / gDepthPower); }

// froxel の連続座標（tid + 0.5 + ジッタ）→ view 空間位置。
// NDC → z=1 平面上のレイ（ClusterBuild.hlsl の ViewRay と同じ導出）を viewZ 倍する。
float3 FroxelToViewPos(float3 coord)
{
    float2 uv  = coord.xy / float2(FROXEL_X, FROXEL_Y);       // 0..1
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);  // D3D: 画面 y は下向き / NDC y は上向き
    float  vz  = FroxelWToViewZ(coord.z / float(FROXEL_Z));
    return float3(ndc.x * gProjParams.x * vz, ndc.y * gProjParams.y * vz, vz);  // LH: 前方 +Z
}

// view 空間位置 → world。gInvView は CPU 側で XMMatrixInverse(view) を転置して送っている
// （view 行列が回転+平行移動だけという前提に頼らない）。
float3 FogViewToWorld(float3 vp) { return mul(float4(vp, 1.0), gInvView).xyz; }

// ---------------------------------------------------------------------------
// Henyey-Greenstein 位相関数
//   p(cosθ) = (1/4π) (1-g²) / (1+g²-2g cosθ)^{3/2}     （球面上の積分は 1）
// ★呼び出し側は cosTheta = dot(-V, L) を渡すこと（V = froxel→カメラ / L = froxel→ライト）。
//   dot(V,L) を渡すと g>0 が後方散乱になり「太陽を背にしたときだけ光る」逆の絵になる。
// ---------------------------------------------------------------------------
float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float d  = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (4.0 * FOG_PI * d * sqrt(d));
}

// 等方散乱の位相関数値（アンビエント用）
#define FOG_ISOTROPIC_PHASE (1.0 / (4.0 * FOG_PI))

// ---------------------------------------------------------------------------
// CSM サンプリング（1 タップ）— ★FOG_SCATTER のときだけ宣言する。
//
// shaders/forward/Forward.hlsl の SelectCascade / SampleCascade / CalcShadow の規約を
// 「意図的に複製」している（共通 .hlsli へ切り出すと他の実装計画と競合するため）。
// ★Forward.hlsl のカスケード規約（cascadeSplitsView.x > 1e8 = 正射で無影 /
//   proj.z - shadowParams.y のバイアス / uv.y = 1-uv.y）を変えたらここも必ず同時に直すこと。
//
// froxel は 921,600 個あるので 3x3 PCF（8.3M タップ）は過剰。1 タップで十分
// （froxel が元々低解像度でトライリニア + 時間再投影が掛かる）。
// ---------------------------------------------------------------------------
#ifdef FOG_SCATTER
Texture2DArray         g_csmShadowMap  : register(t0);
SamplerComparisonState g_fogShadowSamp : register(s0);

float FogSampleCsm(float3 worldPos, float viewDepth)
{
    if (gCascadeSplits.x > 1.0e8) return 1.0;   // 正射カメラ = CSM 無効（Forward.hlsl と同じ規約）

    int c = 3;
    [unroll] for (int i = 0; i < 4; ++i) { if (viewDepth <= gCascadeSplits[i]) { c = i; break; } }

    float4 lc   = mul(float4(worldPos, 1.0), gCascadeViewProj[c]);
    if (lc.w <= 0.0) return 1.0;
    float3 proj = lc.xyz / lc.w;
    float2 uv   = proj.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;

    return g_csmShadowMap.SampleCmpLevelZero(g_fogShadowSamp,
               float3(uv, (float)c), proj.z - gShadowParams.y);
}
#endif // FOG_SCATTER

#endif // FOG_COMMON_HLSLI
