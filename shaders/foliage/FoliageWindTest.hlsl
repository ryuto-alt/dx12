// FoliageWindTest.hlsl - テスト専用: FoliageWindDelta を N 件評価して FoliageMath.h の WindDelta と GPU = CPU を確かめる。
// 本番では使わない（tests/foliage_gpu_test.cpp だけが読む）。
// 1 レコード = 32 float（wp(3) origin(3) nrm(3) localY vc(3) phase windScale scale t pad(3) windA(4) windB(4) layer(4)）。
#include "FoliageWind.hlsli"

ByteAddressBuffer   g_in  : register(t0);
RWByteAddressBuffer g_out : register(u0);

cbuffer TestCB : register(b0)
{
    uint4 gCount;   // x = 件数
};

[numthreads(64, 1, 1)]
void CSWindTest(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gCount.x) return;
    const uint base = dtid.x * 128u;   // 32 float × 4B
    float f[32];
    [unroll]
    for (uint i = 0; i < 32u; ++i) f[i] = asfloat(g_in.Load(base + i * 4u));
    const float3 wp = float3(f[0], f[1], f[2]);
    const float3 origin = float3(f[3], f[4], f[5]);
    const float3 nrm = float3(f[6], f[7], f[8]);
    const float localY = f[9];
    const float3 vc = float3(f[10], f[11], f[12]);
    const float phase = f[13], windScale = f[14], scale = f[15], t = f[16];
    const FoliageWindFrame wf = FoliageMakeWindFrame(float4(f[20], f[21], f[22], f[23]), float4(f[24], f[25], f[26], f[27]));
    FoliageWindLayer wl;
    wl.bend = f[28]; wl.flutter = f[29]; wl.bendExp = f[30]; wl.heightLocal = f[31];
    const float3 d = FoliageWindDelta(wp, origin, nrm, localY, vc, phase, windScale, scale, wf, wl, t);
    g_out.Store3(dtid.x * 16u, asuint(d));   // 16B ストライド（float3 + pad）
}
