// FoliageCull.hlsl - 植生 F1: GPU カリング + LOD 選択 + 間引き + クロスフェード（compute）。
//
//   CSReset    : カウンタを 0 にする（レイヤーごと・フレームごと 1 回）
//   CSCount / CSScan / CSEmit : 1 グループ = 1 チャンク（≤128 個）。チャンクを AABB で棄却してから個々のインスタンスを見る。
//                距離 / 視錐台（球）/ 前フレーム HZB / 間引き → LOD ごとの compact ストリーム（64B/個）へ「決定論的な順序で」書く（下の説明）
//   CSFinalize : カウンタ → ExecuteIndirect の引数（DrawIndexedInstanced の InstanceCount）
//
// ★式は src/renderer/foliage/FoliageMath.h の EvaluateInstance / SphereInPlanes / SphereOccludedByHiZ と一対一。
//   片方だけ直すと GPU と CPU がずれる（tests/foliage_gpu_test.cpp が見張る）。迷ったら「見える」に倒す。
// ★専用ルートシグネチャ（メインの 62/64 DWORD には触れない）。バッファはルート記述子、HZB だけテーブル。
#include "FoliageCommon.hlsli"

cbuffer CullCB : register(b0)
{
    float4 gPlanes[6];     // ワールド平面（a,b,c,d）。内側 dot(n,p)+d >= 0
    float4 gCamDist;       // xyz = カメラ位置 / w = cullDist
    float4 gLodDist;       // xyz = LOD 切替距離 / w = thinStart
    float4 gParamsA;       // x = lodFade / y = boundCenterY / z = boundRadius / w = radiusScale
    float4 gParamsB;       // x = layerScale / y = chunkMargin / z = rectInflatePx / w = 未使用
    uint4  gFlags;         // x = variantCount / y = shadowMaxLod / z = flags(bit0=影, bit1=HZB) / w = lodStride
    uint4  gLodCount;      // variant ごとの LOD 数
    float4 gLayer0;        // レイヤーの 3x4（行）
    float4 gLayer1;
    float4 gLayer2;
    uint4  gCounts;        // x = chunkCount / y = listBase（インスタンス単位）/ z = cap / w = counterBase
    uint4  gFin;           // x = G（描画グループ数）/ y = S（ビューあたりの list 数）/ z = numViews / w = 0
    uint4  gCaps;          // x = capMain / y = capShadow / zw = 0
    float4x4 gPrevVP;      // 前フレームのジッタ付き VP（HiZ シェーダと同じ行ベクトル規約）
    float4 gHzb;           // xy = mip0 の解像度 / z = ミップ数 / w = 0
};

ByteAddressBuffer          g_src      : register(t0);   // インスタンス表（32B × N）
ByteAddressBuffer          g_chunks   : register(t1);   // チャンク表（32B × M）
Texture2D<float>           g_hzb      : register(t2);   // 前フレームの HZB（全ミップ）
StructuredBuffer<uint>     g_groupSlot: register(t3);   // 描画グループ → list 番号（variant * lodStride + lod）
RWByteAddressBuffer        g_visible  : register(u0);   // compact ストリーム（64B × cap × list）
RWStructuredBuffer<uint>   g_counters : register(u1);   // list ごとの個数（オーバーフローしても数え続ける）
RWStructuredBuffer<uint>   g_args     : register(u2);   // DrawIndexedInstanced 引数（5 uint × 描画グループ × ビュー）

static const uint kFlagShadow = 1u;
static const uint kFlagHzb    = 2u;

[numthreads(64, 1, 1)]
void CSReset(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x < gFin.y * gFin.z) g_counters[dtid.x] = 0u;
}

// ---- HZB（HiZMath.h / HiZCull.hlsl と同一）----
struct FSBounds { float4 rect; float minZ; bool valid; };

