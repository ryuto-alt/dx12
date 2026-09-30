#pragma once
//
// VgCullReference.h ― GPU カリング（shaders/vg/*.hlsl）の CPU 参照実装。テスト専用（tests/vg_cull_gpu_test.cpp）。
//
//   ★GPU と「同じアルゴリズム・同じ数式」で書く（インスタンス → BVH 走査 → クラスタ → 二相 HZB）。
//     式は VgLodMath.h（LOD / 錐台 / コーン）と renderer/HiZMath.h（HZB の投影・ミップ選択・遮蔽判定）を**そのまま呼ぶ**。
//     HLSL は VgLodMath.h / HiZMath.h の手書きの写しなので、GPU の結果 = この参照 が写しのドリフト検出になる。
//   ★出力の順序は GPU では不定（InterlockedAdd）。比較は集合で行う。
//   ★ヘッダオンリー。VgeoFormat.h（リーダ）+ HiZMath.h（DirectXMath）に依存する。
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "renderer/HiZMath.h"
#include "renderer/vg/VgGpuTypes.h"
#include "renderer/vg/VgLodMath.h"
#include "renderer/vg/VgeoFormat.h"

namespace dx12e::vg
{

// 全ページをメモリに持つ CPU 側のアセット（テスト用の小さいアセット向け）。
struct CpuAsset
{
    VgeoMeta meta;
    std::vector<u8> pages;
    bool Load(const ByteSource& src)
    {
        ReadOptions ro;
        if (!LoadMeta(src, meta, ro).ok()) return false;
        pages.assign(static_cast<size_t>(meta.header.pageCount) * kPageSize, 0);
        for (u32 p = 0; p < meta.header.pageCount; ++p)
            if (!ReadPage(src, meta, p, pages.data() + static_cast<size_t>(p) * kPageSize, ro).ok()) return false;
        return true;
    }
    ClusterHeader Cluster(u32 page, u32 idx) const { return ReadClusterHeader(pages.data() + static_cast<size_t>(page) * kPageSize, idx); }
};

// CPU 側の HZB ピラミッド（max 縮約。mip0 = 深度そのもの。奇数の端は 3x3 に広げて畳む = HiZBuild.hlsl と同じ）。
struct HzbCpu
{
    u32 w = 0, h = 0, mips = 0;
    std::vector<std::vector<f32>> data;      // data[m] = (w_m * h_m)
    std::vector<std::pair<u32, u32>> size;   // size[m] = (w_m, h_m)
    f32 Load(i32 x, i32 y, u32 m) const
    {
        const auto [mw, mh] = size[m];
        x = std::clamp<i32>(x, 0, static_cast<i32>(mw) - 1);
        y = std::clamp<i32>(y, 0, static_cast<i32>(mh) - 1);
        return data[m][static_cast<size_t>(y) * mw + static_cast<size_t>(x)];
    }
    static HzbCpu FromDepth(const std::vector<f32>& depth, u32 w, u32 h)
    {
        HzbCpu z;
        z.w = w; z.h = h;
        z.data.push_back(depth);
        z.size.push_back({w, h});
        u32 mw = w, mh = h;
        while (mw > 1 || mh > 1)
        {
            const u32 nw = std::max(1u, mw / 2), nh = std::max(1u, mh / 2);
            std::vector<f32> nx(static_cast<size_t>(nw) * nh);
            const auto& src = z.data.back();
            for (u32 y = 0; y < nh; ++y)
                for (u32 x = 0; x < nw; ++x)
                {
                    // 出力テクセル (x, y) は入力の [2x, 2x+1] x [2y, 2y+1]。端（奇数寸法の余り）は 3 テクセルまで広げる
                    const u32 x0 = 2 * x, x1 = (x == nw - 1 && (mw & 1)) ? std::min(mw - 1, 2 * x + 2) : std::min(mw - 1, 2 * x + 1);
                    const u32 y0 = 2 * y, y1 = (y == nh - 1 && (mh & 1)) ? std::min(mh - 1, 2 * y + 2) : std::min(mh - 1, 2 * y + 1);
                    f32 m = 0.0f;
                    for (u32 yy = y0; yy <= y1; ++yy)
                        for (u32 xx = x0; xx <= x1; ++xx) m = std::max(m, src[static_cast<size_t>(yy) * mw + xx]);
                    nx[static_cast<size_t>(y) * nw + x] = m;
                }
            z.data.push_back(std::move(nx));
            z.size.push_back({nw, nh});
            mw = nw; mh = nh;
        }
        z.mips = static_cast<u32>(z.data.size());
        return z;
    }
};

struct RefView
{
    f32 viewProj[16]{};
    f32 prevViewProj[16]{};
    f32 camPos[3]{};
    f32 zNear = 0.1f;
    f32 projScale = 1000.0f;
    f32 vpX = 0, vpY = 0, vpW = 1920, vpH = 1080;
    f32 tau = 1.0f;
    f32 instanceMinPx = 0.5f;
    bool coneCulling = true;
    const HzbCpu* hzbPrev = nullptr; bool prevValid = false;
    const HzbCpu* hzbCur = nullptr;  bool curValid = false;
};

struct RefStats
{
    u32 instances = 0, instFrustum = 0, instHzbRej = 0, nodesVisited = 0, childrenTested = 0, groups = 0;
    u32 clustersTested = 0, lodSelected = 0, visibleP1 = 0, visibleP2 = 0;
    u64 tris = 0, srcTris = 0;
    u32 clCullFrustum = 0, clCullCone = 0, clDeferred = 0, clHzbDrop = 0;
    u32 nodeCullFrustum = 0, nodeCullLod = 0, nodeDeferred = 0;
    u32 hist[VG_HIST_LEVELS] = {};
};

struct RefResult
{
    std::set<std::pair<u32, u32>> visible;       // {instance, clusterRef}
    std::set<std::pair<u32, u32>> phase2Only;    // うち二相目で救われたもの
    RefStats stats;
};

namespace refdetail
{
// HZB 判定（VgCommon.hlsli の VgHzbOccludedSphere と同じ手順。投影 / ミップ選択 / 遮蔽は HiZMath.h を直接呼ぶ）
inline bool HzbOccludedSphere(const HzbCpu& hzb, const f32 vp[16], const f32 c[3], f32 r, const RefView& v)
{
    using namespace DirectX;
    XMFLOAT4X4 m;
    std::memcpy(&m, vp, 64);
    HiZViewport viewport;
    viewport.originX = v.vpX; viewport.originY = v.vpY; viewport.width = v.vpW; viewport.height = v.vpH;
    const ScreenBounds sb = ProjectAabbToScreen(XMVectorSet(c[0] - r, c[1] - r, c[2] - r, 0.0f), XMVectorSet(c[0] + r, c[1] + r, c[2] + r, 0.0f), XMLoadFloat4x4(&m), viewport);
    if (!sb.valid) return false;
    const u32 mip = SelectHiZMip(sb, hzb.mips);
    const f32 inv = 1.0f / static_cast<f32>(1u << mip);
    const i32 msx = std::max(1, static_cast<i32>(static_cast<f32>(hzb.w) * inv));
    const i32 msy = std::max(1, static_cast<i32>(static_cast<f32>(hzb.h) * inv));
    const i32 t0x = std::clamp(static_cast<i32>(sb.minX * inv), 0, msx - 1), t0y = std::clamp(static_cast<i32>(sb.minY * inv), 0, msy - 1);
    const i32 t1x = std::clamp(static_cast<i32>(sb.maxX * inv), 0, msx - 1), t1y = std::clamp(static_cast<i32>(sb.maxY * inv), 0, msy - 1);
    f32 mx = hzb.Load(t0x, t0y, mip);
    mx = std::max(mx, hzb.Load(t1x, t0y, mip));
    mx = std::max(mx, hzb.Load(t0x, t1y, mip));
    mx = std::max(mx, hzb.Load(t1x, t1y, mip));
    return IsOccludedByHiZ(sb, mx);
}

inline bool HzbEnabled(const RefView& v, u32 phase) { return phase == 0 ? (v.prevValid && v.hzbPrev) : (v.curValid && v.hzbCur); }

inline bool HzbOccluded(const RefView& v, u32 phase, const VgInstance& in, const f32 localC[3], f32 localR)
{
    if (!HzbEnabled(v, phase)) return false;
    f32 c[3];
    if (phase == 0)
    {
        TransformPoint34(in.prevWorld, localC, c);
        return HzbOccludedSphere(*v.hzbPrev, v.prevViewProj, c, localR * in.maxScale, v);
    }
    TransformPoint34(in.world, localC, c);
    return HzbOccludedSphere(*v.hzbCur, v.viewProj, c, localR * in.maxScale, v);
}
} // namespace refdetail

// 1 回の Cull（インスタンス配列 x アセット配列）。GPU の Execute と同じ二相構成。
inline RefResult CullReference(const std::vector<const CpuAsset*>& assets, const std::vector<VgInstanceInput>& inputs, const RefView& v)
{
    RefResult R;
    RefStats& S = R.stats;
    FrustumPlanes planes;
    ExtractFrustumPlanes(v.viewProj, planes);

    std::vector<VgInstance> inst;
    inst.reserve(inputs.size());
    std::vector<std::array<f32, 3>> camObj(inputs.size());
    std::vector<u8> camObjOk(inputs.size(), 0);
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        inst.push_back(MakeInstance(inputs[i], inputs[i].assetId));
        f32 o[3] = {0, 0, 0};
        camObjOk[i] = CameraToObjectSpace(inputs[i].world, v.camPos, o) ? 1 : 0;
        camObj[i] = {o[0], o[1], o[2]};
    }
    S.instances = static_cast<u32>(inst.size());

