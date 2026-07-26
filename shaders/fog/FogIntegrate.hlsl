// FogIntegrate.hlsl - パス③: Z 方向へ前方から後方へ積分する（cs_6_0 / CSIntegrate）。
//
// 1 スレッド = 1 本の視線レイ（froxel の xy 列）。64 スライスを順に舐めて
//   .rgb = カメラからそのスライスまでの累積 in-scattering
//   .a   = 同区間の transmittance
// を書く。合成パスは深度から w を作ってこのボリュームをトライリニアサンプルするだけでよくなる。

#include "FogCommon.hlsli"

Texture3D<float4>   g_scatter    : register(t1);
RWTexture3D<float4> g_integrated : register(u0);

[numthreads(8, 8, 1)]
void CSIntegrate(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy >= uint2(FROXEL_X, FROXEL_Y))) return;

    // 視線レイの「view 空間 Z 1 単位あたりの実距離」。画面端ほど 1 より大きい。
    // これを掛けないと画面周辺のフォグが薄くなる。
    float2 uv  = (float2(tid.xy) + 0.5) / float2(FROXEL_X, FROXEL_Y);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 ray = float3(ndc.x * gProjParams.x, ndc.y * gProjParams.y, 1.0);
    float  rayLen = length(ray);

    float3 accumScatter = 0.0;
    float  accumTrans   = 1.0;
    float  prevZ        = 0.0;

    [loop]
    for (uint z = 0; z < FROXEL_Z; ++z)
    {
        float nextZ = FroxelWToViewZ(float(z + 1) / float(FROXEL_Z));
        float dz    = max(nextZ - prevZ, 0.0) * rayLen;   // このスライスの実際の光路長
        prevZ = nextZ;

        float4 s      = g_scatter[uint3(tid.xy, z)];
        float  sigmaT = max(s.a, 1.0e-6);

        // ★Hillaire (Frostbite SIGGRAPH 2015 スライド 28) のエネルギー保存解析積分。
        //   scattering は froxel あたり 1 サンプルでよいが、transmittance はスライスの
        //   奥行き方向に積分しないとエネルギーが保存しない（スライスが厚いほど誤差が出る）。
        //     T       = exp(-σ_t d)
        //     ∫S·T dx = (S - S·T) / σ_t
        float  T     = exp(-sigmaT * dz);
        float3 integ = (s.rgb - s.rgb * T) / sigmaT;

        accumScatter += integ * accumTrans;
        accumTrans   *= T;

        g_integrated[uint3(tid.xy, z)] = float4(FogSanitize(accumScatter), saturate(accumTrans));
    }
}
