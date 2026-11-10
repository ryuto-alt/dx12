// VgCommon.hlsli ― 仮想ジオメトリ GPU カリング（P2）の共通部品。cs_6_6（バインドレス）。
//
// ★C++ 側との対応（1 バイトずつ一致させる。片方だけ直すと壊れる）:
//     構造体 / 定数バッファ … src/renderer/vg/VgGpuTypes.h（static_assert でサイズ固定）
//     数字（カウンタ番号・統計番号・スレッド構成）… src/renderer/vg/VgShared.h（この HLSL が #include する）
//     数式 … src/renderer/vg/VgLodMath.h（CPU 参照と同じ演算順）。CPU 参照 = VgCullReference.h
// ★専用ルートシグネチャ（CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED）。メインの RS には一切触らない。
//     b0 = カリング定数（ルート CBV）/ b1 = パスごとの定数（ルート定数 8 DWORD）。
//     バッファ・テクスチャは全部 ResourceDescriptorHeap[] から引く（添字は b0 の gHeap*）。
// ★P2 の出力は「可視クラスタ配列 + 統計」。P3 はそれをメッシュシェーダで可視性バッファ + 深度へ描く（VgRaster.hlsl）。
#ifndef VG_COMMON_HLSLI
#define VG_COMMON_HLSLI

#include "VgTypes.hlsli"      // 構造体（VgInstance / VgAssetGpu / VgMaterialGpu / HierNode）と VgShared.h

cbuffer VgCullCB : register(b0)
{
    float4x4 gViewProj;
    float4x4 gPrevViewProj;
    float4   gPlanes[6];
    float4   gCamPos;        // xyz, w = zNear
    float4   gViewport;      // x, y, w, h
    float4   gLod;           // x = tau(px), y = projScale, z = instanceMinPx, w = HZB の球の矩形（1 = 正確 / 0 = 旧の立方体）
    float4   gHzb;           // x, y = mip0 解像度, z = ミップ数
    uint4    gCounts;        // x = instanceCount, y = hzbPrevValid, z = hzbCurValid, w = coneCulling
    uint4    gHeap0;         // x = assetsSrv, y = instancesSrv, z = workUav, w = instExtraUav
    uint4    gHeap1;         // x = hzbPrevSrv, y = hzbCurSrv, z = argsUav, w = materialsSrv（P4）
    uint4    gCaps;          // x = nodeQCap, y = groupQCap, z = visibleCap, w = deferredCap
    uint4    gOffs0;         // x = nodeQ0, y = nodeQ1, z = deferred, w = groupQ0
    uint4    gOffs1;         // x = groupQ1, y = visible, z = counters, w = stats
    uint4    gHeap2;         // P3: x = depthSrv, y = visSrv, z = overdrawUav, w = visUav
    uint4    gRast;          // P3: x = ラスタのフラグ(VG_RF_*), y = 可視性バッファの幅, z = 高さ
    float4   gDbg;           // P3: x = 深度の表示レンジ(m), y = 過剰描画の表示上限, z = zFar
    float4x4 gViewProjNJ;    // P4: 今フレームのジッタ無し VP（速度）
    float4x4 gPrevViewProjNJ;// P4: 前フレームのジッタ無し VP（速度）
};

cbuffer VgPassCB : register(b1)
{
    uint4 gPass0;
    uint4 gPass1;
};

// ── 作業バッファ ────────────────────────────────────────────────────────────
RWByteAddressBuffer VgWork() { return ResourceDescriptorHeap[gHeap0.z]; }

uint LinearThread(uint3 gid, uint gi)
{
    return (gid.y * VG_GRID_X + gid.x) * VG_THREADS + gi;
}

// 統計（u32）。ウェーブ内の合計を 1 回のアトミックで足す。
// ★ WaveActiveSum は「そのコードに到達した有効レーン」の合計。呼び出し側は分岐の外（全レーンが通る位置）で呼ぶこと。
void VgStatAdd(RWByteAddressBuffer w, uint idx, uint v)
{
    const uint s = WaveActiveSum(v);
    if (WaveIsFirstLane() && s != 0)
    {
        uint o;
        w.InterlockedAdd(gOffs1.w + idx * 4u, s, o);
    }
}

// 1 スレッドぶんだけ足す（まれなイベント用）
void VgStatAdd1(RWByteAddressBuffer w, uint idx, uint v)
{
    uint o;
    w.InterlockedAdd(gOffs1.w + idx * 4u, v, o);
}

