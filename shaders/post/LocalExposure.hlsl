// ローカル露出（UE5 の Local Exposure 相当）。トーンマップ前・露出後のリニア HDR に対して、
// 画素ごとの「周囲の明るさ(base)」を中間灰へ寄せ、細部(detail = log2(L) - base)は保ったまま足し戻す。
//   base   = lerp(バイラテラルグリッド(1/16, 輝度方向 32 段)を trilinear で引いた値, ぼかした縮小輝度(1/8), blend)
//   middle = log2(0.18) + bias
//   base > middle: (base - middle) * leHighlightContrast / base < middle: (base - middle) * leShadowContrast
//   newLog = middle + 縮めた差 + detail * leDetailStrength → 色に exp2(newLog - log2(L)) を掛ける
//
// 構成（すべて 1 フレームに 1 回・LocalExposurePass.cpp が呼ぶ）:
//   CSGrid      16x16 のグループ = グリッドの 1 柱(x,y)。shared メモリに 32 段ぶんを貯め、最後に直書き（atomic 不要）
//   CSDown      8x8 のグループ = 縮小輝度の 1 テクセル。ブロックの対数輝度の平均
//   CSGridBlur  グリッドを 3x3x3 (1,2,1) でぼかす
//   CSLumBlur   縮小輝度を 9x9 ガウスでぼかす
//   LocalExposurePS  フルスクリーン。シーン RT と同寸の RGBA16F へ「色 * gain」を書く
//
// 露出倍率 expo は uber(PostProcess.hlsl) が掛けるものと同じ値（自動露出バッファ * 定数）。
// gain は色に掛ける倍率なので、uber 側の expo と可換（ブルーム等へも同じ絵が回る）。

#include "FullscreenTri.hlsli"

cbuffer LECB : register(b0)
{
    float4 p0;    // x=定数側の露出倍率, y=露出バッファ(t1)も掛けるなら 1, z=グリッド輝度下限 log2, w=1EV あたりのビン数
    uint4  dim0;  // x=W, y=H, z=gridW, w=gridH
    uint4  dim1;  // x=lumW(1/8), y=lumH, z=_, w=_
    float4 le0;   // x=highlightContrast, y=shadowContrast, z=detailStrength, w=blurredLumBlend
    float4 le1;   // x=middle(log2), y=gain 下限(EV), z=gain 上限(EV), w=_
};

static const uint kBins = 32u;
static const float kFixed = 64.0;   // グリッドの和は 1/64 EV の固定小数

Texture2D<float4>         gScene    : register(t0);
StructuredBuffer<float>   gExpo     : register(t1);
RWStructuredBuffer<uint2> gGridRaw  : register(u0);   // [z][y][x] = (和, 個数)
RWStructuredBuffer<float2> gGridBlur: register(u1);   // ぼかし後 (和, 個数)
RWStructuredBuffer<float> gLumA     : register(u2);   // 1/8 対数輝度
RWStructuredBuffer<float> gLumB     : register(u3);   // ぼかし後
// 描画パス用（グラフィクス側のルートシグネチャでは t2 / t3 に root SRV）
StructuredBuffer<float2>  gGridBlurSrv : register(t2);
StructuredBuffer<float>   gLumBSrv     : register(t3);

static float Expo() { return ((p0.y > 0.5) ? gExpo[0] : 1.0) * p0.x; }

static float LogLum(float3 c, float expo)
{
    if (!all(isfinite(c))) c = float3(0, 0, 0);
    c = min(max(c, 0.0), 60000.0);
    float L = dot(c, float3(0.2127, 0.7152, 0.0722)) * expo;
    return clamp(log2(max(L, 1e-6)), -20.0, 20.0);
}

static float GridRange() { return float(kBins) / p0.w; }   // グリッドが覆う EV 幅

// ── ① グリッドへ splat: 1 グループ = 1 柱 ──
groupshared uint gsSum[kBins];
groupshared uint gsCnt[kBins];

