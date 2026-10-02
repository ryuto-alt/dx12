// InstanceCull.hlsl - GPU 駆動のインスタンス群（設計 §4.2 = 4-3）: チャンク → インスタンスの 2 段カリング + LOD 選択 + 間接引数。
//
//   CSReset    : list ごとのカウンタを 0 にする
//   CSCount    : 1 グループ = 1 チャンク（連続 128 インスタンス）。チャンクを AABB で棄却 → インスタンスの球を視錐台で判定 → LOD（= list）を決める。
//                list ごとの個数をチャンク単位で書く
//   CSScan     : 1 グループ = 1 list。チャンクごとの個数を排他的プレフィックス和にして、合計を g_counters へ
//   CSFinalize : カウンタ → ExecuteIndirect の引数（DrawIndexedInstanced: サブメッシュ × list）
//   CSEmit     : 同じ判定をもう一度して、(list の先頭 + チャンクの先頭 + チャンク内の順位) へ MeshInstanceData（64B）と前フレーム（48B）を書く
//
// ★出力の並びは「チャンク順 → チャンク内の番号順」＝インスタンス番号の昇順で、スレッドの実行順に依存しない（原子カウンタで Append しない）。
//   CPU 経路は (lod, guid, instanceIndex) の昇順に描くので、ON/OFF で深度が同点の断片の勝者（描画順）も一致する。
// ★式は src/renderer/instancegpu/InstanceGpuMath.h の SelectList / ChunkInPlanes と一対一（tests/instance_gpu_test.cpp が見張る）。
// ★専用ルートシグネチャ（メインの 62/64 DWORD には触れない）。バッファは全部ルート記述子（ヒープを張り替えない）。

cbuffer CullCB : register(b0)
{
    float4 gPlanes[6];   // ワールド平面（a,b,c,d）。内側 dot(n,p)+d >= 0
    float4 gCam;         // xyz = LOD 基準のカメラ位置
    float4 gColor;       // 全インスタンス共通の色（MeshInstanceData.color）
    uint4  gP0;          // x = instanceCount / y = chunkCount / z = lodBias / w = maxList
    float4 gP1;          // x = texelWorld（0 = 無効）/ y = lodScale / zw = 0
    uint4  gP2;          // x = writePrev / y = submeshCount / z = prevStride（48 or 64）/ w = 0
};

ByteAddressBuffer          g_inst        : register(t0);   // 64B × N（r0,r1,r2,center.xyz,radius）
ByteAddressBuffer          g_chunks      : register(t1);   // 32B × M（mn.xyz,first,mx.xyz,count）
ByteAddressBuffer          g_prev        : register(t2);   // 前フレーム（stride = gP2.z。48B 専用バッファ or inst 自身 = 64B）
StructuredBuffer<uint>     g_indexCounts : register(t3);   // [submesh * 5 + list] のインデックス数
RWByteAddressBuffer        g_out         : register(u0);   // 可視インスタンス（64B × N）
RWByteAddressBuffer        g_outPrev     : register(u1);   // 可視インスタンスの前フレーム（48B × N）
RWStructuredBuffer<uint>   g_chunkCounts : register(u2);   // [list * chunkCount + chunk]
RWStructuredBuffer<uint>   g_counters    : register(u3);   // [list] = 個数
RWStructuredBuffer<uint>   g_args        : register(u4);   // DrawIndexedInstanced 引数（5 uint × submesh × list）

static const uint kLists = 5u;
static const uint kInvalid = 0xFFFFFFFFu;

[numthreads(8, 1, 1)]
void CSReset(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x < kLists) g_counters[dtid.x] = 0u;
}

bool ChunkVisible(float3 mn, float3 mx)
{
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        const float3 n = gPlanes[i].xyz;
        const float3 pv = float3(n.x >= 0.0 ? mx.x : mn.x, n.y >= 0.0 ? mx.y : mn.y, n.z >= 0.0 ? mx.z : mn.z);
        if (dot(n, pv) + gPlanes[i].w < 0.0) return false;
    }
    return true;
}

// 出力先 list（LOD）。kInvalid = 描かない。
uint SelectList(float3 c, float r)
{
    [unroll]
    for (int i = 0; i < 6; ++i)
        if (dot(gPlanes[i].xyz, c) + gPlanes[i].w < -r) return kInvalid;
    const float dist = length(c - gCam.xyz);
    const float apparent = r / max(dist, 1e-3) * gP1.y;
    uint lod = (apparent < 0.008) ? 4u : (apparent < 0.020) ? 3u : (apparent < 0.055) ? 2u : (apparent < 0.125) ? 1u : 0u;
    lod += gP0.z;
    if (gP1.x > 0.0)
    {
        const float texels = 2.0 * r / gP1.x;
        const uint tl = (texels < 32.0) ? 4u : (texels < 96.0) ? 3u : (texels < 288.0) ? 2u : (texels < 864.0) ? 1u : 0u;
        lod = max(lod, tl);
    }
    return min(lod, gP0.w);
}

groupshared uint gsMask[kLists * 4];   // [list * 4 + gi / 32] のビット

