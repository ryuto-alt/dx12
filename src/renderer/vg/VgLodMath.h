#pragma once
//
// VgLodMath.h ― 仮想ジオメトリの LOD 選択・錐台判定・インスタンス変換の純関数（CPU 版）。
//
//   ★GPU（shaders/vg/*.hlsl）と「同じ数式・同じ演算順」で書いてある。片方だけ直すと
//     CPU 参照（VgCullReference.h）と GPU の選択集合が食い違う。式を変えるときは必ず両方を直し、
//     tests/vg_lod_math_test.cpp / vg_cull_gpu_test.cpp を更新すること。
//   ★ヘッダオンリー・標準ライブラリのみ（D3D / DirectXMath 非依存）。tests/ の依存ゼロ構成でも動く。
//
// 規約（エンジン全体と同じ）:
//   ・行列は行ベクトル規約（p' = p * M。平行移動は 4 行目）。16 個の float は XMFLOAT4X4 と同じ行優先。
//   ・深度は標準 Z（near = 0 / far = 1）。ビューポートは左上原点。
//   ・インスタンスの world は「3 行 x 4 列」（VgInstance::world）。行 i = 4x4 行列の**列 i**
//     （出力の軸 i = dot(float4(p, 1), row_i)）。4 行目 (0,0,0,1) は省略する。
//
#include <cmath>
#include <cstdint>
#include <cstring>

#include "renderer/vg/VgShared.h"

