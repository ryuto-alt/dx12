// 自動露出（eye adaptation）。
// CSHistogram: シーンHDR のログ輝度を 256bin ヒストグラムへ集計（16x16 タイル、shared 集計→global へ atomic）。
// CSAdapt:     並列リダクションで加重平均ログ輝度→時間適応→露出倍率を書き込み、ヒストグラムをクリア。
// 参考: Bruno Opsenica "Automatic Exposure Using a Luminance Histogram" / Alex Tardif
//
// bin0 = ほぼ黒(lum < 1e-5) 専用で、平均からは除外される（外れ値耐性）。

cbuffer AECB : register(b0)
{
    float4 p0;   // x=minLog2, y=1/(maxLog2-minLog2), z=dt, w=適応速度
    float4 p1;   // x=EV補正, y=矩形の総ピクセル数, zw=未使用
    int4   rect; // x=left, y=top, z=width, w=height（シーンRT内の測光サブ矩形 px）
};

Texture2D<float4>        gScene    : register(t0);
RWStructuredBuffer<uint> gHist     : register(u0);  // 256 bins
RWStructuredBuffer<float> gExposure : register(u1); // [0]=露出倍率, [1]=適応済み平均輝度

groupshared uint hs[256];

static uint BinForLum(float lum)
{
    if (lum < 1e-5) return 0u;
    float logNorm = saturate((log2(lum) - p0.x) * p0.y);
    return (uint)(logNorm * 254.0 + 1.0);
}

[numthreads(16, 16, 1)]
void CSHistogram(uint gi : SV_GroupIndex, uint3 id : SV_DispatchThreadID)
{
    hs[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();

    if (id.x < (uint)rect.z && id.y < (uint)rect.w)
    {
        float3 c = gScene.Load(int3(rect.x + (int)id.x, rect.y + (int)id.y, 0)).rgb;
        float lum = dot(c, float3(0.2127, 0.7152, 0.0722));
        InterlockedAdd(hs[BinForLum(lum)], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    InterlockedAdd(gHist[gi], hs[gi]);
}

[numthreads(256, 1, 1)]
void CSAdapt(uint gi : SV_GroupIndex)
{
    uint count = gHist[gi];
    hs[gi] = count * gi;   // bin0 は gi=0 なので分子に寄与しない＝黒を自動除外
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint cutoff = 128u; cutoff > 0u; cutoff >>= 1u)
    {
        if (gi < cutoff) hs[gi] += hs[gi + cutoff];
        GroupMemoryBarrierWithGroupSync();
    }

    if (gi == 0u)
    {
        float numPix   = max(p1.y - (float)gHist[0], 1.0);       // 黒ピクセルを分母から除外
        float meanBin  = (float)hs[0] / numPix;
        float logLum   = ((meanBin - 1.0) / 254.0) / p0.y + p0.x;
        float targetLum = exp2(logLum);

        float prev = gExposure[1];
        if (!(prev > 0.0)) prev = targetLum;                      // 初回/NaN ガード
        float a = saturate(1.0 - exp(-p0.z * p0.w));
        float adapted = lerp(prev, targetLum, a);
        if (a >= 0.999) adapted = targetLum;                      // 初回は即適応（dt 大で強制）

        gExposure[1] = adapted;
        float key = 0.18 * exp2(p1.x);                            // EV補正込みのキー値
        gExposure[0] = key / max(adapted, 1e-4);
    }

    gHist[gi] = 0u;  // 次フレーム用にクリア
}