FSBounds FProjectAabb(float3 wmin, float3 wmax)
{
    FSBounds o;
    o.rect = float4(0, 0, 0, 0); o.minZ = 0; o.valid = false;
    const float kMinW = 1e-4;
    if (!(wmin.x <= wmax.x) || !(wmin.y <= wmax.y) || !(wmin.z <= wmax.z)) return o;
    const float2 vpSize = gHzb.xy;
    float2 ndcMin = float2(1e30, 1e30);
    float2 ndcMax = float2(-1e30, -1e30);
    float ndcMinZ = 1e30;
    [unroll]
    for (int c = 0; c < 8; ++c)
    {
        const float3 p = float3((c & 1) ? wmax.x : wmin.x, (c & 2) ? wmax.y : wmin.y, (c & 4) ? wmax.z : wmin.z);
        const float4 clip = mul(float4(p, 1.0), gPrevVP);
        if (!(clip.w > kMinW)) return o;
        const float3 ndc = clip.xyz / clip.w;
        if (!(ndc.x == ndc.x) || !(ndc.y == ndc.y) || !(ndc.z == ndc.z)) return o;
        if (ndc.z < 0.0) return o;
        ndcMin = min(ndcMin, ndc.xy);
        ndcMax = max(ndcMax, ndc.xy);
        ndcMinZ = min(ndcMinZ, ndc.z);
    }
    if (ndcMinZ > 1.0) return o;
    const float sx0 = (ndcMin.x * 0.5 + 0.5) * vpSize.x;
    const float sx1 = (ndcMax.x * 0.5 + 0.5) * vpSize.x;
    const float sy0 = (1.0 - (ndcMax.y * 0.5 + 0.5)) * vpSize.y;
    const float sy1 = (1.0 - (ndcMin.y * 0.5 + 0.5)) * vpSize.y;
    if (sx1 < 0 || sx0 > vpSize.x || sy1 < 0 || sy0 > vpSize.y) return o;
    o.rect = float4(max(sx0, 0.0), max(sy0, 0.0), min(sx1, vpSize.x), min(sy1, vpSize.y));
    o.minZ = ndcMinZ;
    o.valid = true;
    return o;
}

uint FSelectMip(float4 rect, uint mipCount)
{
    if (mipCount == 0) return 0;
    const float w = rect.z - rect.x;
    const float h = rect.w - rect.y;
    float maxSide = max(w, h);
    if (!(maxSide > 0.0)) maxSide = 1.0;
    int mip = 0;
    {
        float texel = 2.0;
        [loop] while (texel < maxSide && mip + 1 < (int)mipCount) { texel *= 2.0; ++mip; }
    }
    [loop] for (; mip + 1 < (int)mipCount; ++mip)
    {
        const float inv = 1.0 / (float)(1u << mip);
        const int x0 = (int)(rect.x * inv), x1 = (int)(rect.z * inv);
        const int y0 = (int)(rect.y * inv), y1 = (int)(rect.w * inv);
        if ((x1 - x0) <= 1 && (y1 - y0) <= 1) break;
    }
    return (uint)mip;
}

bool FSphereOccluded(float3 c, float r)
{
    FSBounds sb = FProjectAabb(c - r, c + r);
    if (!sb.valid) return false;
    const float infl = gParamsB.z;
    const float2 vpSize = gHzb.xy;
    sb.rect = float4(max(0.0, sb.rect.x - infl), max(0.0, sb.rect.y - infl),
                     min(vpSize.x, sb.rect.z + infl), min(vpSize.y, sb.rect.w + infl));
    const uint mipCount = (uint)gHzb.z;
    const uint mip = FSelectMip(sb.rect, mipCount);
    const float inv = 1.0 / (float)(1u << mip);
    const int2 mipSize = max(int2(1, 1), int2(vpSize * inv));
    const int2 t0 = clamp(int2(sb.rect.xy * inv), int2(0, 0), mipSize - 1);
    const int2 t1 = clamp(int2(sb.rect.zw * inv), int2(0, 0), mipSize - 1);
    float hzbMax = g_hzb.Load(int3(t0.x, t0.y, mip));
    hzbMax = max(hzbMax, g_hzb.Load(int3(t1.x, t0.y, mip)));
    hzbMax = max(hzbMax, g_hzb.Load(int3(t0.x, t1.y, mip)));
    hzbMax = max(hzbMax, g_hzb.Load(int3(t1.x, t1.y, mip)));
    if (!(hzbMax == hzbMax) || !(hzbMax < 1.0)) return false;
    return sb.minZ > hzbMax;
}

// 出力先（1 個のインスタンスは最大 2 つの list へ出る = クロスフェード帯）
struct FEmitSlot { uint slot; float lo; float hi; };

