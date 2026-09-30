// VgRaster.hlsl ― 仮想ジオメトリ P3: 可視クラスタ（VgClusterCull の Visible キュー）をメッシュシェーダで可視性バッファ + 深度へ描く。
//
//   ・増幅シェーダ（AS。任意）: 32 クラスタ / グループ。追加カリング（球が画素中心を 1 つも覆わない）→ 残りを payload で MS へ。
//   ・メッシュシェーダ（MS）: 1 グループ = 1 クラスタ（128 スレッド = 頂点 i と三角形 i）。頂点をデコード（VgeoDecode.hlsli）して
//       インスタンス行列 → ジッタ付き VP。三角形ごとの小三角形カリング（SV_CullPrimitive。フラグで有効）。
//       プリミティブ属性 vis = (可視スロット << 7) | クラスタ内三角形番号（nointerpolation）。
//   ・ピクセルシェーダ（PS）: R32_UINT へ vis を書くだけ。深度は HW が書く（LESS）。カリングは NONE（フォワードと同じ両面）。
//       -DVG_PS_COUNT 版は [earlydepthstencil] + 断片数 / オーバードロー画像の計測（既定は使わない）。
//
// ★専用ルートシグネチャ（VgCommon.hlsli と同じ）。ルート定数 gPass0 = {ラスタのフラグ(VG_RF_*), phase, 0, 0}。
//   範囲 [start, start + count) は VgRasterArgs（VgUtil.hlsl）が作業バッファのカウンタ VG_CNT_RASTER_START / COUNT へ書いている。
// ★コンパイル: MS = ms_6_5（-DVG_USE_AS 版は payload を受ける）/ AS = as_6_5 / PS = ps_6_6（ResourceDescriptorHeap を使う）。
#include "VgCommon.hlsli"
#include "VgeoDecode.hlsli"

struct VgPayload { uint slot[VG_AS_THREADS]; };

struct VOut { float4 pos : SV_Position; };
struct POut
{
    nointerpolation uint vis : VIS;
    bool cull : SV_CullPrimitive;
};

// 可視スロット → {instance, clusterRef} → プールの参照
struct VgClusterCtx
{
    VgInstance inst;
    VgAssetGpu asset;
    uint pageBase;
    uint clusterIdx;
    uint chunk;          // poolSrvBase からの相対
};

VgClusterCtx VgFetchCtx(RWByteAddressBuffer work, uint slot)
{
    const uint2 e = work.Load2(gOffs1.y + slot * 8u);
    StructuredBuffer<VgInstance> instances = ResourceDescriptorHeap[gHeap0.y];
    StructuredBuffer<VgAssetGpu> assets     = ResourceDescriptorHeap[gHeap0.x];
    VgClusterCtx c;
    c.inst = instances[e.x];
    c.asset = assets[c.inst.assetIndex];
    const uint page = e.y & 0xFFFFu;
    c.clusterIdx = (e.y >> 16) & 0xFFu;
    c.pageBase = (page & ((1u << VG_CHUNK_PAGES_LOG2) - 1u)) * VG_PAGE_BYTES;
    c.chunk = page >> VG_CHUNK_PAGES_LOG2;
    return c;
}

// 画素中心（i + 0.5）を 1 つも含まない範囲か。HW の固定小数点（サブピクセル 8 ビット）の丸めぶん 1/128 px 広げて保守的に。
bool VgNoPixelCenter(float2 mn, float2 mx)
{
    const float eps = 1.0 / 128.0;
    const float2 a = ceil(mn - eps - 0.5);
    const float2 b = floor(mx + eps - 0.5);
    return a.x > b.x || a.y > b.y;
}

float2 VgToPixel(float4 clip)
{
    const float2 ndc = clip.xy / clip.w;
    return float2(gViewport.x + (ndc.x * 0.5 + 0.5) * gViewport.z,
                  gViewport.y + (1.0 - (ndc.y * 0.5 + 0.5)) * gViewport.w);
}

