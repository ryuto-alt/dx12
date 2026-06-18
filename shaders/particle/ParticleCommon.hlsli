// ParticleCommon.hlsli
//   コード駆動プロシージャル VFX 用の共通プリミティブ。
//   テクスチャを一切使わず、ハッシュ→勾配ノイズ→fbm→ドメインワープ→
//   コサインパレット→2D SDF を全部数式で生成する（iq / Bridson 由来）。
//   Particle.hlsl から #include して各 kind の見た目を組み立てる。
#ifndef PARTICLE_COMMON_HLSLI
#define PARTICLE_COMMON_HLSLI

static const float PI    = 3.14159265359;
static const float TAU   = 6.28318530718;

// ---- 整数ハッシュ（PCG 派生）。sin ハッシュの GPU 依存チラつきを回避 ----
uint  pcg(uint v)
{
    v = v * 747796405u + 2891336453u;
    uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}
float u2f(uint h)        { return float(h) * (1.0 / 4294967296.0); }   // [0,1)
float hash11(float x)    { return u2f(pcg(asuint(x))); }
float hash21(float2 p)   { return u2f(pcg(asuint(p.x) ^ pcg(asuint(p.y)))); }

// 格子点の勾配ベクトル（ランダム単位ベクトル）
float2 grad2(int2 i)
{
    uint h = pcg(uint(i.x) ^ pcg(uint(i.y)));
    float a = u2f(h) * TAU;
    return float2(cos(a), sin(a));
}

// 勾配ノイズ 2D（quintic 補間で C1 連続 → fbm/カールが滑らか）。戻り ~[-0.7,0.7]
float gnoise2(float2 p)
{
    float2 ip = floor(p);
    float2 fp = p - ip;
    float2 u  = fp * fp * fp * (fp * (fp * 6.0 - 15.0) + 10.0);
    int2 i = int2(ip);
    float n00 = dot(grad2(i + int2(0, 0)), fp - float2(0, 0));
    float n10 = dot(grad2(i + int2(1, 0)), fp - float2(1, 0));
    float n01 = dot(grad2(i + int2(0, 1)), fp - float2(0, 1));
    float n11 = dot(grad2(i + int2(1, 1)), fp - float2(1, 1));
    float nx0 = lerp(n00, n10, u.x);
    float nx1 = lerp(n01, n11, u.x);
    return lerp(nx0, nx1, u.y);
}

// [0,1] に正規化したノイズ
float gnoise2_01(float2 p) { return saturate(0.5 + 0.75 * gnoise2(p)); }

// fbm（octave 加算）。oct=2..5。戻り ~[-1,1]
float fbm2(float2 p, int oct)
{
    float a = 0.5, s = 0.0;
    // 各 octave を回転させて格子の整列を消す（iq）
    float2x2 m = float2x2(1.6, 1.2, -1.2, 1.6);
    [loop] for (int i = 0; i < oct; ++i)
    {
        s += a * gnoise2(p);
        p  = mul(m, p);
        a *= 0.5;
    }
    return s;
}
float fbm2_01(float2 p, int oct) { return saturate(0.5 + 0.6 * fbm2(p, oct)); }

// ドメインワープ（iq）: 模様をうねらせて有機的にする
float fbmWarp(float2 p, int oct)
{
    float2 q = float2(fbm2(p, oct), fbm2(p + float2(5.2, 1.3), oct));
    return fbm2(p + 4.0 * q, oct);
}

// コサインパレット（iq）: テクスチャ無しのグラデーション ramp
float3 palette(float t, float3 a, float3 b, float3 c, float3 d)
{
    return a + b * cos(TAU * (c * t + d));
}

// 2D 線分 SDF（iq）: 稲妻/筋に使う
float sdSegment(float2 p, float2 a, float2 b)
{
    float2 pa = p - a, ba = b - a;
    float h = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h);
}

// ソフトスター（N 枚の花弁）: 0=外, 1=芯
float starMask(float2 uv, float points, float sharp)
{
    float r = length(uv);
    float a = atan2(uv.y, uv.x);
    float petals = pow(abs(cos(a * points * 0.5)), sharp);
    return saturate(1.0 - r / max(0.15 + 0.85 * petals, 1e-3));
}

#endif // PARTICLE_COMMON_HLSLI
