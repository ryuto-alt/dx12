// VgInstanceCull.hlsl ― K0: インスタンスカリング（1 スレッド = 1 インスタンス）。cs_6_6。
//
// アセット全体の外接球をワールドへ → 錐台 / 極小棄却 → 一相目は前フレーム HZB。
// 生き残り → NodeQ0 に {instance, rootNode}。一相目の HZB でだけ落ちたもの → Deferred に {instance, rootNode}
// （二相目が今フレームの HZB で救う）。カメラのオブジェクト空間座標（法線コーンの背面棄却用）を instExtra へ書く。
// 設計書 §2.4.2。式は src/renderer/vg/VgLodMath.h / VgCullReference.h と同一。
#include "VgCommon.hlsli"

// オブジェクト空間へ戻したカメラ位置（VgLodMath.h の CameraToObjectSpace と同一）。w = 1 で有効、0 で「コーン棄却しない」。
float4 VgCamToObject(VgInstance inst)
{
    // world の行 i = 4x4 の列 i → 4x4 行列 m[r][c] は w{c}[r]
    const float m00 = inst.w0.x, m10 = inst.w0.y, m20 = inst.w0.z, m30 = inst.w0.w;
    const float m01 = inst.w1.x, m11 = inst.w1.y, m21 = inst.w1.z, m31 = inst.w1.w;
    const float m02 = inst.w2.x, m12 = inst.w2.y, m22 = inst.w2.z, m32 = inst.w2.w;
    const float det = m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) + m02 * (m10 * m21 - m11 * m20);
    if (!(abs(det) > 1e-30)) return float4(0, 0, 0, 0);
    const float inv = 1.0 / det;
    float a[9];
    a[0] = (m11 * m22 - m12 * m21) * inv;
    a[1] = (m02 * m21 - m01 * m22) * inv;
    a[2] = (m01 * m12 - m02 * m11) * inv;
    a[3] = (m12 * m20 - m10 * m22) * inv;
    a[4] = (m00 * m22 - m02 * m20) * inv;
    a[5] = (m02 * m10 - m00 * m12) * inv;
    a[6] = (m10 * m21 - m11 * m20) * inv;
    a[7] = (m01 * m20 - m00 * m21) * inv;
    a[8] = (m00 * m11 - m01 * m10) * inv;
    const float3 d = gCamPos.xyz - float3(m30, m31, m32);
    float3 o;
    o.x = d.x * a[0] + d.y * a[3] + d.z * a[6];
    o.y = d.x * a[1] + d.y * a[4] + d.z * a[7];
    o.z = d.x * a[2] + d.y * a[5] + d.z * a[8];
    return float4(o, 1.0);
}

[numthreads(VG_THREADS, 1, 1)]
void CSInstanceCull(uint3 dtid : SV_DispatchThreadID)
{
    RWByteAddressBuffer work = VgWork();
    const uint i = dtid.x;
    const bool valid = i < gCounts.x;

    uint pass = 0;          // 1 = 錐台を通った
    uint hzbRej = 0;
    uint srcTris = 0;

    if (valid)
    {
        StructuredBuffer<VgInstance> instances = ResourceDescriptorHeap[gHeap0.y];
        StructuredBuffer<VgAssetGpu> assets     = ResourceDescriptorHeap[gHeap0.x];
        RWStructuredBuffer<float4>   extra      = ResourceDescriptorHeap[gHeap0.w];

        const VgInstance inst = instances[i];
        const VgAssetGpu asset = assets[inst.assetIndex];

        const float3 lc = asset.boundingSphere.xyz;
        const float  lr = asset.boundingSphere.w;
        const float3 c = VgXform(inst.w0, inst.w1, inst.w2, lc);
        const float  r = lr * inst.maxScale;

        extra[i] = VgCamToObject(inst);

        bool cull = VgSphereOutsideFrustum(c, r);

        // 画面上の半径が小さすぎるインスタンスは棄却（gLod.z = 0 で無効）
        if (!cull && gLod.z > 0.0)
        {
            const float3 dv = c - gCamPos.xyz;
            float dist = sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
            if (dist < gCamPos.w) dist = gCamPos.w;
            const float px = r * gLod.y / dist;
            if (px < gLod.z) cull = true;
        }

        if (!cull)
        {
            pass = 1;
            srcTris = asset.sourceTriangles;
            if (VgHzbOccluded(0, inst, lc, lr))
            {
                hzbRej = 1;
                VgPush(work, gOffs0.z, VG_CNT_DEFERRED, gCaps.w, VG_STAT_OVF_DEFERRED, uint2(i, asset.rootNode));
            }
            else
            {
                VgPush(work, gOffs0.x, VG_CNT_NODEQ0, gCaps.x, VG_STAT_OVF_NODEQ, uint2(i, asset.rootNode));
            }
        }
    }

    VgStatAdd(work, VG_STAT_INSTANCES, valid ? 1u : 0u);
    VgStatAdd(work, VG_STAT_INST_FRUSTUM, pass);
    VgStatAdd(work, VG_STAT_INST_HZB_REJ, hzbRej);

    // 錐台内の LOD0 三角形数（u64）。インスタンスごとに 1 回（最大 65536 回）足し、下位が折り返したら上位へ繰り上げる。
    // ★ウェーブ合計は u32 で折り返しうる（64 インスタンス x 1 億 tri）ので使わない。
    if (srcTris != 0)
    {
        uint o;
        work.InterlockedAdd(gOffs1.w + VG_STAT_SRC_TRIS_LO * 4u, srcTris, o);
        if (o + srcTris < o) work.InterlockedAdd(gOffs1.w + VG_STAT_SRC_TRIS_HI * 4u, 1u, o);
    }
}
