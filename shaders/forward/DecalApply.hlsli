// DecalApply.hlsli - フォワード PS でのデカール適用。
//
// ★include の順序: Lighting.hlsli（ClusterIndexFromPixel / clusterGrid / clusterExtra が要る）と
//   g_sampler(s0) の宣言より後に include すること。Forward.hlsl / ForwardSkinned.hlsl の
//   テクスチャ宣言ブロックの末尾がその位置。
//
// ★t16 = SSR / t17 = SSGI は計画04 の予約なので飛ばして t18 から使う。
//   t18..t21 は kSlotClusterSRV（slot 11）のテーブルのレンジ2として最初から確保されている
//   ＝ ルートシグネチャの DWORD 増分は 0（00-COORDINATION §1）。

#ifndef DECAL_APPLY_HLSLI
#define DECAL_APPLY_HLSLI

#include "DecalCommon.hlsli"

StructuredBuffer<Decal> g_decals     : register(t18);
StructuredBuffer<uint>  g_decalIndex : register(t19);
StructuredBuffer<uint>  g_decalCount : register(t20);
Texture2D               g_decalAtlas : register(t21);

// クラスタに割り当てられたデカールを下から上へ（sortOrder 昇順）通常のアルファブレンドで重ねる。
//
// 呼び出し位置は「albedo / N / metallic / roughness が確定した直後、ライティングを始める前」。
// そうすると後続の ShadePunctual / AccumulatePunctualLights / IBL / 影が
// 一切の変更なしにデカールの法線とマテリアルで動く（前方レンダラでこれができるのがこの方式の要）。
//
// ★ddx/ddy はこの関数の中（早期 return の後）で取る。
//   分岐条件が cbuffer 由来＝ドローの中で完全に一様なので、quad 内のレーンが割れることはない
//   （＝微分は常に有効）。呼び出し側で先に計算して渡すと、デカール 0 個のシーンでも
//   6 命令ぶんとレジスタを常に払うことになる。
void ApplyDecals(float3 worldPos, float2 svPosXY, float viewZ,
                 inout float3 albedo, inout float3 N,
                 inout float metallic, inout float roughness,
                 inout float3 emissiveAdd)
{
    // clusterExtra.w = シーンのデカール数（0 なら丸ごとスキップ）。
    // clusterGrid.w = 0 はクラスタード無効（正射 / カメラプレビュー / サムネイル / 設定 OFF）。
    // ★デカールが 1 個も無いシーンではこの [branch] で 1 命令も走らない。
    [branch]
    if (clusterExtra.w < 0.5 || clusterGrid.w <= 0.5) return;

    float3 dWdx = ddx(worldPos);
    float3 dWdy = ddy(worldPos);

    uint ci    = ClusterIndexFromPixel(svPosXY, viewZ);
    uint first = ci * (uint)DECAL_MAX_PER_CLUSTER;
    uint cnt   = min(g_decalCount[ci], (uint)DECAL_MAX_PER_CLUSTER);

    [loop]
    for (uint k = 0; k < cnt; ++k)
    {
        Decal D = g_decals[g_decalIndex[first + k]];

        // ---- 投影（単位立方体 [-0.5, 0.5] へ）。ここが最頻の早期棄却 ≒ 20 ALU ----
        float3 lp = mul(float4(worldPos, 1.0), D.invWorld).xyz;
        if (any(abs(lp) > 0.5)) continue;

        // ---- 投影角度フェード（急斜面でデカールが引き伸ばされるのを防ぐ標準手法）----
        float nd = dot(N, D.axisW);
        float angleFade = saturate((nd - D.cosAngleFade) / max(1.0 - D.cosAngleFade, 1e-3));
        if (angleFade <= 0.0) continue;

        // ---- 縁フェード（ボックスの 3 軸すべて）----
        float3 e = saturate((0.5 - abs(lp)) / max(D.fadeEdge, 1e-3));
        float  edgeFade = e.x * e.y * e.z;

        // ---- アトラス UV と微分 ----
        // 投影軸はデカールローカル +Y なので、貼られる面はローカル XZ 平面。
        float2 uv = float2(lp.x + 0.5, 0.5 - lp.z);
        float2 dl0 = mul(float4(dWdx, 0.0), D.invWorld).xz;
        float2 dl1 = mul(float4(dWdy, 0.0), D.invWorld).xz;
        float2 duv0 = float2(dl0.x, -dl0.y);
        float2 duv1 = float2(dl1.x, -dl1.y);

        // ★Sample() ではなく SampleGrad() を使う。クラスタ境界やデカール境界で ddx が壊れて
        //   ミップが暴れるため。アトラスを使うときは微分にアトラスのスケールも掛ける。
        //   https://turanszkij.wordpress.com/2017/10/12/forward-decal-rendering/
        float2 auv  = D.atlasUV.xy + saturate(uv) * D.atlasUV.zw;
        float4 tex  = g_decalAtlas.SampleGrad(g_sampler, auv,
                                              duv0 * D.atlasUV.zw, duv1 * D.atlasUV.zw);

        float a = tex.a * D.opacity * angleFade * edgeFade;
        if (a <= 0.0) continue;

        // ---- 法線（アトラスに法線矩形があるときだけ）----
        // デカールの接空間 = (ローカル +X, ローカル +Z の逆, ローカル +Y)。
        if (D.atlasUVNormal.z > 0.0)
        {
            float2 nuv = D.atlasUVNormal.xy + saturate(uv) * D.atlasUVNormal.zw;
            float3 nTS = g_decalAtlas.SampleGrad(g_sampler, nuv,
                                                 duv0 * D.atlasUVNormal.zw,
                                                 duv1 * D.atlasUVNormal.zw).xyz * 2.0 - 1.0;
            float3 T = D.tangentW;
            float3 B = cross(D.axisW, T);
            float3 decalN = normalize(nTS.x * T + nTS.y * B + nTS.z * D.axisW);
            N = normalize(lerp(N, decalN, saturate(a * D.normalStrength)));
        }

        albedo = lerp(albedo, tex.rgb * D.tint, a);
        if (D.roughness >= 0.0) roughness = lerp(roughness, D.roughness, a);
        if (D.metallic  >= 0.0) metallic  = lerp(metallic,  D.metallic,  a);
        emissiveAdd += D.emissive * a;
    }
}

#endif // DECAL_APPLY_HLSLI
