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

Texture3D<float4>   g_media    : register(t1);
Texture3D<float4>   g_history  : register(t2);   // 前フレームの scatter ボリューム
SamplerState        g_linClamp : register(s1);
RWTexture3D<float4> g_scatter  : register(u0);

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