// AABB 頂点の p-vertex テスト（視錐台 vs チャンク。margin = インスタンス球の余裕）
bool FChunkInPlanes(float3 mn, float3 mx, float margin)
{
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        const float3 n = gPlanes[i].xyz;
        const float3 pv = float3(n.x >= 0 ? mx.x : mn.x, n.y >= 0 ? mx.y : mn.y, n.z >= 0 ? mx.z : mn.z);
        if (dot(n, pv) + gPlanes[i].w < -margin) return false;
    }
    return true;
}

// チャンク棄却（グループ内で一様）。true = このチャンクは見る価値がある
bool FChunkVisible(FoliageChunk ch)
{
    // レイヤー Transform をかけた 8 隅の外接 AABB
    float3 wmn = float3(1e30, 1e30, 1e30), wmx = float3(-1e30, -1e30, -1e30);
    [unroll]
    for (int c = 0; c < 8; ++c)
    {
        const float3 p = float3((c & 1) ? ch.mx.x : ch.mn.x, (c & 2) ? ch.mx.y : ch.mn.y, (c & 4) ? ch.mx.z : ch.mn.z);
        const float3 w = float3(dot(gLayer0.xyz, p) + gLayer0.w, dot(gLayer1.xyz, p) + gLayer1.w, dot(gLayer2.xyz, p) + gLayer2.w);
        wmn = min(wmn, w); wmx = max(wmx, w);
    }
    const float margin = gParamsB.y;
    const float3 cam = gCamDist.xyz;
    const float3 q = max(max(wmn - cam, cam - wmx), float3(0, 0, 0));
    if (length(q) - margin >= gCamDist.w) return false;
    return FChunkInPlanes(wmn, wmx, margin);
}

// 1 個のインスタンスを評価して出力先を決める。戻り値 = 出力数（0..2）。式は FoliageMath.h の EvaluateInstance と一対一。
uint FEvaluate(FoliageInstance ins, float4 r0, float4 r1, float4 r2, out FEmitSlot e0, out FEmitSlot e1)
{
    e0.slot = 0u; e0.lo = 0.0; e0.hi = 2.0;
    e1 = e0;
    const float3 wp = float3(r0.w, r1.w, r2.w);
    const float3 dcam = wp - gCamDist.xyz;
    const float dist = length(dcam);
    const float s = FoliageUnpackScale(ins.rotScale);
    const float3 center = wp + float3(r0.y, r1.y, r2.y) * gParamsA.y;
    const float radius = gParamsA.z * abs(s) * gParamsB.x * gParamsA.w;

    const float cullDist = gCamDist.w;
    if (dist >= cullDist) return 0u;
    [unroll]
    for (int i = 0; i < 6; ++i)
        if (dot(gPlanes[i].xyz, center) + gPlanes[i].w < -radius) return 0u;

    const bool shadowPass = (gFlags.z & kFlagShadow) != 0u;
    if (!shadowPass && (gFlags.z & kFlagHzb) != 0u)
    {
        if (FSphereOccluded(center, radius)) return 0u;
    }

    // 間引き
    const uint seed = FoliageSeed(ins.seedType);
    const float thinStart = gLodDist.w;
    const float span = max((1.0 - thinStart) * cullDist, 1e-4);
    const float tThin = saturate((dist - thinStart * cullDist) / span);
    const float keep = (1.0 + kFoliageThinBand) * (1.0 - tThin);
    const float h = FoliageHash01(seed ^ 0xA511E9B3u);
    const float b = saturate((keep - h) / kFoliageThinBand);
    if (b <= 0.0) return 0u;

    uint variant = FoliageTypeOf(ins.seedType);
    const uint variantCount = gFlags.x;
    if (variant >= variantCount) variant = variantCount - 1u;
    const uint lcN = max(1u, min(gLodCount[variant], 4u));
    const uint lodStride = gFlags.w;

    uint nominal = 0u;
    [unroll]
    for (uint k = 0; k < 3u; ++k) if (dist >= gLodDist[k]) nominal = k + 1u;

    if (shadowPass)
    {
        const uint lod = min(nominal, lcN - 1u);
        if (lod > gFlags.y) return 0u;
        e0.slot = variant * lodStride + lod; e0.lo = 2.0; e0.hi = 2.0;
        return 1u;
    }

    const float lodFade = gParamsA.x;
    if (lodFade > 0.0)
    {
        [unroll]
        for (uint k2 = 0; k2 < 3u; ++k2)
        {
            if (k2 + 1u >= lcN) break;
            const float D = gLodDist[k2];
            const float t0 = D * (1.0 - lodFade), t1 = D * (1.0 + lodFade);
            if (dist > t0 && dist < t1)
            {
                const float t = saturate((dist - t0) / (t1 - t0));
                e0.slot = variant * lodStride + k2;      e0.lo = (1.0 - t) * b; e0.hi = 2.0;
                e1.slot = variant * lodStride + k2 + 1u; e1.lo = 0.0;           e1.hi = 1.0 - t * b;
                return 2u;
            }
        }
    }
    e0.slot = variant * lodStride + min(nominal, lcN - 1u); e0.lo = b; e0.hi = 2.0;
    return 1u;
}

