// DecalCommon.hlsli - クラスタードフォワードデカールの共有定義。
//
// フォワード PS（Forward.hlsl / ForwardSkinned.hlsl）と
// カリング CS（ClusterCullDecals.hlsl）の両方から include する。
//
// 方式: 「クラスタードフォワードデカール」。デカールを計画02 のクラスタグリッドへビニングし、
// フォワード PS の「マテリアル確定直後・ライティング開始前」で albedo/normal/roughness/metallic を
// 書き換える。その後の ShadePunctual / IBL / 影は一切の変更なしにデカールの法線で動く。
//   出典: turanszkij (Wicked Engine), "Forward+ decal rendering"
//         https://turanszkij.wordpress.com/2017/10/12/forward-decal-rendering/
//
// ★C++ 側の実体は src/renderer/DecalSystem.h の DecalGPU（static_assert(sizeof == 176) つき）。
//   片方を変えたら必ず両方直すこと。

#ifndef DECAL_COMMON_HLSLI
#define DECAL_COMMON_HLSLI

// ★src/renderer/DecalSystem.h の kMaxDecals / kMaxPerCluster と一致させること。
#define DECAL_MAX_SCENE        256
#define DECAL_MAX_PER_CLUSTER   16

// 176B。ワールド行列そのものではなく「投影軸/接線をワールドで持つ」ことで、
// PS 側で invWorld を逆行列に戻す必要をなくしている。
struct Decal
{
    float4x4 invWorld;       //   0  world → デカールローカル（単位立方体 [-0.5, 0.5]）
    float4   atlasUV;        //  64  カラー矩形 (u0, v0, du, dv)
    float4   atlasUVNormal;  //  80  法線矩形。.z <= 0 なら法線マップ無し
    float3   tint;      float opacity;         //  96
    float3   axisW;     float normalStrength;  // 112  投影軸（デカールローカル +Y のワールド方向）
    float3   tangentW;  float roughness;       // 128  デカールローカル +X のワールド方向 / <0 = 変更しない
    float3   emissive;  float metallic;        // 144  <0 = 変更しない
    float    cosAngleFade; float fadeEdge; float2 _decalPad;  // 160
};

#endif // DECAL_COMMON_HLSLI
