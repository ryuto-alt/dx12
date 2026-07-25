#pragma once

// レイ交差判定の純関数群。DirectXMath 以外に依存しない（GPU も entt も ImGui も要らない）ので
// そのまま単体テストできる（tests/ray_pick_test.cpp）。
// editor/ScenePick.cpp（シーンビューの精密ピッキング）が使う実体。

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace dx12e::raygeo
{

// これ未満の成分は「レイが軸/面と平行」とみなす閾値。
inline constexpr float kParallelEps = 1e-12f;

// 除数を符号ごとに kParallelEps でクランプする。
// 呼び出し側が定数 0 の方向ベクトル（例: dir=(0,0,1) の x 成分）を渡すと、
// 直前の早期リターンで到達しないはずの除算でも MSVC は定数畳み込み後の
// 「0 除算」を見て C4723 を出し、/WX でビルドが止まる。到達可能な経路では
// |v| >= kParallelEps が保証済み＝値は一切変わらない、警告封じ専用のクランプ。
inline float SafeDivisor(float v)
{
    return (v >= 0.0f) ? (std::max)(v, kParallelEps) : (std::min)(v, -kParallelEps);
}

// レイ vs 三角形（Möller–Trumbore, 両面）。ヒットで t(>0)、外れ/平行なら -1。
//
// dir は正規化されている必要はない。ワールドのレイを「メッシュのワールド行列の逆行列」で
// ローカル空間へ落とし、方向を正規化せずに渡せば、返る t はワールド空間の t と一致する
// （p = o + t*d は線形変換で保たれる）。これが複数メッシュのヒットを t で直接比較できる理由。
inline float RayTriangle(DirectX::XMVECTOR orig, DirectX::XMVECTOR dir,
                         DirectX::XMVECTOR v0, DirectX::XMVECTOR v1, DirectX::XMVECTOR v2)
{
    using namespace DirectX;
    const XMVECTOR e1 = XMVectorSubtract(v1, v0);
    const XMVECTOR e2 = XMVectorSubtract(v2, v0);
    const XMVECTOR p  = XMVector3Cross(dir, e2);
    const float det = XMVectorGetX(XMVector3Dot(e1, p));
    if (std::abs(det) < kParallelEps) return -1.0f;    // レイが三角形と平行
    const float inv = 1.0f / SafeDivisor(det);
    const XMVECTOR tv = XMVectorSubtract(orig, v0);
    const float u = XMVectorGetX(XMVector3Dot(tv, p)) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const XMVECTOR q = XMVector3Cross(tv, e1);
    const float v = XMVectorGetX(XMVector3Dot(dir, q)) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    const float t = XMVectorGetX(XMVector3Dot(e2, q)) * inv;
    return t > 0.0f ? t : -1.0f;
}

// レイ vs 軸平行 AABB（スラブ法）。ヒットで入口 t（レイ原点が内側なら出口 t）、外れは -1。
// bmin/bmax は入れ替わっていても良い（負スケールをローカル空間へ持ち込んだ時の保険）。
inline float RayAabb(const DirectX::XMFLOAT3& o, const DirectX::XMFLOAT3& d,
                     DirectX::XMFLOAT3 bmin, DirectX::XMFLOAT3 bmax)
{
    if (bmin.x > bmax.x) std::swap(bmin.x, bmax.x);
    if (bmin.y > bmax.y) std::swap(bmin.y, bmax.y);
    if (bmin.z > bmax.z) std::swap(bmin.z, bmax.z);

    float tmin = -3.402823466e+38F, tmax = 3.402823466e+38F;
    auto slab = [&](float ro, float rd, float lo, float hi) -> bool {
        if (std::abs(rd) < kParallelEps)
            return (ro >= lo && ro <= hi);   // 軸平行: 範囲内かどうかだけで判定
        const float inv = 1.0f / SafeDivisor(rd);
        float t1 = (lo - ro) * inv;
        float t2 = (hi - ro) * inv;
        if (t1 > t2) std::swap(t1, t2);
        tmin = (std::max)(tmin, t1);
        tmax = (std::min)(tmax, t2);
        return tmin <= tmax;
    };
    if (slab(o.x, d.x, bmin.x, bmax.x)
        && slab(o.y, d.y, bmin.y, bmax.y)
        && slab(o.z, d.z, bmin.z, bmax.z)
        && tmax > 0.0f)
    {
        const float t = tmin > 0.0f ? tmin : tmax;
        return t > 0.0f ? t : -1.0f;
    }
    return -1.0f;
}

// レイ vs 球。ヒットで手前の交点 t（原点が球内なら 0）、外れは -1。
// ブロードフェーズ専用なので dir は正規化済み前提。
inline float RaySphere(const DirectX::XMFLOAT3& o, const DirectX::XMFLOAT3& d,
                       const DirectX::XMFLOAT3& c, float r)
{
    const float mx = o.x - c.x, my = o.y - c.y, mz = o.z - c.z;
    const float b = mx * d.x + my * d.y + mz * d.z;
    const float cc = mx * mx + my * my + mz * mz - r * r;
    if (cc <= 0.0f) return 0.0f;            // 原点が球の内側
    if (b > 0.0f) return -1.0f;             // 球は完全に後方
    const float disc = b * b - cc;
    if (disc < 0.0f) return -1.0f;
    const float t = -b - std::sqrt(disc);
    return t >= 0.0f ? t : -1.0f;
}

// ヒット列を距離の昇順に並べる。同距離は元の順序を保つ（stable）ので、
// 同じ t のサブメッシュが列挙順どおりに並び、循環選択の順番が毎回ブレない。
// T は float 型のメンバ distance を持つこと。
template <class T>
inline void SortHitsByDistance(std::vector<T>& hits)
{
    std::stable_sort(hits.begin(), hits.end(),
                     [](const T& a, const T& b) { return a.distance < b.distance; });
}

} // namespace dx12e::raygeo