[numthreads(16, 16, 1)]
void CSGrid(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex, uint3 id : SV_DispatchThreadID)
{
    if (gi < kBins) { gsSum[gi] = 0u; gsCnt[gi] = 0u; }
    GroupMemoryBarrierWithGroupSync();

    if (id.x < dim0.x && id.y < dim0.y)
    {
        float l   = LogLum(gScene.Load(int3(id.xy, 0)).rgb, Expo());
        float rel = clamp(l - p0.z, 0.0, GridRange());
        uint  bin = min((uint)(rel * p0.w), kBins - 1u);
        InterlockedAdd(gsSum[bin], (uint)(rel * kFixed + 0.5));
        InterlockedAdd(gsCnt[bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi < kBins)
        gGridRaw[(gi * dim0.w + gid.y) * dim0.z + gid.x] = uint2(gsSum[gi], gsCnt[gi]);
}

// ── ② 1/8 縮小の対数輝度: 1 グループ = 1 テクセル（8x8 ブロックの平均）──
groupshared float gsL[64];
groupshared float gsN[64];

[numthreads(8, 8, 1)]
void CSDown(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex, uint3 gtid : SV_GroupThreadID)
{
    uint2 p = gid.xy * 8u + gtid.xy;
    bool ok = (p.x < dim0.x) && (p.y < dim0.y);
    gsL[gi] = ok ? LogLum(gScene.Load(int3(p, 0)).rgb, Expo()) : 0.0;
    gsN[gi] = ok ? 1.0 : 0.0;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint s = 32u; s > 0u; s >>= 1u)
    {
        if (gi < s) { gsL[gi] += gsL[gi + s]; gsN[gi] += gsN[gi + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0u && gid.x < dim1.x && gid.y < dim1.y)
        gLumA[gid.y * dim1.x + gid.x] = gsL[0] / max(gsN[0], 1.0);
}

// ── ③ グリッドの 3x3x3 ぼかし（重み (1,2,1)^3）──
[numthreads(8, 8, 1)]
void CSGridBlur(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= dim0.z || id.y >= dim0.w || id.z >= kBins) return;
    float2 acc = 0.0;
    [unroll] for (int dz = -1; dz <= 1; ++dz)
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        int3 q = int3(id) + int3(dx, dy, dz);
        q.x = clamp(q.x, 0, (int)dim0.z - 1);
        q.y = clamp(q.y, 0, (int)dim0.w - 1);
        q.z = clamp(q.z, 0, (int)kBins - 1);
        float w = (2.0 - abs((float)dx)) * (2.0 - abs((float)dy)) * (2.0 - abs((float)dz));
        acc += float2(gGridRaw[(q.z * dim0.w + q.y) * dim0.z + q.x]) * w;
    }
    gGridBlur[(id.z * dim0.w + id.y) * dim0.z + id.x] = acc;
}

// ── ④ 縮小輝度の 9x9 ガウス（σ=2 テクセル ≒ 画面の 16px 級）──
[numthreads(8, 8, 1)]
void CSLumBlur(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= dim1.x || id.y >= dim1.y) return;
    float acc = 0.0, wsum = 0.0;
    [loop] for (int dy = -4; dy <= 4; ++dy)
    [loop] for (int dx = -4; dx <= 4; ++dx)
    {
        int2 q = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(dim1.xy) - 1);
        float w = exp(-0.125 * float(dx * dx + dy * dy));   // exp(-d^2 / (2*2^2))
        acc  += gLumA[q.y * dim1.x + q.x] * w;
        wsum += w;
    }
    gLumB[id.y * dim1.x + id.x] = acc / wsum;
}

// ── ⑤ 適用 ──
static float SampleLumBlur(float2 pix)
{
    float2 t  = pix / 8.0 - 0.5;
    int2   i0 = (int2)floor(t);
    float2 f  = t - float2(i0);
    int2   mx = int2(dim1.xy) - 1;
    int2 a = clamp(i0, int2(0, 0), mx);
    int2 b = clamp(i0 + 1, int2(0, 0), mx);
    float v00 = gLumBSrv[a.y * dim1.x + a.x];
    float v10 = gLumBSrv[a.y * dim1.x + b.x];
    float v01 = gLumBSrv[b.y * dim1.x + a.x];
    float v11 = gLumBSrv[b.y * dim1.x + b.x];
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

// グリッドの trilinear。戻り値 x=base(log2) y=有効重み（ほぼ 0 なら信用しない）
static float2 SampleGrid(float2 pix, float l)
{
    float3 t  = float3(pix / 16.0 - 0.5, (clamp(l, p0.z, p0.z + GridRange()) - p0.z) * p0.w - 0.5);
    int3   i0 = (int3)floor(t);
    float3 f  = t - float3(i0);
    float2 acc = 0.0;
    [unroll] for (int dz = 0; dz <= 1; ++dz)
    [unroll] for (int dy = 0; dy <= 1; ++dy)
    [unroll] for (int dx = 0; dx <= 1; ++dx)
    {
        int3 q = i0 + int3(dx, dy, dz);
        q.x = clamp(q.x, 0, (int)dim0.z - 1);
        q.y = clamp(q.y, 0, (int)dim0.w - 1);
        q.z = clamp(q.z, 0, (int)kBins - 1);
        float w = (dx ? f.x : 1.0 - f.x) * (dy ? f.y : 1.0 - f.y) * (dz ? f.z : 1.0 - f.z);
        acc += gGridBlurSrv[(q.z * dim0.w + q.y) * dim0.z + q.x] * w;
    }
    return float2(acc.x / max(acc.y * kFixed, 1e-6) + p0.z, acc.y);
}

float4 LocalExposurePS(FSQuadVSOut i) : SV_TARGET
{
    int2   px  = int2(i.pos.xy);
    float4 src = gScene.Load(int3(px, 0));
    float  l   = LogLum(src.rgb, Expo());

    float2 pix = float2(px) + 0.5;
    float2 g   = SampleGrid(pix, l);
    float  bl  = SampleLumBlur(pix);
    float  baseG = (g.y > 0.02) ? g.x : bl;          // 柱に 1 画素も居ない所はぼかし輝度で代用
    float  base  = lerp(baseG, bl, saturate(le0.w));

    float middle = le1.x;
    float d      = base - middle;
    float scaled = d * ((d > 0.0) ? le0.x : le0.y);
    float detail = l - base;
    float newLog = middle + scaled + detail * le0.z;
    float gain   = exp2(clamp(newLog - l, le1.y, le1.z));

    return float4(src.rgb * gain, src.a);
}