// キューへ {a, b} を積む。容量を超えたら捨てて overflow 統計を立てる。
void VgPush(RWByteAddressBuffer w, uint queueOffset, uint counterIdx, uint cap, uint ovfStat, uint2 v)
{
    uint slot;
    w.InterlockedAdd(gOffs1.z + counterIdx * 4u, 1u, slot);
    if (slot < cap) w.Store2(queueOffset + slot * 8u, v);
    else            VgStatAdd1(w, ovfStat, 1u);
}

// ── 変換・錐台 ──────────────────────────────────────────────────────────────
// （VgXform は VgTypes.hlsli。resolve と共有する）

bool VgSphereOutsideFrustum(float3 c, float r)
{
    [unroll]
    for (int k = 0; k < 6; ++k)
    {
        const float d = gPlanes[k].x * c.x + gPlanes[k].y * c.y + gPlanes[k].z * c.z + gPlanes[k].w;
        if (d < -r) return true;
    }
    return false;
}

// 誤差の画面射影（VgLodMath.h の ProjectedErrorPx と同一）
float VgProjErr(float err, float4 sphereW, float s)
{
    if (!(err < 3.0e38)) return 3.4e38;
    const float3 dv = sphereW.xyz - gCamPos.xyz;
    const float dist = sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
    float d = dist - sphereW.w;
    if (d < gCamPos.w) d = gCamPos.w;
    return err * s * gLod.y / d;
}

// 球の最遠点で射影（VgLodMath.h の ProjectedErrorPxFar と同一）
float VgProjErrFar(float err, float4 sphereW, float s)
{
    if (!(err < 3.0e38)) return 3.4e38;
    const float3 dv = sphereW.xyz - gCamPos.xyz;
    float d = sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z) + sphereW.w;
    if (d < gCamPos.w) d = gCamPos.w;
    return err * s * gLod.y / d;
}

// ── HZB（式は src/renderer/HiZMath.h / shaders/hiz/HiZCull.hlsl と一対一。保守原則: 迷ったら「見える」）──
struct VgScreenBounds { float4 rect; float minZ; bool valid; };

VgScreenBounds VgProjectAabb(float4x4 vp, float3 wmin, float3 wmax)
{
    VgScreenBounds o;
    o.rect = float4(0, 0, 0, 0);
    o.minZ = 0;
    o.valid = false;

    const float kMinW = 1e-4;
    if (!(wmin.x <= wmax.x) || !(wmin.y <= wmax.y) || !(wmin.z <= wmax.z)) return o;
    if (!(gViewport.z > 0) || !(gViewport.w > 0)) return o;

    float2 ndcMin = float2( 1e30,  1e30);
    float2 ndcMax = float2(-1e30, -1e30);
    float  ndcMinZ = 1e30;

    [unroll]
    for (int c = 0; c < 8; ++c)
    {
        const float3 p = float3((c & 1) ? wmax.x : wmin.x,
                                (c & 2) ? wmax.y : wmin.y,
                                (c & 4) ? wmax.z : wmin.z);
        const float4 clip = mul(float4(p, 1.0), vp);
        if (!(clip.w > kMinW)) return o;
        const float3 ndc = clip.xyz / clip.w;
        if (!(ndc.x == ndc.x) || !(ndc.y == ndc.y) || !(ndc.z == ndc.z)) return o;
        if (ndc.z < 0.0) return o;
        ndcMin = min(ndcMin, ndc.xy);
        ndcMax = max(ndcMax, ndc.xy);
        ndcMinZ = min(ndcMinZ, ndc.z);
    }
    if (ndcMinZ > 1.0) return o;

    const float sx0 = gViewport.x + (ndcMin.x * 0.5 + 0.5) * gViewport.z;
    const float sx1 = gViewport.x + (ndcMax.x * 0.5 + 0.5) * gViewport.z;
    const float sy0 = gViewport.y + (1.0 - (ndcMax.y * 0.5 + 0.5)) * gViewport.w;
    const float sy1 = gViewport.y + (1.0 - (ndcMin.y * 0.5 + 0.5)) * gViewport.w;
    const float vpMinX = gViewport.x, vpMaxX = gViewport.x + gViewport.z;
    const float vpMinY = gViewport.y, vpMaxY = gViewport.y + gViewport.w;
    if (sx1 < vpMinX || sx0 > vpMaxX || sy1 < vpMinY || sy0 > vpMaxY) return o;

    o.rect  = float4(max(sx0, vpMinX), max(sy0, vpMinY), min(sx1, vpMaxX), min(sy1, vpMaxY));
    o.minZ  = ndcMinZ;
    o.valid = true;
    return o;
}