namespace dx12e::vg
{

// ── 誤差の画面射影 ────────────────────────────────────────────────────────────
// 設計書 §2.3.2。透視のみ。projScale = 0.5 * 画面高(px) / tan(fovY / 2)。s = インスタンスの最大軸スケール。
// sphere = (center.xyz, radius) はワールド空間。球の最近点までの距離 d を zNear で下から押さえる。
// +INF の誤差（ルートの parentLodError）は +INF を返す（呼び出し側の > τ が常に真）。
inline float ProjectedErrorPx(float err, const float sphereWorld[4], const float camPos[3], float s, float projScale, float zNear)
{
    if (!(err < 3.0e38f)) return 3.4e38f;                 // +INF / 巨大値
    const float dx = sphereWorld[0] - camPos[0];
    const float dy = sphereWorld[1] - camPos[1];
    const float dz = sphereWorld[2] - camPos[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    float d = dist - sphereWorld[3];
    if (d < zNear) d = zNear;
    return err * s * projScale / d;
}

// 部分木の枝刈り用: 球の最遠点（= 中心距離 + 半径）までの距離で射影する（誤差の下界）。
inline float ProjectedErrorPxFar(float err, const float sphereWorld[4], const float camPos[3], float s, float projScale, float zNear)
{
    if (!(err < 3.0e38f)) return 3.4e38f;
    const float dx = sphereWorld[0] - camPos[0];
    const float dy = sphereWorld[1] - camPos[1];
    const float dz = sphereWorld[2] - camPos[2];
    float d = std::sqrt(dx * dx + dy * dy + dz * dz) + sphereWorld[3];
    if (d < zNear) d = zNear;
    return err * s * projScale / d;
}

// ── 錐台（6 平面）─────────────────────────────────────────────────────────────
// 平面は (nx, ny, nz, d)。内側が dot(n, p) + d >= 0。正規化済み。順序: left / right / bottom / top / near / far。
// D3D の深度 0..1 なので near は 3 列目そのもの（z >= 0）、far は w - z。
struct FrustumPlanes
{
    float p[6][4];
};

// vp = 行優先 4x4（XMFLOAT4X4 と同じ並び。行ベクトル規約: clip = float4(pos, 1) * vp）。
inline void ExtractFrustumPlanes(const float vp[16], FrustumPlanes& out)
{
    auto col = [&](int c, float o[4]) { for (int r = 0; r < 4; ++r) o[r] = vp[r * 4 + c]; };
    float c0[4], c1[4], c2[4], c3[4];
    col(0, c0); col(1, c1); col(2, c2); col(3, c3);
    for (int i = 0; i < 4; ++i)
    {
        out.p[0][i] = c3[i] + c0[i];    // left   : w + x >= 0
        out.p[1][i] = c3[i] - c0[i];    // right  : w - x >= 0
        out.p[2][i] = c3[i] + c1[i];    // bottom : w + y >= 0
        out.p[3][i] = c3[i] - c1[i];    // top    : w - y >= 0
        out.p[4][i] = c2[i];            // near   : z >= 0（D3D）
        out.p[5][i] = c3[i] - c2[i];    // far    : w - z >= 0
    }
    for (int k = 0; k < 6; ++k)
    {
        const float len = std::sqrt(out.p[k][0] * out.p[k][0] + out.p[k][1] * out.p[k][1] + out.p[k][2] * out.p[k][2]);
        if (len > 1e-20f)
        {
            const float inv = 1.0f / len;
            for (int i = 0; i < 4; ++i) out.p[k][i] *= inv;
        }
    }
}

// 球が完全に外側なら true（保守的: 迷ったら false = 見える）。
inline bool SphereOutsideFrustum(const FrustumPlanes& f, const float center[3], float radius)
{
    for (int k = 0; k < 6; ++k)
    {
        const float d = f.p[k][0] * center[0] + f.p[k][1] * center[1] + f.p[k][2] * center[2] + f.p[k][3];
        if (d < -radius) return true;
    }
    return false;
}

// ── インスタンス変換 ──────────────────────────────────────────────────────────
inline void TransformPoint34(const float rows[12], const float p[3], float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = p[0] * rows[i * 4 + 0] + p[1] * rows[i * 4 + 1] + p[2] * rows[i * 4 + 2] + rows[i * 4 + 3];
}

// 行優先 4x4（行ベクトル規約）→ 3x4 の「行 i = 列 i」表現。
inline void PackWorld34(const float m4x4[16], float rows[12])
{
    for (int i = 0; i < 3; ++i)
        for (int r = 0; r < 4; ++r) rows[i * 4 + r] = m4x4[r * 4 + i];
}

// 最大軸スケール（基底ベクトル = 行 0..2 の上 3 成分の長さの最大）。
inline float MaxAxisScale(const float m4x4[16])
{
    float best = 0.0f;
    for (int r = 0; r < 3; ++r)
    {
        const float l = std::sqrt(m4x4[r * 4] * m4x4[r * 4] + m4x4[r * 4 + 1] * m4x4[r * 4 + 1] + m4x4[r * 4 + 2] * m4x4[r * 4 + 2]);
        if (l > best) best = l;
    }
    return best;
}

inline float Det3(const float m4x4[16])
{
    const float* m = m4x4;
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}

// カメラをアセット（オブジェクト）空間へ戻す: camObj = camPos * inverse(world)。
// 平行移動込みのアフィン逆変換（3x3 の余因子行列）。特異なら false（呼び出し側は「コーン棄却しない」）。
// ★背面判定は面と視点の位置関係で、アフィン変換（非一様スケール・ミラーを含む）で保たれる。
//   だからオブジェクト空間の視点で判定すれば、ワールドの軸の符号反転や逆転置を考えなくてよい。
inline bool CameraToObjectSpace(const float m4x4[16], const float camPos[3], float out[3])
{
    const float* m = m4x4;
    const float det = Det3(m4x4);
    if (!(std::fabs(det) > 1e-30f)) return false;
    const float inv = 1.0f / det;
    // 3x3 の逆行列（行ベクトル規約: p_obj = (p_world - t) * A^-1）
    float a[9];
    a[0] = (m[5] * m[10] - m[6] * m[9]) * inv;
    a[1] = (m[2] * m[9] - m[1] * m[10]) * inv;
    a[2] = (m[1] * m[6] - m[2] * m[5]) * inv;
    a[3] = (m[6] * m[8] - m[4] * m[10]) * inv;
    a[4] = (m[0] * m[10] - m[2] * m[8]) * inv;
    a[5] = (m[2] * m[4] - m[0] * m[6]) * inv;
    a[6] = (m[4] * m[9] - m[5] * m[8]) * inv;
    a[7] = (m[1] * m[8] - m[0] * m[9]) * inv;
    a[8] = (m[0] * m[5] - m[1] * m[4]) * inv;
    const float d[3] = {camPos[0] - m[12], camPos[1] - m[13], camPos[2] - m[14]};
    for (int c = 0; c < 3; ++c) out[c] = d[0] * a[0 * 3 + c] + d[1] * a[1 * 3 + c] + d[2] * a[2 * 3 + c];
    return true;
}

// 法線コーンの背面棄却（オブジェクト空間）。axis / cutoff は coneS8 を /127 したもの、center / radius は cullSphere。
// cutoff >= 127/127 は「コーン無効」（カリングしない）。VGEO_SPEC §5.4。
inline bool ConeCulls(const float center[3], float radius, const float axis[3], float cutoff, const float camObj[3])
{
    if (cutoff >= 1.0f) return false;
    const float dx = center[0] - camObj[0], dy = center[1] - camObj[1], dz = center[2] - camObj[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float dp = dx * axis[0] + dy * axis[1] + dz * axis[2];
    return dp >= cutoff * len + radius;
}

inline float ConeAxisComponent(uint32_t coneS8, int i)
{
    const int8_t b = static_cast<int8_t>(static_cast<uint8_t>((coneS8 >> (8u * static_cast<uint32_t>(i))) & 0xFFu));
    return static_cast<float>(b) / 127.0f;
}

} // namespace dx12e::vg
