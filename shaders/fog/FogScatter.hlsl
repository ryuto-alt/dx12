// FogScatter.hlsl - パス②: froxel ごとの in-scattering を計算し時間再投影する（cs_6_0 / CSScatter）。
//
// 出力 g_scatter:
//   .rgb = in-scattered radiance（σ_s 乗算済み）
//   .a   = 消散係数 σ_t
//
// ★Hillaire (Frostbite SIGGRAPH 2015): 「消散係数は線形なので、非線形な transmittance ではなく
//   extinction を時間積分するほうがよい」。よって時間ブレンドは (scattering, extinction) に対して行い、
//   transmittance は最後の積分パス（FogIntegrate.hlsl）で計算する。

#define FOG_SCATTER 1
#include "FogCommon.hlsli"
#include "../forward/ClusterCommon.hlsli"

Texture3D<float4>   g_media    : register(t1);
Texture3D<float4>   g_history  : register(t2);   // 前フレームの scatter ボリューム
SamplerState        g_linClamp : register(s1);
RWTexture3D<float4> g_scatter  : register(u0);

// クラスタードライティング（計画02）の成果物をそのまま引く。
// ★t3..t5 は ClusteredLightCulling::GetSrvTable() の先頭 3 本（フォワードの t13..t15 と同じ並び）。
//   t6,t7 は Application の m_spotShadowSrvIndex から 2 本連番（フォワードの t9,t10 と同じ）。
StructuredBuffer<ClusterLight> g_clusterLights     : register(t3);
StructuredBuffer<uint>         g_clusterLightIndex : register(t4);
StructuredBuffer<uint>         g_clusterLightCount : register(t5);
Texture2DArray                 g_spotShadow        : register(t6);
TextureCubeArray               g_pointShadow       : register(t7);

// スポット影（1 タップ）。Lighting.hlsli の SampleSpotShadow の 3x3 PCF を 1 タップに落としたもの。
// froxel は低解像度 + トライリニア + 時間再投影が掛かるので 1 タップで十分。
float FogSampleSpotShadow(int idx, float3 worldPos)
{
    float4 clip = mul(float4(worldPos, 1.0), gSpotShadowMatrix[idx]);
    if (clip.w <= 0.0) return 1.0;
    float3 ndc = clip.xyz / clip.w;
    float2 uv  = ndc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;
    return g_spotShadow.SampleCmpLevelZero(g_fogShadowSamp, float3(uv, (float)idx),
                                           ndc.z - gShadowParams.y);
}

// ポイント影。Lighting.hlsli の SamplePointShadow と同一（元から 1 タップ）。
float FogSamplePointShadow(int idx, float3 fromLight, float range)
{
    float dist = max(max(abs(fromLight.x), abs(fromLight.y)), abs(fromLight.z));
    float n = gProjParams.w;                    // pointShadowNear
    float f = max(range, n + 0.01);
    float current = (f / (f - n)) - (f * n) / (f - n) / max(dist, 0.0001) - gShadowParams.y;
    return g_pointShadow.SampleCmpLevelZero(g_fogShadowSamp, float4(fromLight, (float)idx), current);
}