// 世界空間の球 (c, r) が画素中心を 1 つも覆わないか。球の外接立方体の 8 隅を投影した画面矩形（球を必ず含む）で調べる。
//   ★球の投影半径を r * f / (w - r) で見積もる近似は使わない（画面の端では楕円に伸びて過小になり、穴を開ける）。
//   1 つでも近平面の後ろ（w <= 0）にある隅があれば判定不能 = 落とさない。
bool VgSphereNoPixelCenter(float3 c, float r)
{
    float2 mn = float2(1e30, 1e30), mx = float2(-1e30, -1e30);
    [unroll]
    for (int k = 0; k < 8; ++k)
    {
        const float3 p = c + r * float3((k & 1) ? 1.0 : -1.0, (k & 2) ? 1.0 : -1.0, (k & 4) ? 1.0 : -1.0);
        const float4 clip = mul(float4(p, 1.0), gViewProj);
        if (!(clip.w > 1e-4)) return false;
        const float2 q = VgToPixel(clip);
        mn = min(mn, q);
        mx = max(mx, q);
    }
    return VgNoPixelCenter(mn, mx);
}

// ── 増幅シェーダ ─────────────────────────────────────────────────────────────
#ifdef VG_AS
groupshared VgPayload sPayload;
groupshared uint sCount;

[numthreads(VG_AS_THREADS, 1, 1)]
void ASMain(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID)
{
    RWByteAddressBuffer work = VgWork();
    if (gi == 0) sCount = 0;
    GroupMemoryBarrierWithGroupSync();

    const uint start = work.Load(gOffs1.z + VG_CNT_RASTER_START * 4u);
    const uint count = work.Load(gOffs1.z + VG_CNT_RASTER_COUNT * 4u);
    const uint idx = (gid.y * VG_GRID_X + gid.x) * VG_AS_THREADS + gi;
    uint culled = 0;
    if (idx < count)
    {
        const uint slot = start + idx;
        const VgClusterCtx c = VgFetchCtx(work, slot);
        ByteAddressBuffer pool = ResourceDescriptorHeap[NonUniformResourceIndex(c.asset.poolSrvBase + c.chunk)];
        const uint hb = c.pageBase + 64u + c.clusterIdx * 128u;
        const float4 cs = asfloat(pool.Load4(hb + 32));       // cullSphere
        bool keep = true;
        const float3 cc = VgXform(c.inst.w0, c.inst.w1, c.inst.w2, cs.xyz);
        const float r = cs.w * c.inst.maxScale;
        // 球が画素中心を 1 つも覆わない（画面上 1 px 未満）クラスタは描かない（近クリップをまたぐ物は判定しない）
        if ((gRast.x & VG_RF_SMALLPRIM_CULL) != 0u && VgSphereNoPixelCenter(cc, r)) keep = false;
        if (keep)
        {
            uint o;
            InterlockedAdd(sCount, 1u, o);
            sPayload.slot[o] = slot;
        }
        else culled = 1;
    }
    if ((gRast.x & VG_RF_COUNT) != 0u) VgStatAdd(work, VG_STAT_AS_CULLED, culled);
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(sCount, 1, 1, sPayload);
}
#endif

// ── メッシュシェーダ ─────────────────────────────────────────────────────────
groupshared float4 sClip[VG_MS_THREADS];

