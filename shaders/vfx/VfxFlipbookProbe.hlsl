// VfxFlipbookProbe.hlsl - 試験用（W1-BD7。tests/vfx_gpu_test.cpp）: VfxFlipbook.hlsli / VfxSixWay.hlsli の算術を、本番と同じ
//   サンプラ（線形・クランプ・SampleLevel 0）とテクスチャ（NB2 の焼き込みの DDS）で走らせて返す。CPU 参照（VfxFlipbook.cpp）と比べるため。
// ルートシグネチャ: b0 = ProbeCB（12 DWORD）, t0 = gIn（root SRV）, t1 = gFr（root SRV）, u0 = gOut（root UAV）, s0 = 線形クランプ（静的）
//   ヒープ: 0 = 炎（fire_flipbook）, 1 = 炎の MV, 2 = RLU, 3 = DBF, 4 = 煙の MV
#define UNO_VFX_NO_OPS
#include "VfxCommon.hlsli"
#include "VfxSixWay.hlsli"

cbuffer ProbeCB : register(b0)
{
    uint4  pc;   // x = 件数, y = cols, z = rows, w = 0
    float4 pf;   // x = 炎の mvRange, y = 煙の mvRange
    float4 pl;   // xyz = 光（ビルボード空間。正規化前）
};

StructuredBuffer<float4> gIn : register(t0);   // x, y = 枠内 uv, z = t, w = tLife
StructuredBuffer<uint4>  gFr : register(t1);   // x = a, y = b
RWStructuredBuffer<float4> gOut : register(u0);   // 件数 * 4: 炎の補間テクセル, 炎の ShadeFire, 煙の補間 RLU, 煙の ShadeSmoke
SamplerState gLinearClamp : register(s0);

[numthreads(64, 1, 1)]
void CSProbe(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= pc.x) return;
    float4 q = gIn[dtid.x];
    uint2 fr = gFr[dtid.x].xy;
    uint2 grid = pc.yz;
    float2 uv = q.xy;
    float t = q.z;

    Texture2D<float4> fire = ResourceDescriptorHeap[0];
    Texture2D<float2> fmv  = ResourceDescriptorHeap[1];
    float2 pa, pb;
    FbResolveTaps(fmv, gLinearClamp, grid, pf.x, uv, t, fr.x, fr.y, pa, pb);
    float4 s0 = lerp(FbSampleFrame(fire, gLinearClamp, grid, fr.x, pa), FbSampleFrame(fire, gLinearClamp, grid, fr.y, pb), t);
    FbShaded f = FbShadeFire(s0, q.w, float3(1000.0, 2800.0, 3.0), float2(0.35, 0.15));

    Texture2D<float4> rlu = ResourceDescriptorHeap[2];
    Texture2D<float4> dbf = ResourceDescriptorHeap[3];
    Texture2D<float2> smv = ResourceDescriptorHeap[4];
    FbResolveTaps(smv, gLinearClamp, grid, pf.y, uv, t, fr.x, fr.y, pa, pb);
    float4 r = lerp(FbSampleFrame(rlu, gLinearClamp, grid, fr.x, pa), FbSampleFrame(rlu, gLinearClamp, grid, fr.y, pb), t);
    float4 d = lerp(FbSampleFrame(dbf, gLinearClamp, grid, fr.x, pa), FbSampleFrame(dbf, gLinearClamp, grid, fr.y, pb), t);
    FbShaded s = FbShadeSmoke(r, d, normalize(pl.xyz), q.w, float3(0.5, 0.45, 0.4), float3(0.2, 0.25, 0.3), float3(2.0, 1.8, 1.5), float2(0.35, 0.15));

    uint o = dtid.x * 4u;
    gOut[o + 0] = s0;
    gOut[o + 1] = float4(f.rgb, f.a);
    gOut[o + 2] = r;
    gOut[o + 3] = float4(s.rgb, s.a);
}