uint VgSelectMip(float4 rect, uint mipCount)
{
    if (mipCount == 0) return 0;
    const float w = rect.z - rect.x;
    const float h = rect.w - rect.y;
    float maxSide = max(w, h);
    if (!(maxSide > 0.0)) maxSide = 1.0;

    int mip = 0;
    {
        float texel = 2.0;
        [loop]
        while (texel < maxSide && mip + 1 < (int)mipCount) { texel *= 2.0; ++mip; }
    }
    [loop]
    for (; mip + 1 < (int)mipCount; ++mip)
    {
        const float inv = 1.0 / (float)(1u << mip);
        const int x0 = (int)(rect.x * inv);
        const int x1 = (int)(rect.z * inv);
        const int y0 = (int)(rect.y * inv);
        const int y1 = (int)(rect.w * inv);
        if ((x1 - x0) <= 1 && (y1 - y0) <= 1) break;
    }
    return (uint)mip;
}

// 球の正確な画面矩形（VgLodMath.h の ProjectSphereToScreen と同一の式・同じ演算順）。ortho = 正射（この式は使えない）。
VgScreenBounds VgProjectSphere(float4x4 vp, float3 c, float r, out bool ortho)
{
    VgScreenBounds o;
    o.rect = float4(0, 0, 0, 0);
    o.minZ = 0;
    o.valid = false;
    ortho = false;
    const float kMinW = 1e-4;
    if (!(r >= 0.0) || !(gViewport.z > 0) || !(gViewport.w > 0)) return o;
    // mul(float4(p,1), vp) の成分 j = vp の列 j
    const float3 nX = float3(vp[0].x, vp[1].x, vp[2].x); const float oX = vp[3].x;
    const float3 nY = float3(vp[0].y, vp[1].y, vp[2].y); const float oY = vp[3].y;
    const float3 nZ = float3(vp[0].z, vp[1].z, vp[2].z); const float oZ = vp[3].z;
    const float3 nW = float3(vp[0].w, vp[1].w, vp[2].w); const float oW = vp[3].w;
    const float nw2 = nW.x * nW.x + nW.y * nW.y + nW.z * nW.z;
    if (!(nw2 > 1e-12)) { ortho = true; return o; }
    const float Xc = c.x * nX.x + c.y * nX.y + c.z * nX.z + oX;
    const float Yc = c.x * nY.x + c.y * nY.y + c.z * nY.z + oY;
    const float Wc = c.x * nW.x + c.y * nW.y + c.z * nW.z + oW;
    const float nwLen = sqrt(nw2);
    if (!(Wc - r * nwLen > kMinW)) return o;
    const float r2 = r * r;
    const float a = Wc * Wc - r2 * nw2;
    const float inv = 1.0 / a;
    float2 kx, ky;
    {
        const float b = Xc * Wc - r2 * (nX.x * nW.x + nX.y * nW.y + nX.z * nW.z);
        const float3 u = float3(Wc * nX.x - Xc * nW.x, Wc * nX.y - Xc * nW.y, Wc * nX.z - Xc * nW.z);
        const float3 x = float3(nX.y * nW.z - nX.z * nW.y, nX.z * nW.x - nX.x * nW.z, nX.x * nW.y - nX.y * nW.x);
        float disc = r2 * ((u.x * u.x + u.y * u.y + u.z * u.z) - r2 * (x.x * x.x + x.y * x.y + x.z * x.z));
        if (!(disc >= 0.0)) disc = 0.0;
        const float s = sqrt(disc);
        kx = float2((b - s) * inv, (b + s) * inv);
    }
    {
        const float b = Yc * Wc - r2 * (nY.x * nW.x + nY.y * nW.y + nY.z * nW.z);
        const float3 u = float3(Wc * nY.x - Yc * nW.x, Wc * nY.y - Yc * nW.y, Wc * nY.z - Yc * nW.z);
        const float3 x = float3(nY.y * nW.z - nY.z * nW.y, nY.z * nW.x - nY.x * nW.z, nY.x * nW.y - nY.y * nW.x);
        float disc = r2 * ((u.x * u.x + u.y * u.y + u.z * u.z) - r2 * (x.x * x.x + x.y * x.y + x.z * x.z));
        if (!(disc >= 0.0)) disc = 0.0;
        const float s = sqrt(disc);
        ky = float2((b - s) * inv, (b + s) * inv);
    }
    if (!(kx.x == kx.x) || !(kx.y == kx.y) || !(ky.x == ky.x) || !(ky.y == ky.y)) return o;
    const float k = r / nwLen;
    const float3 p = float3(c.x - k * nW.x, c.y - k * nW.y, c.z - k * nW.z);
    const float zc = p.x * nZ.x + p.y * nZ.y + p.z * nZ.z + oZ;
    const float wc = p.x * nW.x + p.y * nW.y + p.z * nW.z + oW;
    if (!(wc > kMinW)) return o;
    const float z = zc / wc;
    if (!(z == z) || z < 0.0 || z > 1.0) return o;
    const float sx0 = gViewport.x + (kx.x * 0.5 + 0.5) * gViewport.z;
    const float sx1 = gViewport.x + (kx.y * 0.5 + 0.5) * gViewport.z;
    const float sy0 = gViewport.y + (1.0 - (ky.y * 0.5 + 0.5)) * gViewport.w;
    const float sy1 = gViewport.y + (1.0 - (ky.x * 0.5 + 0.5)) * gViewport.w;
    const float vx1 = gViewport.x + gViewport.z, vy1 = gViewport.y + gViewport.w;
    if (sx1 < gViewport.x || sx0 > vx1 || sy1 < gViewport.y || sy0 > vy1) return o;
    o.rect = float4(max(sx0, gViewport.x), max(sy0, gViewport.y), min(sx1, vx1), min(sy1, vy1));
    o.minZ = z;
    o.valid = true;
    return o;
}

