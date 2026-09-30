#pragma once
// ===========================================================================
// 植生 F1: GPU カリングの CPU 参照（LOD 選択 / 間引き / クロスフェード / 球カリング）と風。
// ---------------------------------------------------------------------------
// ★shaders/foliage/FoliageCull.hlsl・FoliageWind.hlsli と式が一対一。**片方だけ直すと GPU と CPU がずれる**。
//   式を変えるときは必ず両方 + tests/foliage_test.cpp（CPU 単体）と tests/foliage_gpu_test.cpp（GPU 一致）を更新すること。
//
// 座標規約: 「列ベクトル」で書く（world = L * (B * local + p)）。GPU へ渡す 3x4 の各行 r0..r2 は
//   (基底行, 平行移動) = MeshInstanceData と同じ「transpose(world) の先頭 3 行」。
// ===========================================================================
#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "renderer/HiZMath.h"
#include "renderer/foliage/FoliageTypes.h"

namespace dx12e::foliage
{

// ---- 3x4 アフィン（列ベクトル規約。行 = 基底の行 + 平行移動）----
struct Affine34
{
    f32 m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    void Apply(const f32 p[3], f32 out[3]) const
    {
        for (int r = 0; r < 3; ++r) out[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
    }
    void ApplyDir(const f32 d[3], f32 out[3]) const
    {
        for (int r = 0; r < 3; ++r) out[r] = m[r][0] * d[0] + m[r][1] * d[1] + m[r][2] * d[2];
    }
};

// XMFLOAT4X4（行ベクトル規約の world）→ 列ベクトル規約の 3x4。
inline Affine34 AffineFromWorld(const DirectX::XMFLOAT4X4& w)
{
    Affine34 a;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            a.m[r][c] = (c < 3) ? w.m[c][r] : w.m[3][r];
    return a;
}

// 3x3 基底の列の長さの最大値（レイヤー Transform の一様でないスケールに対する保守的な半径倍率）。
inline f32 MaxColumnLength(const Affine34& a)
{
    f32 best = 0.0f;
    for (int c = 0; c < 3; ++c)
    {
        const f32 l = std::sqrt(a.m[0][c] * a.m[0][c] + a.m[1][c] * a.m[1][c] + a.m[2][c] * a.m[2][c]);
        best = std::max(best, l);
    }
    return best;
}

// インスタンス → ワールドの 3x4（レイヤーの Transform L を含む）。B = Rz(tz) * Rx(tx) * Ry(yaw) * s。
inline Affine34 ComposeInstance(const FoliageInstance& in, const Affine34& layer)
{
    const f32 yaw = UnpackYaw(in.rotScale);
    const f32 s   = UnpackScale(in.rotScale);
    const f32 tx  = UnpackTiltX(in.tilt), tz = UnpackTiltZ(in.tilt);
    const f32 cy = std::cos(yaw), sy = std::sin(yaw);
    const f32 cx = std::cos(tx),  sx = std::sin(tx);
    const f32 cz = std::cos(tz),  sz = std::sin(tz);
    // Ry（列ベクトル）: [[c,0,s],[0,1,0],[-s,0,c]]
    const f32 Ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    // Rx: [[1,0,0],[0,c,-s],[0,s,c]]
    const f32 Rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
    // Rz: [[c,-s,0],[s,c,0],[0,0,1]]
    const f32 Rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
    auto mul = [](const f32 a[3][3], const f32 b[3][3], f32 o[3][3])
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    };
    f32 t0[3][3], t1[3][3], B[3][3];
    mul(Rx, Ry, t0);
    mul(Rz, t0, t1);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) B[i][j] = t1[i][j] * s;
    // world = layer * [B | p]
    Affine34 out;
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
            out.m[r][c] = layer.m[r][0] * B[0][c] + layer.m[r][1] * B[1][c] + layer.m[r][2] * B[2][c];
        out.m[r][3] = layer.m[r][0] * in.px + layer.m[r][1] * in.py + layer.m[r][2] * in.pz + layer.m[r][3];
    }
    return out;
}

// ---- カリング ----
constexpr f32 kThinBand = 0.15f;   // 間引きのフェード幅（keepProb 空間）