[numthreads(8, 8, 1)]
void CSScatter(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid >= uint3(FROXEL_X, FROXEL_Y, FROXEL_Z))) return;

    float4 media  = g_media[tid];
    float3 sigmaS = media.rgb;
    float  sigmaT = media.a;

    // ライティングはジッタ込みの位置で評価する（時間方向にジッタを回して収束させる）。
    float3 vp = FroxelToViewPos(float3(tid) + 0.5 + gJitter.xyz);
    float3 wp = FogViewToWorld(vp);

    float3 toCam = gCameraPos - wp;
    float3 V = (dot(toCam, toCam) > 1e-10) ? normalize(toCam) : float3(0.0, 0.0, 1.0);  // froxel → カメラ

    // --- 環境（アンビエント）散乱: 等方なので位相関数は 1/4π ---
    float3 inscatter = gAmbient * FOG_ISOTROPIC_PHASE;

    // --- 太陽 ---
    {
        float3 L      = normalize(-gSunDir);    // froxel → 太陽（Forward.hlsl:183 と同じ規約）
        float  shadow = FogSampleCsm(wp, vp.z);
        // ★cosTheta には dot(-V, L) を渡す。dot(V,L) だと g>0 が後方散乱になり
        //   「太陽を背にしたときだけ光る」逆の絵になる。
        float  phase  = HenyeyGreenstein(dot(-V, L), gAnisotropy);
        inscatter += gSunColor * gMisc.w * shadow * phase;
    }

    // --- 点光源 / スポット（計画02 のクラスタライトリストを引く。Step F5）---
    // ★フォグは冪分布、クラスタは指数分布なので、必ず view 空間 Z を経由して変換する。
    //   froxel の (x,y) はクラスタのタイルへ比率で写す（どちらも同じビューポートを張る）。
    //   ここは SV_Position ではないので clusterViewport.xy の原点引きは不要。
    if (gClusterGrid.w > 0.5)
    {
        uint2 tile = (uint2)clamp(float2(tid.xy) / float2(FROXEL_X, FROXEL_Y) * gClusterGrid.xy,
                                  float2(0.0, 0.0), gClusterGrid.xy - 1.0);
        uint  slice = ClusterSliceFromViewZ(vp.z, gClusterParams.x,
                                            gClusterParams.z, gClusterParams.w, gClusterGrid.z);
        uint  ci    = ClusterLinearIndex(tile.x, tile.y, slice,
                                         (uint)gClusterGrid.x, (uint)gClusterGrid.y);
        uint  first = ci * (uint)gLightCounts.y;
        uint  count = g_clusterLightCount[ci];

        [loop]
        for (uint k = 0; k < count; ++k)
        {
            ClusterLight Lt = g_clusterLights[g_clusterLightIndex[first + k]];

            float3 d    = Lt.position - wp;
            float  dist = length(d);
            if (dist >= Lt.range) continue;          // 減衰 0。影サンプルを丸ごと省く
            float3 Ldir = d / max(dist, 0.0001);

            // ★減衰式・コーン式は Lighting.hlsli の AccumulatePunctualLights と完全同一に保つこと。
            float att = saturate(1.0 - dist / Lt.range);
            att *= att;
            float3 radiance = Lt.color * att;

            if (Lt.type > 0.5)   // spot
            {
                float cd   = dot(Lt.direction, -Ldir);
                float cone = saturate((cd - Lt.cosOuter) / max(Lt.cosInner - Lt.cosOuter, 0.001));
                cone *= cone;
                if (cone <= 0.0) continue;
                radiance *= cone;
                if (Lt.shadowIndex >= 0.0)
                    radiance *= FogSampleSpotShadow((int)Lt.shadowIndex, wp);
            }
            else                 // point
            {
                if (Lt.shadowIndex >= 0.0)
                    radiance *= FogSamplePointShadow((int)Lt.shadowIndex, -d, Lt.range);
            }

            inscatter += radiance * HenyeyGreenstein(dot(-V, Ldir), gAnisotropy);
        }
    }

    float4 cur = float4(FogSanitize(inscatter * sigmaS), max(sigmaT, 0.0));

    // --- 時間再投影 ---
    // 再投影に使うのは「ジッタなしの froxel 中心」。ジッタ込みの位置で引くと
    // 履歴が自分自身のジッタを追いかけて収束しない。
    float alpha = gJitter.w;    // 現フレームの比率（1.0 = 履歴を使わない）
    if (alpha < 0.999)
    {
        float3 wpC = FogViewToWorld(FroxelToViewPos(float3(tid) + 0.5));
        float4 pc  = mul(float4(wpC, 1.0), gPrevViewProj);
        if (pc.w > 1.0e-4)
        {
            float3 pndc = pc.xyz / pc.w;
            float2 puv  = float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5);
            // LH 透視なので clip.w == view 空間 Z。そこから froxel の W を作る。
            float  pw   = ViewZToFroxelW(pc.w);
            if (all(puv > 0.0) && all(puv < 1.0) && pw > 0.0 && pw < 1.0)
            {
                float4 hist = g_history.SampleLevel(g_linClamp, float3(puv, pw), 0);
                hist = float4(FogSanitize(hist.rgb), max(hist.a, 0.0));
                cur  = lerp(hist, cur, alpha);
            }
        }
    }

    g_scatter[tid] = cur;
}
