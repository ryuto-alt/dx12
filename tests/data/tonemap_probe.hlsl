// tests/tonemap_gpu_test.cpp 用の probe。shaders/post/Tonemap.hlsli の新しいトーンマップ（3 = UE Filmic / 4 = 線形 / 5 = PBR Neutral）を
// そのまま呼び、表示(ガンマ)空間の値を書き出す。C++ の参照（renderer/PhotometricMath.h）と突き合わせる。
#include "post/Tonemap.hlsli"

cbuffer C : register(b0)
{
    uint  mode;
    uint  count;
    float whiteClip;   // film1.x（定数は 8 DWORD なので 4 番目の余白へ詰めてある）
    float pad;
    float4 film0;      // slope, toe, shoulder, blackClip
};

StructuredBuffer<float4>   gIn  : register(t0);
RWStructuredBuffer<float4> gOut : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= count) return;
    const float3 c = gIn[id.x].rgb;
    gOut[id.x] = float4(UnoToneMapNew((int)mode, c, film0, whiteClip), 1.0);
}
