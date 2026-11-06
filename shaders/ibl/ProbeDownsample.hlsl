// ProbeDownsample.hlsl - 反射プローブ: キューブ（6 スライスの配列）の mip m を 2x2 平均して mip m+1 を作る（cs_6_0）。
// GGX プリフィルタ（PrefilterEnv.hlsl）が PDF から選んだミップを引くので、蛍を抑えるにはソースにミップ列が要る。

Texture2DArray<float4>   g_src : register(t0);   // SRV: MostDetailedMip = m, MipLevels = 1
RWTexture2DArray<float4> g_dst : register(u0);   // UAV: MipSlice = m + 1

cbuffer ProbeDownConstants : register(b0)
{
    uint dstSize;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= dstSize || id.y >= dstSize) return;
    const int2 s = int2(id.xy) * 2;
    const float4 a = g_src.Load(int4(s + int2(0, 0), id.z, 0));
    const float4 b = g_src.Load(int4(s + int2(1, 0), id.z, 0));
    const float4 c = g_src.Load(int4(s + int2(0, 1), id.z, 0));
    const float4 d = g_src.Load(int4(s + int2(1, 1), id.z, 0));
    g_dst[id] = float4((a.rgb + b.rgb + c.rgb + d.rgb) * 0.25, 1.0);
}