// ---- 決定論的な compact（★出力順がスレッドの実行順に依存しないこと）----
//   以前は list ごとの原子カウンタで Append していたため、インスタンスの並び順が実行ごとに変わり、
//   深度が同点の断片（葉カードの交線など）でどちらが勝つかが変わって、同じ絵が 1 ビットずつ揺れた。
//   今は 3 段: CSCount（チャンクごとの個数）→ CSScan（list ごとにチャンクの排他的プレフィックス和）→ CSEmit（同じ判定をもう一度して
//   「チャンクの先頭位置 + グループ内の順位」へ書く）。順位は groupshared のビットマスクの popcount で決めるので実行順に依存しない。
//   結果の並び = (チャンク順, チャンク内の添字順)。CPU 参照（FoliageMath.h / テスト）と完全一致する。
groupshared uint gsMask[16][4];   // [list][gi / 32] のビット（list ≤ kMaxVariants * kMaxLods = 16）

void FClearMasks(uint gi)
{
    if (gi < 64u) gsMask[gi >> 2][gi & 3u] = 0u;
}

void FSetMask(uint slot, uint gi)
{
    uint dummy;
    InterlockedOr(gsMask[slot & 15u][gi >> 5], 1u << (gi & 31u), dummy);
}

uint FMaskCount(uint slot)
{
    return countbits(gsMask[slot & 15u][0]) + countbits(gsMask[slot & 15u][1]) + countbits(gsMask[slot & 15u][2]) + countbits(gsMask[slot & 15u][3]);
}

uint FMaskRank(uint slot, uint gi)
{
    const uint w = gi >> 5;
    uint r = countbits(gsMask[slot & 15u][w] & ((1u << (gi & 31u)) - 1u));
    if (w > 0u) r += countbits(gsMask[slot & 15u][0]);
    if (w > 1u) r += countbits(gsMask[slot & 15u][1]);
    if (w > 2u) r += countbits(gsMask[slot & 15u][2]);
    return r;
}

RWStructuredBuffer<uint> g_chunkCounts : register(u3);   // [(view*S + slot) * chunkCount + chunk]：Count が個数を書き、Scan が排他的プレフィックス和へ置き換える

