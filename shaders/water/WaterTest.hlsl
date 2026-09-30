// WaterTest.hlsl — テスト専用 compute（tests/water_gpu_test.cpp）。GPU の波・光学の式が CPU 参照（WaterMath.h）と一致するかを確かめる。
//   CSWave  : 入力 (x, z, t, 未使用) の点ごとに変位 + ヤコビアン + 法線を出す（LOD フェード無し）
//   CSOptics: 入力 (cosI, eta, sigma, thickness) ごとにフレネル / 透過率 / 屈折方向を出す
StructuredBuffer<float4>   gWaves : register(t0);   // 波 1 本 = 2 float4（A, B）
StructuredBuffer<float4>   gIn    : register(t1);
RWStructuredBuffer<float4> gOut   : register(u0);

cbuffer TestCB : register(b0)
{
    uint   gWaveCount;
    uint   gCount;
    uint   gPad0, gPad1;
    float2 gFlow;
    float2 gPad2;
};

#define WATER_WAVE_A(i) gWaves[(i) * 2]
#define WATER_WAVE_B(i) gWaves[(i) * 2 + 1]
#include "WaterMath.hlsli"

[numthreads(64, 1, 1)]
void CSWave(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gCount) return;
    const float4 p = gIn[id.x];
    const WaterGerstner g = WaterEvalGerstner(gWaveCount, p.xy, p.z, gFlow, 0.0, 0.0);
    gOut[id.x * 2 + 0] = float4(g.disp, g.jac);
    gOut[id.x * 2 + 1] = float4(g.normal, 0.0);
}

[numthreads(64, 1, 1)]
void CSOptics(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gCount) return;
    const float4 p = gIn[id.x];       // (cosI, eta = n2/n1, sigma, thickness)
    const float f = WaterFresnel(p.x, p.y);
    const float t = exp(-p.z * p.w);
    // 屈折: 入射角 = acos(cosI)。I = (sin, -cos, 0)、N = (0, 1, 0)、eta_refract = 1 / p.y（n1/n2）
    const float sn = sqrt(saturate(1.0 - p.x * p.x));
    bool ok;
    const float3 r = WaterRefract(float3(sn, -p.x, 0.0), float3(0.0, 1.0, 0.0), 1.0 / p.y, ok);
    gOut[id.x * 2 + 0] = float4(f, t, ok ? 1.0 : 0.0, 0.0);
    gOut[id.x * 2 + 1] = float4(r, 0.0);
}