struct CullParams
{
    f32  planes[6][4] = {};        // ワールド空間の平面（a,b,c,d）。内側 dot(n,p)+d >= 0。正規化済み
    f32  camPos[3]    = {0, 0, 0};
    f32  cullDist     = 200.0f;    // これ以上は描かない（m）
    f32  lodDist[3]   = {25.0f, 60.0f, 120.0f};   // LOD0→1 / 1→2 / 2→3 の切替距離（m）
    f32  thinStart    = 0.6f;      // cullDist の何割から密度を落とし始めるか（0..1）
    f32  lodFade      = 0.10f;     // 切替距離 D の ±lodFade×D の帯でクロスフェード（0 でオフ）
    f32  boundCenterY = 0.0f;      // ローカルのバウンディング球（LOD0 の全サブメッシュの外接）
    f32  boundRadius  = 1.0f;
    f32  radiusScale  = 1.0f;      // 風で揺れる分の余裕（>=1）
    f32  layerScale   = 1.0f;      // レイヤー Transform の最大スケール
    u32  variantCount = 1;
    u32  lodCount[kMaxVariants] = {4, 4, 4, 4};
    bool shadowPass   = false;     // 影パス: クロスフェード / ディザ無し、shadowMaxLod 超えは棄却
    u32  shadowMaxLod = 1;
};

// 1 インスタンスの出力先（最大 2 つ: クロスフェード中は隣り合う 2 つの LOD に出す）。
// PS は「ノイズ n が lo <= n <= hi なら discard」。通常は lo>=1（＝捨てない）。
struct EmitInfo
{
    u32 count = 0;
    u32 variant = 0;
    u32 lod[2] = {0, 0};
    f32 lo[2]  = {2.0f, 2.0f};
    f32 hi[2]  = {2.0f, 2.0f};
    f32 dist = 0.0f;
};

inline f32 Saturate(f32 v) { return std::min(1.0f, std::max(0.0f, v)); }

// 球 vs 6 平面。★Frustum::SphereVisible と同じ「完全に外」だけを棄却。
inline bool SphereInPlanes(const f32 planes[6][4], const f32 c[3], f32 r)
{
    for (int i = 0; i < 6; ++i)
    {
        const f32 d = planes[i][0] * c[0] + planes[i][1] * c[1] + planes[i][2] * c[2] + planes[i][3];
        if (d < -r) return false;
    }
    return true;
}

// 間引きの keep 確率が 0 になる「hash 値 h」の求め方の中核（HLSL と一対一）。
inline f32 ThinKeepProb(f32 dist, f32 cullDist, f32 thinStart)
{
    const f32 span = std::max((1.0f - thinStart) * cullDist, 1e-4f);
    const f32 t = Saturate((dist - thinStart * cullDist) / span);
    return (1.0f + kThinBand) * (1.0f - t);
}
inline u32 ThinHashKey(u32 seed24) { return seed24 ^ 0xA511E9B3u; }