// 世界空間の球 (c, r) を HZB で判定する。true = 完全に遮蔽されている。
// hzbSrv = HZB の SRV 添字、vp = そのミップを作った深度と同じ（ジッタ付き）VP。
// 透視は球の正確な画面矩形（VgProjectSphere）、正射だけ従来の「球を包む軸平行箱」の投影。
bool VgHzbOccludedSphere(uint hzbSrv, float4x4 vp, float3 c, float r)
{
    bool ortho = true;
    VgScreenBounds sb;
    if (gLod.w > 0.5) sb = VgProjectSphere(vp, c, r, ortho);   // gLod.w = 1: 正確な球の矩形（0 = 旧の立方体。前後比較用）
    if (ortho) sb = VgProjectAabb(vp, c - r, c + r);
    if (!sb.valid) return false;                       // 判定不能は必ず「見える」

    const uint mipCount = (uint)gHzb.z;
    const uint mip = VgSelectMip(sb.rect, mipCount);
    const float inv = 1.0 / (float)(1u << mip);
    const int2 mipSize = max(int2(1, 1), int2(gHzb.xy * inv));
    const int2 t0 = clamp(int2(sb.rect.xy * inv), int2(0, 0), mipSize - 1);
    const int2 t1 = clamp(int2(sb.rect.zw * inv), int2(0, 0), mipSize - 1);

    Texture2D<float> hzb = ResourceDescriptorHeap[NonUniformResourceIndex(hzbSrv)];
    float hzbMax = hzb.Load(int3(t0.x, t0.y, mip));
    hzbMax = max(hzbMax, hzb.Load(int3(t1.x, t0.y, mip)));
    hzbMax = max(hzbMax, hzb.Load(int3(t0.x, t1.y, mip)));
    hzbMax = max(hzbMax, hzb.Load(int3(t1.x, t1.y, mip)));

    bool occluded = false;
    if ((hzbMax == hzbMax) && (hzbMax < 1.0))
        occluded = sb.minZ > hzbMax;
    return occluded;
}

// フェーズ別の HZB 判定。phase 0 = 前フレーム HZB（prevWorld / prevViewProj で投影）、phase 1 = 今フレームの HZB。
bool VgHzbEnabled(uint phase) { return (phase == 0) ? (gCounts.y != 0) : (gCounts.z != 0); }

bool VgHzbOccluded(uint phase, VgInstance inst, float3 localCenter, float localRadius)
{
    if (!VgHzbEnabled(phase)) return false;
    if (phase == 0)
    {
        const float3 c = VgXform(inst.p0, inst.p1, inst.p2, localCenter);
        return VgHzbOccludedSphere(gHeap1.x, gPrevViewProj, c, localRadius * inst.maxScale);
    }
    const float3 c = VgXform(inst.w0, inst.w1, inst.w2, localCenter);
    return VgHzbOccludedSphere(gHeap1.y, gViewProj, c, localRadius * inst.maxScale);
}

#endif // VG_COMMON_HLSLI
