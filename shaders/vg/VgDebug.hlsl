// VgDebug.hlsl ― 仮想ジオメトリ P3: 可視性バッファの全画面表示（暫定シェーディング + デバッグ可視化）。
//
//   最終シェーディングは P4（resolve）。ここは「可視性バッファが正しく描けているか」を目で見るための物:
//     VG_DBG_SHADE    … 面法線のランバート（暫定。VG ON のときスカイの上、非 VG のフォワードの下に描く）
//     VG_DBG_CLUSTER  … クラスタごとの色（instance + clusterRef のハッシュ。可視スロットの並びに依存しない）
//     VG_DBG_LOD      … LOD レベル（0 = 青 … 最粗 = 赤）
//     VG_DBG_TRI      … 三角形ごとの色（クラスタ + 三角形番号のハッシュ）
//     VG_DBG_DEPTH    … 深度（線形化した距離を gDbg.x で正規化）
//     VG_DBG_OVERDRAW … 断片数（VG_RF_COUNT のラスタが数えた画像）のヒートマップ
//     VG_DBG_COVERAGE … 被覆: VG = 緑 / 非 VG のジオメトリ（深度 < 1）= 灰 / 空 = 黒
//   ルート定数 gPass0.x = モード。VG でない画素は SHADE のときだけ何も描かず（discard）、他のモードは黒を半透明で被せる。
//   P4（resolve の検証）:
//     VG_DBG_MATERIAL       … 材質（アセット + 材質番号のハッシュ色）
//     VG_DBG_MIP            … 解析 UV 勾配から求めたアルベドのミップ段（青 = 0 … 赤 = 10 以上。テクスチャ無しは灰）
//     VG_DBG_TILE_MATERIALS … 8x8 タイル内の異なる材質の数（resolve の波面の分岐の目安。1 = 緑 … 4+ = 赤）
//     VG_DBG_NORMAL         … 補間した頂点法線（ワールド。0.5 + 0.5n）
//     VG_DBG_RAW_*          … テスト用の生データ（R32G32B32A32_FLOAT の RT へ。ブレンド無しの PSO で描く）
#include "VgCommon.hlsli"
#include "VgeoDecode.hlsli"
#include "VgVisShade.hlsli"

struct VSOut { float4 pos : SV_Position; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

uint VgHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

float3 VgHashColor(uint h)
{
    // 色相を一様に、明度 / 彩度は少し揺らす
    const float hue = (h & 0xFFFFu) / 65535.0;
    const float sat = 0.55 + 0.4 * (((h >> 16) & 0xFFu) / 255.0);
    const float val = 0.65 + 0.35 * (((h >> 24) & 0xFFu) / 255.0);
    const float3 k = saturate(abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return val * lerp(float3(1, 1, 1), k, sat);
}

float3 VgHeat(float t)
{
    // 青 → シアン → 緑 → 黄 → 赤
    t = saturate(t);
    return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)));
}