// CPU 参照: 1 インスタンスの棄却 / LOD / フェードを決める（HZB は別関数）。
// centerOut / radiusOut はワールド球（HZB テストや診断に使う）。
inline EmitInfo EvaluateInstance(const FoliageInstance& in, const Affine34& layer, const CullParams& p,
                                 f32 centerOut[3] = nullptr, f32* radiusOut = nullptr)
{
    EmitInfo e;
    const Affine34 w = ComposeInstance(in, layer);
    const f32 wp[3] = {w.m[0][3], w.m[1][3], w.m[2][3]};
    const f32 dx = wp[0] - p.camPos[0], dy = wp[1] - p.camPos[1], dz = wp[2] - p.camPos[2];
    const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    e.dist = dist;
    const f32 s = UnpackScale(in.rotScale);
    // ワールド球: 中心 = 世界の (0, boundCenterY, 0) ローカル点。半径 = boundRadius × s × layerScale × radiusScale
    const f32 lc[3] = {0.0f, p.boundCenterY, 0.0f};
    f32 cdir[3];
    // w の 3x3 部分（スケール込み）で (0, cy, 0) を動かす
    for (int r = 0; r < 3; ++r) cdir[r] = w.m[r][1] * lc[1];
    const f32 center[3] = {wp[0] + cdir[0], wp[1] + cdir[1], wp[2] + cdir[2]};
    const f32 radius = p.boundRadius * std::fabs(s) * p.layerScale * p.radiusScale;
    if (centerOut) { centerOut[0] = center[0]; centerOut[1] = center[1]; centerOut[2] = center[2]; }
    if (radiusOut) *radiusOut = radius;

    if (dist >= p.cullDist) return e;
    if (!SphereInPlanes(p.planes, center, radius)) return e;

    const u32 seed = SeedOf(in.seedType);
    const f32 keep = ThinKeepProb(dist, p.cullDist, p.thinStart);
    const f32 h = Hash01(ThinHashKey(seed));
    const f32 b = Saturate((keep - h) / kThinBand);
    if (b <= 0.0f) return e;

    u32 variant = TypeOf(in.seedType);
    if (variant >= p.variantCount) variant = p.variantCount - 1;
    e.variant = variant;
    const u32 lc_n = std::max(1u, std::min(p.lodCount[variant], kMaxLods));

    // 名目 LOD
    u32 nominal = 0;
    for (u32 k = 0; k < 3; ++k) if (dist >= p.lodDist[k]) nominal = k + 1;
    if (p.shadowPass)
    {
        const u32 lod = std::min(nominal, lc_n - 1);
        if (lod > p.shadowMaxLod) return e;
        e.count = 1; e.lod[0] = lod; e.lo[0] = 2.0f; e.hi[0] = 2.0f;
        return e;
    }
    // クロスフェード帯の判定（帯は互いに重ならない前提: 各 k で D_k(1+f) <= D_{k+1}(1-f)）
    if (p.lodFade > 0.0f)
    {
        for (u32 k = 0; k < 3; ++k)
        {
            if (k + 1 >= lc_n) break;   // 次の LOD が無い
            const f32 D = p.lodDist[k];
            const f32 t0 = D * (1.0f - p.lodFade), t1 = D * (1.0f + p.lodFade);
            if (dist > t0 && dist < t1)
            {
                const f32 t = Saturate((dist - t0) / (t1 - t0));
                e.count = 2;
                e.lod[0] = k;     e.lo[0] = (1.0f - t) * b; e.hi[0] = 2.0f;
                e.lod[1] = k + 1; e.lo[1] = 0.0f;           e.hi[1] = 1.0f - t * b;
                return e;
            }
        }
    }
    e.count = 1;
    e.lod[0] = std::min(nominal, lc_n - 1);
    e.lo[0] = b; e.hi[0] = 2.0f;
    return e;
}

// 視錐台 6 平面（ワールド）を viewProj（行ベクトル規約 = XMFLOAT4X4）から作る。Frustum::FromViewProj と同じ。
inline void PlanesFromViewProj(const DirectX::XMFLOAT4X4& vp, f32 out[6][4])
{
    using namespace DirectX;
    const XMMATRIX m = XMMatrixTranspose(XMLoadFloat4x4(&vp));
    const XMVECTOR r0 = m.r[0], r1 = m.r[1], r2 = m.r[2], r3 = m.r[3];
    XMVECTOR pl[6] = {XMVectorAdd(r3, r0), XMVectorSubtract(r3, r0), XMVectorAdd(r3, r1),
                      XMVectorSubtract(r3, r1), r2, XMVectorSubtract(r3, r2)};
    for (int i = 0; i < 6; ++i)
    {
        pl[i] = XMPlaneNormalize(pl[i]);
        XMFLOAT4 f;
        XMStoreFloat4(&f, pl[i]);
        out[i][0] = f.x; out[i][1] = f.y; out[i][2] = f.z; out[i][3] = f.w;
    }
}