[numthreads(128, 1, 1)]
void CSCount(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint chunkIdx = gid.y * 65535u + gid.x;
    if (chunkIdx >= gCounts.x) return;
    FClearMasks(gi);
    GroupMemoryBarrierWithGroupSync();
    const FoliageChunk ch = FoliageLoadChunk(g_chunks, chunkIdx);
    if (FChunkVisible(ch) && gi < ch.count)
    {
        const FoliageInstance ins = FoliageLoadInstance(g_src, ch.first + gi);
        float4 r0, r1, r2;
        FoliageCompose(ins, gLayer0, gLayer1, gLayer2, r0, r1, r2);
        FEmitSlot e0, e1;
        const uint n = FEvaluate(ins, r0, r1, r2, e0, e1);
        if (n >= 1u) FSetMask(e0.slot, gi);
        if (n >= 2u) FSetMask(e1.slot, gi);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint S = gFin.y;
    if (gi < S) g_chunkCounts[(gCounts.w + gi) * gCounts.x + chunkIdx] = FMaskCount(gi);
}

// 1 グループ = 1 つの list。チャンクごとの個数を排他的プレフィックス和にして、合計を g_counters へ書く。
groupshared uint gsPart[256];

[numthreads(256, 1, 1)]
void CSScan(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint slot = gid.x;
    if (slot >= gFin.y) return;
    const uint n = gCounts.x;
    const uint baseIdx = (gCounts.w + slot) * n;
    const uint per = (n + 255u) / 256u;
    const uint start = min(gi * per, n);
    const uint end = min(start + per, n);
    uint sum = 0u;
    for (uint i = start; i < end; ++i) sum += g_chunkCounts[baseIdx + i];
    gsPart[gi] = sum;
    GroupMemoryBarrierWithGroupSync();
    // Hillis-Steele 包括スキャン
    for (uint off = 1u; off < 256u; off <<= 1)
    {
        const uint v = (gi >= off) ? gsPart[gi - off] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gsPart[gi] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uint run = gsPart[gi] - sum;
    for (uint j = start; j < end; ++j)
    {
        const uint c = g_chunkCounts[baseIdx + j];
        g_chunkCounts[baseIdx + j] = run;
        run += c;
    }
    if (gi == 255u) g_counters[gCounts.w + slot] = gsPart[255];   // 合計（容量超過でもそのまま = 統計に出る）
}

void FWrite(uint off, float4 r0, float4 r1, float4 r2, uint color, uint seedType, uint params, float lo, float hi)
{
    // x = 色 RGBA8 / y = シード(24bit) | 風の強さ倍率(8bit) / z,w = ディザ（lo, hi）
    g_visible.Store4(off,       asuint(r0));
    g_visible.Store4(off + 16u, asuint(r1));
    g_visible.Store4(off + 32u, asuint(r2));
    const uint seed24 = FoliageSeed(seedType);
    g_visible.Store4(off + 48u, uint4(color, seed24 | ((params & 0xFFu) << 24), asuint(lo), asuint(hi)));
}

[numthreads(128, 1, 1)]
void CSEmit(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint chunkIdx = gid.y * 65535u + gid.x;
    if (chunkIdx >= gCounts.x) return;
    FClearMasks(gi);
    GroupMemoryBarrierWithGroupSync();
    const FoliageChunk ch = FoliageLoadChunk(g_chunks, chunkIdx);
    FoliageInstance ins = (FoliageInstance)0;
    float4 r0 = float4(0, 0, 0, 0), r1 = float4(0, 0, 0, 0), r2 = float4(0, 0, 0, 0);
    FEmitSlot e0, e1;
    e0.slot = 0u; e0.lo = 0.0; e0.hi = 2.0; e1 = e0;
    uint n = 0u;
    if (FChunkVisible(ch) && gi < ch.count)
    {
        ins = FoliageLoadInstance(g_src, ch.first + gi);
        FoliageCompose(ins, gLayer0, gLayer1, gLayer2, r0, r1, r2);
        n = FEvaluate(ins, r0, r1, r2, e0, e1);
        if (n >= 1u) FSetMask(e0.slot, gi);
        if (n >= 2u) FSetMask(e1.slot, gi);
    }
    GroupMemoryBarrierWithGroupSync();
    if (n >= 1u)
    {
        const uint cap = gCounts.z;
        const uint idx0 = g_chunkCounts[(gCounts.w + e0.slot) * gCounts.x + chunkIdx] + FMaskRank(e0.slot, gi);
        if (idx0 < cap) FWrite((gCounts.y + e0.slot * cap + idx0) * 64u, r0, r1, r2, ins.color, ins.seedType, ins.params, e0.lo, e0.hi);
        if (n >= 2u)
        {
            const uint idx1 = g_chunkCounts[(gCounts.w + e1.slot) * gCounts.x + chunkIdx] + FMaskRank(e1.slot, gi);
            if (idx1 < cap) FWrite((gCounts.y + e1.slot * cap + idx1) * 64u, r0, r1, r2, ins.color, ins.seedType, ins.params, e1.lo, e1.hi);
        }
    }
}

// 描画グループ × ビューの InstanceCount を書く（IndexCount 等は初期化時にアップロード済み）。
[numthreads(64, 1, 1)]
void CSFinalize(uint3 dtid : SV_DispatchThreadID)
{
    const uint G = gFin.x, S = gFin.y, V = gFin.z;
    const uint n = G * V;
    if (dtid.x >= n) return;
    const uint v = dtid.x / G;
    const uint g = dtid.x % G;
    const uint slot = g_groupSlot[g];
    const uint cap = (v == 0u) ? gCaps.x : gCaps.y;
    const uint cnt = min(g_counters[v * S + slot], cap);
    g_args[dtid.x * 5u + 1u] = cnt;
}