uint MaskCount(uint list)
{
    return countbits(gsMask[list * 4u]) + countbits(gsMask[list * 4u + 1u]) + countbits(gsMask[list * 4u + 2u]) + countbits(gsMask[list * 4u + 3u]);
}

uint MaskRank(uint list, uint gi)
{
    const uint w = gi >> 5;
    uint r = countbits(gsMask[list * 4u + w] & ((1u << (gi & 31u)) - 1u));
    if (w > 0u) r += countbits(gsMask[list * 4u]);
    if (w > 1u) r += countbits(gsMask[list * 4u + 1u]);
    if (w > 2u) r += countbits(gsMask[list * 4u + 2u]);
    return r;
}

// チャンクの 1 インスタンスを評価する（Count / Emit 共通）。戻り値 = list or kInvalid。first / count はチャンク表から。
uint EvalChunkInstance(uint first, uint count, uint gi, out float4 rowC)
{
    rowC = float4(0, 0, 0, 0);
    if (gi >= count) return kInvalid;
    const uint4 w3 = g_inst.Load4((first + gi) * 64u + 48u);
    rowC = asfloat(w3);   // xyz = center, w = radius
    return SelectList(rowC.xyz, rowC.w);
}

void LoadChunk(uint chunk, out float3 mn, out float3 mx, out uint first, out uint count)
{
    const uint4 a = g_chunks.Load4(chunk * 32u);
    const uint4 b = g_chunks.Load4(chunk * 32u + 16u);
    mn = asfloat(a.xyz); first = a.w;
    mx = asfloat(b.xyz); count = b.w;
}

[numthreads(128, 1, 1)]
void CSCount(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint chunk = gid.y * 65535u + gid.x;
    if (chunk >= gP0.y) return;
    if (gi < kLists * 4u) gsMask[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();
    float3 mn, mx; uint first, count;
    LoadChunk(chunk, mn, mx, first, count);
    if (ChunkVisible(mn, mx))
    {
        float4 c;
        const uint list = EvalChunkInstance(first, count, gi, c);
        if (list != kInvalid)
        {
            uint dummy;
            InterlockedOr(gsMask[list * 4u + (gi >> 5)], 1u << (gi & 31u), dummy);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < kLists) g_chunkCounts[gi * gP0.y + chunk] = MaskCount(gi);
}

groupshared uint gsPart[256];

[numthreads(256, 1, 1)]
void CSScan(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint list = gid.x;
    if (list >= kLists) return;
    const uint n = gP0.y;
    const uint baseIdx = list * n;
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
    if (gi == 255u) g_counters[list] = gsPart[255];
}

// サブメッシュ × list の引数。StartInstanceLocation = list の先頭（出力は 1 本のストリームに list を連結してある）。
[numthreads(64, 1, 1)]
void CSFinalize(uint3 dtid : SV_DispatchThreadID)
{
    const uint S = gP2.y;
    const uint n = S * kLists;
    if (dtid.x >= n) return;
    const uint list = dtid.x % kLists;
    uint base = 0u;
    for (uint k = 0u; k < list; ++k) base += g_counters[k];
    const uint o = dtid.x * 5u;
    g_args[o + 0u] = g_indexCounts[dtid.x];
    g_args[o + 1u] = g_counters[list];
    g_args[o + 2u] = 0u;
    g_args[o + 3u] = 0u;
    g_args[o + 4u] = base;
}

[numthreads(128, 1, 1)]
void CSEmit(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint chunk = gid.y * 65535u + gid.x;
    if (chunk >= gP0.y) return;
    if (gi < kLists * 4u) gsMask[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();
    float3 mn, mx; uint first, count;
    LoadChunk(chunk, mn, mx, first, count);
    uint list = kInvalid;
    if (ChunkVisible(mn, mx))
    {
        float4 c;
        list = EvalChunkInstance(first, count, gi, c);
        if (list != kInvalid)
        {
            uint dummy;
            InterlockedOr(gsMask[list * 4u + (gi >> 5)], 1u << (gi & 31u), dummy);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (list == kInvalid) return;
    uint base = 0u;
    for (uint k = 0u; k < list; ++k) base += g_counters[k];
    const uint idx = base + g_chunkCounts[list * gP0.y + chunk] + MaskRank(list, gi);
    const uint src = first + gi;
    const uint4 r0 = g_inst.Load4(src * 64u);
    const uint4 r1 = g_inst.Load4(src * 64u + 16u);
    const uint4 r2 = g_inst.Load4(src * 64u + 32u);
    g_out.Store4(idx * 64u,        r0);
    g_out.Store4(idx * 64u + 16u,  r1);
    g_out.Store4(idx * 64u + 32u,  r2);
    g_out.Store4(idx * 64u + 48u,  asuint(gColor));
    if (gP2.x != 0u)
    {
        const uint ps = gP2.z;
        g_outPrev.Store4(idx * 48u,        g_prev.Load4(src * ps));
        g_outPrev.Store4(idx * 48u + 16u,  g_prev.Load4(src * ps + 16u));
        g_outPrev.Store4(idx * 48u + 32u,  g_prev.Load4(src * ps + 32u));
    }
}