// 球 → 前フレーム HZB による遮蔽判定（CPU 参照。HiZMath.h をそのまま使う）。
// hzbAt(mip, x, y) = HZB の値。rectInflatePx = スクリーン矩形の膨らませ（disocclusion の 1 フレーム遅れの緩和）。
template <typename HzbFn>
inline bool SphereOccludedByHiZ(const f32 center[3], f32 radius, const DirectX::XMFLOAT4X4& prevVP,
                                f32 vpW, f32 vpH, u32 mipCount, f32 rectInflatePx, HzbFn hzbAt)
{
    using namespace DirectX;
    const XMVECTOR mn = XMVectorSet(center[0] - radius, center[1] - radius, center[2] - radius, 0);
    const XMVECTOR mx = XMVectorSet(center[0] + radius, center[1] + radius, center[2] + radius, 0);
    HiZViewport vp{0.0f, 0.0f, vpW, vpH};
    ScreenBounds sb = ProjectAabbToScreen(mn, mx, XMLoadFloat4x4(&prevVP), vp);
    if (!sb.valid) return false;
    sb.minX = std::max(0.0f, sb.minX - rectInflatePx);
    sb.minY = std::max(0.0f, sb.minY - rectInflatePx);
    sb.maxX = std::min(vpW, sb.maxX + rectInflatePx);
    sb.maxY = std::min(vpH, sb.maxY + rectInflatePx);
    const u32 mip = SelectHiZMip(sb, mipCount);
    const f32 inv = 1.0f / static_cast<f32>(1u << mip);
    const i32 mw = std::max(1, static_cast<i32>(vpW * inv)), mh = std::max(1, static_cast<i32>(vpH * inv));
    const i32 x0 = std::clamp(static_cast<i32>(sb.minX * inv), 0, mw - 1);
    const i32 x1 = std::clamp(static_cast<i32>(sb.maxX * inv), 0, mw - 1);
    const i32 y0 = std::clamp(static_cast<i32>(sb.minY * inv), 0, mh - 1);
    const i32 y1 = std::clamp(static_cast<i32>(sb.maxY * inv), 0, mh - 1);
    f32 tileMax = hzbAt(mip, x0, y0);
    tileMax = std::max(tileMax, hzbAt(mip, x1, y0));
    tileMax = std::max(tileMax, hzbAt(mip, x0, y1));
    tileMax = std::max(tileMax, hzbAt(mip, x1, y1));
    return IsOccludedByHiZ(sb, tileMax);
}

// ===========================================================================
// 風（FoliageWind.hlsli と一対一）
// ---------------------------------------------------------------------------
// フレーム共通（b1 相当 = PerFrame 予約 [0..1]）:
//   A = (dir.x, dir.z, speed, gustStrength)   B = (gustFreq, turbulence, time, prevTime)
// レイヤー別（b0 の末尾）:
//   bend（幹の曲げの強さ）/ flutter（葉のはばたきの振幅 m）/ bendExp（高さ指数）/ heightLocal（モデル高さ m）
// ===========================================================================
struct WindFrame
{
    f32 dirX = 1.0f, dirZ = 0.0f, speed = 0.0f, gustStrength = 0.5f;
    f32 gustFreq = 0.35f, turbulence = 0.25f, time = 0.0f, prevTime = 0.0f;
};
struct WindLayer
{
    f32 bend = 1.0f, flutter = 0.0f, bendExp = 2.0f, heightLocal = 1.0f;
};

inline f32 WFrac(f32 x) { return x - std::floor(x); }
inline f32 WHashLattice(i32 ix, i32 iy)
{
    return Hash01((static_cast<u32>(ix) * 0x8da6b343u) ^ (static_cast<u32>(iy) * 0xd8163841u) ^ 0x1b873593u);
}
// 値ノイズ 0..1（滑らか補間）
inline f32 WValueNoise2(f32 x, f32 y)
{
    const f32 fx = std::floor(x), fy = std::floor(y);
    const i32 ix = static_cast<i32>(fx), iy = static_cast<i32>(fy);
    f32 ux = x - fx, uy = y - fy;
    ux = ux * ux * (3.0f - 2.0f * ux);
    uy = uy * uy * (3.0f - 2.0f * uy);
    const f32 a = WHashLattice(ix, iy), b = WHashLattice(ix + 1, iy);
    const f32 c = WHashLattice(ix, iy + 1), d = WHashLattice(ix + 1, iy + 1);
    return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}
inline f32 WTri(f32 x) { return std::fabs(WFrac(x + 0.5f) * 2.0f - 1.0f); }

