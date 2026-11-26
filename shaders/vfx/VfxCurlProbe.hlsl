// VfxCurlProbe.hlsl - 試験用（W1-BD4。tests/vfx_gpu_test.cpp）: カール場の Texture3D を、本番（VfxCommon.hlsli の kOpCurl）と同じサンプラ設定
//   （線形・ラップ・SampleLevel 0）で uvw の列に対して引いて返す。ハードウェアの三重線形フィルタの量子化を含めて CPU 参照と比べるため。
// ルートシグネチャ: b0 = { curlSrv, count, 0, 0 }（4 DWORD）, t0 = gUvw（root SRV）, u0 = gOut（root UAV）, s0 = 線形ラップ（静的）
cbuffer ProbeCB : register(b0)
{
    uint4 pc;   // x = ResourceDescriptorHeap の番号, y = 件数
};

StructuredBuffer<float4>   gUvw : register(t0);
RWStructuredBuffer<float4> gOut : register(u0);
SamplerState gLinearWrap : register(s0);

[numthreads(64, 1, 1)]
void CSProbe(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= pc.y) return;
    Texture3D<float4> curl = ResourceDescriptorHeap[pc.x];
    gOut[dtid.x] = float4(curl.SampleLevel(gLinearWrap, gUvw[dtid.x].xyz, 0).xyz, 0.0);
}
