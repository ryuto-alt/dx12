// ProbeCopyFace.hlsl - 反射プローブ: 撮った 1 面（HDR の 2D）を、ミップ chain 用のキューブ（6 スライスの配列）の mip0 へ書く（cs_6_0）。
// 同時に NaN / Inf / 極端な輝点を落とす（小さな強い光が GGX 畳み込みで大きな蛍にならないよう上限を掛ける）。

Texture2D<float4>        g_src : register(t0);
RWTexture2DArray<float4> g_dst : register(u0);

cbuffer ProbeCopyConstants : register(b0)
{
    uint size;    // 1 面のピクセル数
    uint face;    // 書き込むスライス 0..5
    uint _pad0;
    uint _pad1;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size || id.y >= size) return;
    float3 c = g_src.Load(int3(id.xy, 0)).rgb;
    if (!all(isfinite(c))) c = float3(0.0, 0.0, 0.0);
    c = clamp(c, 0.0, 6000.0);
    g_dst[uint3(id.xy, face)] = float4(c, 1.0);
}