// 場所と時間での風の強さ（m/s）。突風が風下へ流れる。
inline f32 WindStrengthAt(f32 wx, f32 wz, f32 t, const WindFrame& f, f32 instPhase01)
{
    const f32 gc = (wx * f.dirX + wz * f.dirZ) * 0.08f - t * f.gustFreq;
    const f32 cr = (-wx * f.dirZ + wz * f.dirX) * 0.05f;
    const f32 gust = WValueNoise2(gc, cr);
    f32 s = f.speed * (1.0f + f.gustStrength * (gust * 2.0f - 1.0f) * 0.85f);
    const f32 turb = WValueNoise2(t * 6.0f + instPhase01 * 17.0f, instPhase01 * 5.0f + 0.37f);
    s *= 1.0f + f.turbulence * (turb - 0.5f) * 1.2f;
    return std::max(s, 0.0f);
}

// 頂点の風による変位（ワールド空間。ワールド位置に足す）。
//   wp = 風なしのワールド位置 / origin = インスタンスの原点（ワールド）/ nrm = ワールド法線（正規化）
//   localY = ローカル高さ / vc = 頂点色 (R=フラッター重み, G=位相, B=曲げ重み) / scale = インスタンスのワールドスケール
inline void WindDelta(const f32 wp[3], const f32 origin[3], const f32 nrm[3], f32 localY, const f32 vc[3],
                      f32 instPhase01, f32 windScale, f32 scale, const WindFrame& f, const WindLayer& l, f32 t,
                      f32 out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (f.speed <= 0.0f || (l.bend == 0.0f && l.flutter == 0.0f)) return;
    const f32 s = WindStrengthAt(origin[0], origin[2], t, f, instPhase01) * windScale;
    const f32 h = Saturate(localY / std::max(l.heightLocal, 1e-3f));
    const f32 osc = 0.85f + 0.15f * std::sin(t * 1.7f + instPhase01 * kTwoPi);
    // 曲げ: 風下方向へ h^exp に比例
    const f32 mag = l.bend * s * 0.01f * std::pow(h, l.bendExp) * osc * vc[2] * l.heightLocal * scale;
    const f32 perp = std::sin(t * 1.1f + instPhase01 * kTwoPi + h * 1.5f) * 0.3f * mag;
    f32 off[3] = {f.dirX * mag - f.dirZ * perp, 0.0f, f.dirZ * mag + f.dirX * perp};
    // 長さを保つ（伸びない曲げ）
    const f32 r[3] = {wp[0] - origin[0], wp[1] - origin[1], wp[2] - origin[2]};
    const f32 r2[3] = {r[0] + off[0], r[1] + off[1], r[2] + off[2]};
    const f32 l1 = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    const f32 l2 = std::max(std::sqrt(r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2]), 1e-4f);
    const f32 k = l1 / l2;
    out[0] = r2[0] * k - r[0];
    out[1] = r2[1] * k - r[1];
    out[2] = r2[2] * k - r[2];
    // はばたき（葉）: 4 本の三角波（周波数 1.975 / 0.793 / 0.375 / 0.193）を法線方向へ
    if (l.flutter != 0.0f && vc[0] > 0.0f)
    {
        const f32 ph = vc[1] * kTwoPi + instPhase01 * 3.7f;
        const f32 k1 = 3.0f;
        const f32 tri = (WTri(t * 1.975f * k1 + ph) + WTri(t * 0.793f * k1 + ph * 1.31f)
                       + WTri(t * 0.375f * k1 + ph * 1.73f) + WTri(t * 0.193f * k1 + ph * 2.11f)) * 0.25f * 2.0f - 1.0f;
        const f32 amp = l.flutter * vc[0] * scale * (0.2f + 0.8f * Saturate(s / 8.0f));
        out[0] += nrm[0] * tri * amp;
        out[1] += nrm[1] * tri * amp;
        out[2] += nrm[2] * tri * amp;
    }
}

// ===========================================================================
// ディザ（PS）。SV_Position のピクセル座標から 0..1。全パスで同じ式を使うこと（深度プリパスと本体が一致しないと穴が開く）。
// ===========================================================================
inline f32 InterleavedGradientNoise(f32 px, f32 py)
{
    return WFrac(52.9829189f * WFrac(px * 0.06711056f + py * 0.00583715f));
}

} // namespace dx12e::foliage