[outputtopology("triangle")]
[numthreads(VG_MS_THREADS, 1, 1)]
void MSMain(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID,
#ifdef VG_USE_AS
            in payload VgPayload pl,
#endif
            out vertices VOut verts[VG_MS_THREADS],
            out indices uint3 tris[VG_MS_THREADS],
            out primitives POut prims[VG_MS_THREADS])
{
    RWByteAddressBuffer work = VgWork();
#ifdef VG_USE_AS
    const uint slot = pl.slot[gid.x];
    const bool valid = true;
#else
    // グループ数は 2 次元へ畳んで切り上げているので、範囲外のグループは何も出さない（グループ全体で一様）
    const uint lin = gid.y * VG_GRID_X + gid.x;
    const bool valid = lin < work.Load(gOffs1.z + VG_CNT_RASTER_COUNT * 4u);
    const uint slot = work.Load(gOffs1.z + VG_CNT_RASTER_START * 4u) + lin;
#endif
    // ★SetMeshOutputCounts は 1 回だけ（複数の呼び出し箇所があると検証エラー）。valid でないグループは (0, 0)。
    uint vc = 0, tc = 0;
    if (valid)
    {
        const VgClusterCtx c = VgFetchCtx(work, slot);
        ByteAddressBuffer pool = ResourceDescriptorHeap[NonUniformResourceIndex(c.asset.poolSrvBase + c.chunk)];
        const VgClusterRaster cl = VgLoadClusterRaster(pool, c.pageBase, c.clusterIdx);
        vc = cl.vertexCount;
        tc = cl.triangleCount;
    }
    SetMeshOutputCounts(vc, tc);

    if (gi < vc)      // vc = 0（範囲外グループ）なら誰も入らない
    {
        const VgClusterCtx c = VgFetchCtx(work, slot);
        ByteAddressBuffer pool = ResourceDescriptorHeap[NonUniformResourceIndex(c.asset.poolSrvBase + c.chunk)];
        const VgClusterRaster cl = VgLoadClusterRaster(pool, c.pageBase, c.clusterIdx);
        const float3 p = VgDecodePos(pool, c.pageBase, cl, gi, c.asset.posOrigin, c.asset.posStep);
        const float3 w = VgXform(c.inst.w0, c.inst.w1, c.inst.w2, p);
        const float4 clip = mul(float4(w, 1.0), gViewProj);
        verts[gi].pos = clip;
        sClip[gi] = clip;
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi < tc)
    {
        const VgClusterCtx c = VgFetchCtx(work, slot);
        ByteAddressBuffer pool = ResourceDescriptorHeap[NonUniformResourceIndex(c.asset.poolSrvBase + c.chunk)];
        const VgClusterRaster cl = VgLoadClusterRaster(pool, c.pageBase, c.clusterIdx);
        const uint3 t = VgReadTri(pool, c.pageBase, cl, gi);
        tris[gi] = t;
        prims[gi].vis = (slot << VG_VIS_TRI_BITS) | gi;
        bool cull = false;
        if ((gRast.x & VG_RF_SMALLPRIM_CULL) != 0u)
        {
            const float4 a = sClip[t.x], b = sClip[t.y], d = sClip[t.z];
            if (a.w > 1e-4 && b.w > 1e-4 && d.w > 1e-4)          // 全頂点が前方のときだけ（近クリップをまたぐ物は HW に任せる）
            {
                const float2 pa = VgToPixel(a), pb = VgToPixel(b), pd = VgToPixel(d);
                cull = VgNoPixelCenter(min(pa, min(pb, pd)), max(pa, max(pb, pd)));
            }
        }
        prims[gi].cull = cull;
        if ((gRast.x & VG_RF_COUNT) != 0u)
        {
            VgStatAdd(work, VG_STAT_MS_TRIS_OUT, 1u);
            VgStatAdd(work, VG_STAT_MS_PRIM_CULLED, cull ? 1u : 0u);
        }
    }
}

// ── ピクセルシェーダ ─────────────────────────────────────────────────────────
struct PIn
{
    float4 pos : SV_Position;
    nointerpolation uint vis : VIS;
};

#ifdef VG_PS_COUNT
[earlydepthstencil]
#endif
uint PSMain(PIn i) : SV_Target0
{
#ifdef VG_PS_COUNT
    if (!IsHelperLane())
    {
        RWByteAddressBuffer work = VgWork();
        VgStatAdd(work, VG_STAT_PS_INVOC, 1u);
        if (gHeap2.z != VG_NONE)
        {
            RWTexture2D<uint> od = ResourceDescriptorHeap[gHeap2.z];
            uint o;
            InterlockedAdd(od[uint2(i.pos.xy)], 1u, o);
        }
    }
#endif
    return i.vis;
}