float4 PSMain(VSOut i) : SV_Target0
{
    const uint mode = gPass0.x;
    const int2 px = int2(i.pos.xy);
    Texture2D<uint> visTex = ResourceDescriptorHeap[gHeap2.y];
    const uint vis = visTex.Load(int3(px, 0));

    if (mode == VG_DBG_DEPTH)
    {
        Texture2D<float> dep = ResourceDescriptorHeap[gHeap2.x];
        const float d = dep.Load(int3(px, 0));
        if (d >= 1.0) return float4(0, 0, 0, 1);
        const float n = gCamPos.w, f = gDbg.z;
        const float lin = n * f / (f - d * (f - n));
        const float g = 1.0 - saturate(lin / max(gDbg.x, 1e-3));
        return float4(g, g, g, 1);
    }
    if (mode == VG_DBG_COVERAGE)
    {
        Texture2D<float> dep = ResourceDescriptorHeap[gHeap2.x];
        const float d = dep.Load(int3(px, 0));
        if (vis != VG_VIS_EMPTY) return float4(0.1, 0.9, 0.2, 1);
        return (d < 1.0) ? float4(0.45, 0.45, 0.45, 1) : float4(0, 0, 0, 1);
    }
    if (mode == VG_DBG_OVERDRAW)
    {
        float n = 0;
        if (gHeap2.z != VG_NONE)
        {
            RWTexture2D<uint> od = ResourceDescriptorHeap[gHeap2.z];
            n = (float)od[px];
        }
        if (n <= 0.0) return float4(0, 0, 0, 1);
        return float4(VgHeat(n / max(gDbg.y, 1.0)), 1);
    }

    if (mode >= VG_DBG_RAW_BARY)
    {
        // テスト用の生データ（ブレンド無しの R32G32B32A32_FLOAT）。空の画素は (-1,-1,-1,-1)。
        VgVisSurface s;
        if (!VgReconstruct(vis, i.pos.xy, gViewProj, float2(gRast.yz), gHeap0.z, gOffs1.y, gHeap0.y, gHeap0.x, s))
            return float4(-1, -1, -1, -1);
        if (mode == VG_DBG_RAW_BARY)   return float4(s.bary, (float)s.slot);
        if (mode == VG_DBG_RAW_UVGRAD) return float4(s.uvDx, s.uvDy);
        return float4(s.uv, length(s.worldPosDx), (float)s.tri);
    }

    if (mode == VG_DBG_TILE_MATERIALS)
    {
        // 8x8 タイル内の異なる材質（アセット + 材質番号）の数。空の画素は数えない。
        const int2 t0 = (px >> 3) << 3;
        uint keys[8];
        uint n = 0;
        [loop] for (int y = 0; y < 8; ++y)
        [loop] for (int x = 0; x < 8; ++x)
        {
            const uint v = visTex.Load(int3(t0 + int2(x, y), 0));
            if (v == VG_VIS_EMPTY) continue;
            RWByteAddressBuffer w = VgWork();
            const uint2 e = w.Load2(gOffs1.y + (v >> VG_VIS_TRI_BITS) * 8u);
            StructuredBuffer<VgInstance> insts = ResourceDescriptorHeap[gHeap0.y];
            StructuredBuffer<VgAssetGpu> assts = ResourceDescriptorHeap[gHeap0.x];
            const VgInstance ii = insts[e.x];
            const VgAssetGpu aa = assts[ii.assetIndex];
            const uint pg = e.y & 0xFFFFu;
            ByteAddressBuffer pl = ResourceDescriptorHeap[NonUniformResourceIndex(aa.poolSrvBase + (pg >> VG_CHUNK_PAGES_LOG2))];
            const uint hb = (pg & ((1u << VG_CHUNK_PAGES_LOG2) - 1u)) * VG_PAGE_BYTES + 64u + ((e.y >> 16) & 0xFFu) * 128u;
            const uint key = (ii.assetIndex << 16) | (pl.Load(hb + 72) >> 16);
            bool found = false;
            [loop] for (uint k = 0; k < n; ++k) found = found || (keys[k] == key);
            if (!found && n < 8) keys[n++] = key;
        }
        if (vis == VG_VIS_EMPTY) return float4(0.0, 0.0, 0.0, 0.8);
        return float4(VgHeat(saturate((float(n) - 1.0) / 3.0)), 1);
    }

    if (vis == VG_VIS_EMPTY)
    {
        if (mode == VG_DBG_SHADE) discard;
        return float4(0.0, 0.0, 0.0, 0.8);
    }

    if (mode == VG_DBG_MATERIAL || mode == VG_DBG_MIP || mode == VG_DBG_NORMAL)
    {
        VgVisSurface s;
        VgReconstruct(vis, i.pos.xy, gViewProj, float2(gRast.yz), gHeap0.z, gOffs1.y, gHeap0.y, gHeap0.x, s);
        if (mode == VG_DBG_MATERIAL) return float4(VgHashColor(VgHash((s.inst.assetIndex + 1u) * 0x9E3779B1u ^ (s.materialIndex * 0x85EBCA6Bu))), 1);
        if (mode == VG_DBG_NORMAL)   return float4(normalize(s.worldNormal) * 0.5 + 0.5, 1);
        const VgMaterialGpu m = VgLoadMaterial(gHeap1.w, s.asset, s.materialIndex);
        if (m.albedoSrv == VG_NONE) return float4(0.35, 0.35, 0.35, 1);
        Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(m.albedoSrv)];
        uint tw, th, levels;
        tex.GetDimensions(0, tw, th, levels);
        const float2 sz = float2(tw, th);
        const float2 gx = s.uvDx * m.uvScaleOffset.xy * sz, gy = s.uvDy * m.uvScaleOffset.xy * sz;
        const float lod = 0.5 * log2(max(max(dot(gx, gx), dot(gy, gy)), 1e-12));
        return float4(VgHeat(saturate(lod / 10.0)), 1);
    }

    RWByteAddressBuffer work = VgWork();
    const uint slot = vis >> VG_VIS_TRI_BITS;
    const uint tri = vis & VG_VIS_TRI_MASK;
    const uint2 e = work.Load2(gOffs1.y + slot * 8u);        // {instance, clusterRef}

    if (mode == VG_DBG_CLUSTER) return float4(VgHashColor(VgHash(e.x * 0x9E3779B1u ^ e.y)), 1);
    if (mode == VG_DBG_TRI)     return float4(VgHashColor(VgHash((e.x * 0x9E3779B1u ^ e.y) * 131u + tri)), 1);

    StructuredBuffer<VgInstance> instances = ResourceDescriptorHeap[gHeap0.y];
    StructuredBuffer<VgAssetGpu> assets     = ResourceDescriptorHeap[gHeap0.x];
    const VgInstance inst = instances[e.x];
    const VgAssetGpu asset = assets[inst.assetIndex];
    const uint page = e.y & 0xFFFFu;
    const uint cidx = (e.y >> 16) & 0xFFu;
    const uint pageBase = (page & ((1u << VG_CHUNK_PAGES_LOG2) - 1u)) * VG_PAGE_BYTES;
    ByteAddressBuffer pool = ResourceDescriptorHeap[asset.poolSrvBase + (page >> VG_CHUNK_PAGES_LOG2)];
    const VgClusterRaster cl = VgLoadClusterRaster(pool, pageBase, cidx);

    if (mode == VG_DBG_LOD)
    {
        const float lv = (float)((cl.flags >> 8) & 0xFFu);
        return float4(VgHeat(lv / max((float)asset.levelCount - 1.0, 1.0)), 1);
    }

    // 暫定シェーディング: 面法線のランバート
    const uint3 t = VgReadTri(pool, pageBase, cl, tri);
    const float3 p0 = VgXform(inst.w0, inst.w1, inst.w2, VgDecodePos(pool, pageBase, cl, t.x, asset.posOrigin, asset.posStep));
    const float3 p1 = VgXform(inst.w0, inst.w1, inst.w2, VgDecodePos(pool, pageBase, cl, t.y, asset.posOrigin, asset.posStep));
    const float3 p2 = VgXform(inst.w0, inst.w1, inst.w2, VgDecodePos(pool, pageBase, cl, t.z, asset.posOrigin, asset.posStep));
    float3 n = cross(p1 - p0, p2 - p0);
    const float len = length(n);
    n = len > 0.0 ? n / len : float3(0, 1, 0);
    if (dot(n, gCamPos.xyz - p0) < 0.0) n = -n;               // 両面（巻き順に依存しない）
    const float3 L = normalize(float3(0.45, 0.8, -0.35));
    const float hemi = n.y * 0.5 + 0.5;
    const float3 albedo = float3(0.72, 0.72, 0.72);
    const float3 col = albedo * (lerp(0.12, 0.30, hemi) + 0.85 * saturate(dot(n, L)));
    return float4(col, 1);
}
