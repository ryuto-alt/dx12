// VgeoDecode.hlsli ― .vgeo のクラスタ（ページ内の頂点 / 三角形ブロック）を GPU でデコードする部品（P3）。
//
// ★仕様 = docs/VGEO_SPEC.md §5 / §9。CPU 参照 = src/renderer/vg/VgeoFormat.h の DecodeCluster（tests/vg_raster_gpu_test.cpp が
//   GPU の結果と一致することを確かめる）。頂点の位置は「origin + q * step」（f32）で、DequantizeCoord と同じ式。
// ★ページプール = アセットごとの ByteAddressBuffer チャンク（ページ p = チャンク p >> 12、バイト (p & 4095) * 131072）。
#ifndef VGEO_DECODE_HLSLI
#define VGEO_DECODE_HLSLI

// クラスタヘッダの「ラスタ / シェーディング側」（ページ先頭 + 64 + 128 * i の +64 以降）
struct VgClusterRaster
{
    uint vertexOffset;      // ページ先頭からのバイト
    uint triangleOffset;
    uint vertexCount;
    uint triangleCount;
    uint materialIndex;
    uint3 bits;             // bx, by, bz
    int3 posMin;
    uint flags;             // bit0 = LOD0 / [8:16) = level
    float maxEdge;          // 最長辺（アセット空間。+60）
};

VgClusterRaster VgLoadClusterRaster(ByteAddressBuffer pool, uint pageBase, uint clusterIdx)
{
    const uint hb = pageBase + 64u + clusterIdx * 128u;
    const uint4 a = pool.Load4(hb + 64);     // vertexOffset, triangleOffset, packedCounts, posBits
    const uint4 b = pool.Load4(hb + 80);     // posMin.xyz, flags
    VgClusterRaster c;
    c.vertexOffset = a.x;
    c.triangleOffset = a.y;
    c.vertexCount = a.z & 0xFFu;
    c.triangleCount = (a.z >> 8) & 0xFFu;
    c.materialIndex = a.z >> 16;
    c.bits = uint3(a.w & 31u, (a.w >> 5) & 31u, (a.w >> 10) & 31u);
    c.posMin = int3(b.x, b.y, b.z);
    c.flags = b.w;
    c.maxEdge = asfloat(pool.Load(hb + 60));
    return c;
}

// n <= 24 ビット、LSB ファースト。base はブロック先頭のバイト。bitPos + n の範囲は最大 55 ビット先まで（2 dword）。
uint VgLoadBits(ByteAddressBuffer pool, uint base, uint bitPos, uint n)
{
    const uint byteAddr = base + (bitPos >> 3);
    const uint sh = (byteAddr & 3u) * 8u + (bitPos & 7u);          // 0..31
    const uint2 w = pool.Load2(byteAddr & ~3u);
    const uint v = (w.x >> sh) | ((sh != 0u) ? (w.y << (32u - sh)) : 0u);
    return v & ((1u << n) - 1u);
}

// 頂点 i の量子化座標 → アセット空間の位置（VgeoFormat.h の DequantizeCoord と同じ式）
float3 VgDecodePos(ByteAddressBuffer pool, uint pageBase, VgClusterRaster c, uint i, float3 origin, float step)
{
    const uint bpp = c.bits.x + c.bits.y + c.bits.z;
    const uint base = pageBase + c.vertexOffset;
    const uint bit = i * bpp;
    const uint3 q = uint3(VgLoadBits(pool, base, bit, c.bits.x),
                          VgLoadBits(pool, base, bit + c.bits.x, c.bits.y),
                          VgLoadBits(pool, base, bit + c.bits.x + c.bits.y, c.bits.z)) + asuint(c.posMin);
    return origin + float3(q) * step;
}

// 三角形 t の 3 頂点のローカル番号（4 バイト境界をまたぐので 2 dword から取り出す）
uint3 VgReadTri(ByteAddressBuffer pool, uint pageBase, VgClusterRaster c, uint t)
{
    const uint b = pageBase + c.triangleOffset + t * 3u;
    const uint2 w = pool.Load2(b & ~3u);
    const uint sh = (b & 3u) * 8u;
    const uint v = (w.x >> sh) | ((sh != 0u) ? (w.y << (32u - sh)) : 0u);
    return uint3(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu);
}

#endif // VGEO_DECODE_HLSLI
