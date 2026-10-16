// CloudNoise.hlsl — ボリュメトリッククラウド用ノイズの起動時生成(compute。自前実装。外部コードは使っていない)
//   ShapeCS   : 128^3 RGBA8  R = Perlin-Worley(低周波の大きな塊)/ G,B,A = 反転 Worley FBM(周波数 4 / 8 / 16 セル)
//   DetailCS  :  32^3 RGBA8  R,G,B = 反転 Worley FBM(周波数 2 / 4 / 8 セル。輪郭を削る高周波)
//   WeatherCS : 256^2 RGBA8  R = 雲量の大域パターン / G = 雲の型の大域パターン / B = 中周波の濃淡 / A = 1
// どれも周期境界(タイル可能)。手法: Schneider "The Real-time Volumetric Cloudscapes of Horizon Zero Dawn" (2015) の構成。
// ルートシグネチャ・レジスタは CloudMarch.hlsl と共通(CloudRenderer.cpp)。

RWTexture3D<float4> gOutVol     : register(u4);
RWTexture2D<float4> gOutWeather : register(u5);

cbuffer NoiseCB : register(b1)   // CloudCB(CloudMarch.hlsl)と同じ b1。先頭の 16 バイトだけ読む
{
    uint4 gNoiseParam;   // x = 3D 解像度(shape 128 / detail 32) y = 2D 解像度(256) zw = 未使用
}

uint3 Pcg3(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
float3 Hash3(int3 c, uint seed)
{
    return float3(Pcg3(uint3(c) + uint3(seed * 7919u, seed * 104729u, seed * 1299709u))) * (1.0 / 4294967296.0);
}
int3 WrapCell(int3 c, int period)
{
    return int3(((c.x % period) + period) % period, ((c.y % period) + period) % period, ((c.z % period) + period) % period);
}

// 反転 Worley(1 = セルの特徴点、0 = 遠い)。p は [0,1)^3、freq = 1 辺のセル数(周期)
float InvWorley(float3 p, int freq, uint seed)
{
    float3 q = p * freq;
    int3 ci = int3(floor(q));
    float3 f = q - float3(ci);
    float dmin = 1.0e9;
    [unroll] for (int z = -1; z <= 1; ++z)
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int3 o = int3(x, y, z);
        float3 feat = Hash3(WrapCell(ci + o, freq), seed) + float3(o);
        float3 d = feat - f;
        dmin = min(dmin, dot(d, d));
    }
    return saturate(1.0 - sqrt(dmin) * 1.15);   // 1.15 = F1 の典型的な最大値で正規化(0..1 にほぼ収める)
}
float WorleyFbm(float3 p, int f0, uint seed)
{
    return InvWorley(p, f0, seed) * 0.625 + InvWorley(p, f0 * 2, seed + 11u) * 0.25 + InvWorley(p, f0 * 4, seed + 23u) * 0.125;
}

// 周期 period の勾配ノイズ(Perlin)。出力はおよそ [-1,1]
float3 GradDir(int3 c, uint seed)
{
    float3 h = Hash3(c, seed) * 2.0 - 1.0;
    return normalize(h + 1e-4);
}
float Perlin(float3 p, int period, uint seed)
{
    float3 q = p * period;
    int3 ci = int3(floor(q));
    float3 f = q - float3(ci);
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float v[8];
    [unroll] for (int i = 0; i < 8; ++i)
    {
        int3 o = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        v[i] = dot(GradDir(WrapCell(ci + o, period), seed), f - float3(o));
    }
    float x00 = lerp(v[0], v[1], u.x), x10 = lerp(v[2], v[3], u.x);
    float x01 = lerp(v[4], v[5], u.x), x11 = lerp(v[6], v[7], u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z) * 1.6;
}
float PerlinFbm(float3 p, int period, uint seed, int oct)
{
    float a = 0.5, s = 0.0, n = 0.0;
    int per = period;
    for (int i = 0; i < oct; ++i)
    {
        s += Perlin(p, per, seed + uint(i) * 17u) * a;
        n += a; a *= 0.5; per *= 2;
    }
    return s / n;
}
float Remap(float v, float lo, float hi, float nlo, float nhi) { return nlo + (v - lo) / (hi - lo) * (nhi - nlo); }

[numthreads(4, 4, 4)]
void ShapeCS(uint3 id : SV_DispatchThreadID)
{
    uint n = gNoiseParam.x;
    if (any(id >= n)) return;
    float3 p = (float3(id) + 0.5) / float(n);
    float perlin = saturate(PerlinFbm(p, 4, 101u, 4) * 0.5 + 0.5);
    float wfbm = WorleyFbm(p, 4, 7u);
    float pw = saturate(Remap(perlin, 0.0, 1.0, wfbm, 1.0));
    float g = WorleyFbm(p, 4, 31u);
    float b = WorleyFbm(p, 8, 53u);
    float a = InvWorley(p, 16, 71u) * 0.6 + InvWorley(p, 32, 83u) * 0.4;
    gOutVol[id] = float4(pw, g, b, a);
}

[numthreads(4, 4, 4)]
void DetailCS(uint3 id : SV_DispatchThreadID)
{
    uint n = gNoiseParam.x;
    if (any(id >= n)) return;
    float3 p = (float3(id) + 0.5) / float(n);
    float r = WorleyFbm(p, 2, 211u);
    float g = WorleyFbm(p, 4, 223u);
    float b = InvWorley(p, 8, 239u) * 0.625 + InvWorley(p, 16, 241u) * 0.375;
    gOutVol[id] = float4(r, g, b, 1.0);
}

[numthreads(8, 8, 1)]
void WeatherCS(uint3 id : SV_DispatchThreadID)
{
    uint n = gNoiseParam.y;
    if (any(id.xy >= n)) return;
    float3 p = float3((float2(id.xy) + 0.5) / float(n), 0.37);   // z は固定(2D 断面)。周期は xy のみで効く
    float r = PerlinFbm(p, 3, 307u, 5) * 0.5 + 0.5;
    float g = PerlinFbm(p, 2, 331u, 3) * 0.5 + 0.5;
    float b = PerlinFbm(p, 8, 353u, 3) * 0.5 + 0.5;
    // 分布が中央に寄るので、コントラストを足して 0..1 に広げる
    r = saturate((r - 0.5) * 1.9 + 0.5);
    g = saturate((g - 0.5) * 1.9 + 0.5);
    b = saturate((b - 0.5) * 1.9 + 0.5);
    gOutWeather[id.xy] = float4(r, g, b, 1.0);
}