    using Q = std::vector<std::pair<u32, u32>>;   // {instance, node}
    Q nq, deferredNodes;
    std::vector<std::pair<u32, u32>> gq0, gq1;    // {instance, groupPacked}

    // K0
    for (u32 i = 0; i < inst.size(); ++i)
    {
        const VgInstance& in = inst[i];
        const CpuAsset& a = *assets[in.assetIndex];
        const VgeoHeader& h = a.meta.header;
        const f32 lc[3] = {h.boundingSphere[0], h.boundingSphere[1], h.boundingSphere[2]};
        const f32 lr = h.boundingSphere[3];
        f32 c[3];
        TransformPoint34(in.world, lc, c);
        const f32 r = lr * in.maxScale;
        bool cull = SphereOutsideFrustum(planes, c, r);
        if (!cull && v.instanceMinPx > 0.0f)
        {
            const f32 dx = c[0] - v.camPos[0], dy = c[1] - v.camPos[1], dz = c[2] - v.camPos[2];
            f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < v.zNear) dist = v.zNear;
            const f32 px = r * v.projScale / dist;
            if (px < v.instanceMinPx) cull = true;
        }
        if (cull) continue;
        ++S.instFrustum;
        {
            const u64 st = h.sourceTriangleCount;
            S.srcTris += std::min<u64>(st, 0xFFFFFFFFull);   // GPU の VgAssetGpu.sourceTriangles は u32 に飽和する
        }
        if (refdetail::HzbOccluded(v, 0, in, lc, lr)) { ++S.instHzbRej; deferredNodes.push_back({i, h.rootNode}); }
        else nq.push_back({i, h.rootNode});
    }

    // K1: 1 段ぶんの走査（phase 0 / 1 で同じコード。GPU と同じ分岐）
    auto traverse = [&](u32 phase, Q cur)
    {
        while (!cur.empty())
        {
            Q next;
            for (const auto& e : cur)
            {
                const VgInstance& in = inst[e.first];
                const CpuAsset& a = *assets[in.assetIndex];
                ++S.nodesVisited;
                const HierNode& node = a.meta.nodes[e.second];
                for (int ci = 0; ci < 4; ++ci)
                {
                    const HierChild& c = node.child[ci];
                    if (c.ref == kNone) continue;
                    ++S.childrenTested;
                    const f32 s = in.maxScale;
                    f32 cc[3];
                    TransformPoint34(in.world, c.cullSphere, cc);
                    if (SphereOutsideFrustum(planes, cc, c.cullSphere[3] * s)) { ++S.nodeCullFrustum; continue; }
                    f32 lc[3];
                    TransformPoint34(in.world, c.lodSphere, lc);
                    const f32 ls[4] = {lc[0], lc[1], lc[2], c.lodSphere[3] * s};
                    const bool parentCoarse = ProjectedErrorPx(c.maxParentError, ls, v.camPos, s, v.projScale, v.zNear) > v.tau;
                    const bool ownFine = !(ProjectedErrorPxFar(c.minOwnError, ls, v.camPos, s, v.projScale, v.zNear) > v.tau);
                    if (!(parentCoarse && ownFine)) { ++S.nodeCullLod; continue; }
                    const bool leaf = (c.ref & 0x80000000u) != 0;
                    if (refdetail::HzbOccluded(v, phase, in, c.cullSphere, c.cullSphere[3]))
                    {
                        if (phase == 0)
                        {
                            ++S.nodeDeferred;
                            if (leaf) gq1.push_back({e.first, c.groupPacked});
                            else deferredNodes.push_back({e.first, c.ref});
                        }
                    }
                    else if (leaf)
                    {
                        ++S.groups;
                        (phase == 0 ? gq0 : gq1).push_back({e.first, c.groupPacked});
                    }
                    else next.push_back({e.first, c.ref});
                }
            }
            cur = std::move(next);
        }
    };

    // K2
    auto clusters = [&](u32 phase, const std::vector<std::pair<u32, u32>>& gq)
    {
        for (const auto& e : gq)
        {
            const VgInstance& in = inst[e.first];
            const CpuAsset& a = *assets[in.assetIndex];
            const u32 gp = e.second;
            const u32 count = GroupCount(gp), page = GroupPage(gp), first = GroupFirst(gp);
            for (u32 slot = 0; slot < count; ++slot)
            {
                const u32 idx = first + slot;
                const ClusterHeader ch = a.Cluster(page, idx);
                ++S.clustersTested;
                const f32 s = in.maxScale;
                f32 lc[3], pc[3];
                TransformPoint34(in.world, ch.lodSphere, lc);
                TransformPoint34(in.world, ch.parentLodSphere, pc);
                const f32 lsW[4] = {lc[0], lc[1], lc[2], ch.lodSphere[3] * s};
                const f32 psW[4] = {pc[0], pc[1], pc[2], ch.parentLodSphere[3] * s};
                const bool parentCoarse = ProjectedErrorPx(ch.parentLodError, psW, v.camPos, s, v.projScale, v.zNear) > v.tau;
                const bool fine = !(ProjectedErrorPx(ch.lodError, lsW, v.camPos, s, v.projScale, v.zNear) > v.tau);
                if (!(parentCoarse && fine)) continue;
                ++S.lodSelected;
                f32 cc[3];
                TransformPoint34(in.world, ch.cullSphere, cc);
                if (SphereOutsideFrustum(planes, cc, ch.cullSphere[3] * s)) { ++S.clCullFrustum; continue; }
                bool coneCull = false;
                if (v.coneCulling && camObjOk[e.first])
                {
                    const f32 cutoff = static_cast<f32>(ConeByte(ch.coneS8, 3)) / 127.0f;
                    if (cutoff < 1.0f)
                    {
                        const f32 axis[3] = {ConeAxisComponent(ch.coneS8, 0), ConeAxisComponent(ch.coneS8, 1), ConeAxisComponent(ch.coneS8, 2)};
                        coneCull = ConeCulls(ch.cullSphere, ch.cullSphere[3], axis, cutoff, camObj[e.first].data());
                    }
                }
                if (coneCull) { ++S.clCullCone; continue; }
                if (refdetail::HzbOccluded(v, phase, in, ch.cullSphere, ch.cullSphere[3]))
                {
                    if (phase == 0) { ++S.clDeferred; gq1.push_back({e.first, MakeGroupPacked(page, idx, 1)}); }
                    else ++S.clHzbDrop;
                    continue;
                }
                R.visible.insert({e.first, MakeClusterRef(page, idx)});
                if (phase == 0) ++S.visibleP1; else { ++S.visibleP2; R.phase2Only.insert({e.first, MakeClusterRef(page, idx)}); }
                S.tris += ClusterTriangleCount(ch);
                ++S.hist[std::min<u32>(ClusterLevel(ch), VG_HIST_LEVELS - 1)];
            }
        }
    };

    traverse(0, nq);
    const std::vector<std::pair<u32, u32>> g0 = gq0;    // 一相目の K2 の入力（K2 が gq1 へ積むので固定しておく）
    clusters(0, g0);
    // 二相目: Deferred ノード → 走査 → GroupQ1 全部を K2
    const Q def = deferredNodes;
    traverse(1, def);
    const std::vector<std::pair<u32, u32>> g1 = gq1;
    clusters(1, g1);
    return R;
}

} // namespace dx12e::vg
