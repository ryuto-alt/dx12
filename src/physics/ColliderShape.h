#pragma once

#include "core/Types.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <vector>

// ===== 当たり判定の「実効サイズ」の唯一の規約 =====
//
// コライダーとトリガーの大きさは、コンポーネントに書いた値そのものではなく
// **Transform のワールドスケールを掛けたもの**が実際の判定に使われる。
// 掛け方は形ごとに違う（箱は成分ごと / 球は最大成分 / カプセルは軸で別）。
//
// ★この規則が【当たり判定を作る側】と【それを線で描く側】で食い違うと、
//   デバッグ表示が嘘をつく。「見えている線と実際に当たる場所が違う」は
//   デバッグ機能として最悪の壊れ方で、しかも見ただけでは気づけない。
//   （実際、以前の PhysicsDebugRenderer はスケールもオフセットも無視していたため、
//     SpawnBox → scale で拡大した床が、線だけ 0.5 半径のままだった）
//
// だから規則はここ 1 箇所に置き、使う側は全部ここを通す:
//   - PhysicsSystem.cpp        … Jolt の Shape を作るとき
//   - PhysicsDebugRenderer.cpp … 線を描くとき
//   - ScriptEngine.cpp         … Trigger の内外判定
// tests/collider_shape_test.cpp が規則そのものを固定している。
namespace dx12e::collider
{

// 箱: ハーフサイズへ成分ごとにスケールを掛ける。
// これが無いと「見た目だけ拡大した床」が既定の 0.5 半径でしか衝突しない。
inline DirectX::XMFLOAT3 BoxHalfExtents(const DirectX::XMFLOAT3& halfExtents,
                                        const DirectX::XMFLOAT3& scale)
{
    return { halfExtents.x * scale.x, halfExtents.y * scale.y, halfExtents.z * scale.z };
}

// 球: スケールの最大成分だけを使う（潰れた球は作れないので、めり込むより大きい方に倒す）。
inline f32 SphereRadius(f32 radius, const DirectX::XMFLOAT3& scale)
{
    return radius * (std::max)({ scale.x, scale.y, scale.z });
}

// カプセル: 半径は XZ の最大、高さは Y。軸は Y 固定。
inline f32 CapsuleRadius(f32 radius, const DirectX::XMFLOAT3& scale)
{
    return radius * (std::max)(scale.x, scale.z);
}
inline f32 CapsuleHalfHeight(f32 halfHeight, const DirectX::XMFLOAT3& scale)
{
    return halfHeight * scale.y;
}

// コライダー部品を何も持たないとき: Transform のスケールそのものを箱にする。
inline DirectX::XMFLOAT3 FallbackHalfExtents(const DirectX::XMFLOAT3& scale)
{
    return { scale.x * 0.5f, scale.y * 0.5f, scale.z * 0.5f };
}

// ---- Trigger（物理ではなく ScriptEngine が自前で内外判定する）----
// 規約はコライダーと同じ（箱は成分ごと / 球は最大成分）。
inline DirectX::XMFLOAT3 TriggerBoxHalfExtents(const DirectX::XMFLOAT3& halfExtents,
                                               const DirectX::XMFLOAT3& scale)
{
    return BoxHalfExtents(halfExtents, scale);
}
inline f32 TriggerSphereRadius(f32 radius, const DirectX::XMFLOAT3& scale)
{
    return SphereRadius(radius, scale);
}

// ===== 物理が実際に使う「安全な」実効サイズ（2026-10 追加）=====
//
// 上の BoxHalfExtents などは「素の掛け算」で、負のスケール・0・NaN をそのまま通す。
// Jolt の形状は半径 / ハーフサイズが正であることを前提にしているので、負や 0 を渡すと
// 球・カプセルが床を抜け続ける（実測: scale -1 の球は y=-26.8 まで落ちた）・箱が浮く・
// （デバッグビルドでは assert）になる。物理とデバッグ描画はこちら（Effective*）を使う。
//   ・符号は捨てる（箱 / 球 / カプセルは軸対称なので、鏡像でも形は同じ。鏡像でずれるのは offset だけで、
//     それは ScaledOffset が符号つきの scale を掛けて表す）。
//   ・下限 kMinColliderSize（1mm）。0 / NaN は下限へ、上限 kMaxColliderSize。
constexpr f32 kMinColliderSize = 0.001f;
constexpr f32 kMaxColliderSize = 1.0e6f;

inline f32 SafeSize(f32 v)
{
    const f32 a = std::fabs(v);
    if (!(a >= kMinColliderSize)) return kMinColliderSize;   // 0 / NaN / 極小
    return a > kMaxColliderSize ? kMaxColliderSize : a;
}

inline DirectX::XMFLOAT3 EffectiveBoxHalfExtents(const DirectX::XMFLOAT3& halfExtents,
                                                 const DirectX::XMFLOAT3& scale)
{
    return { SafeSize(halfExtents.x * scale.x), SafeSize(halfExtents.y * scale.y),
             SafeSize(halfExtents.z * scale.z) };
}
inline f32 EffectiveSphereRadius(f32 radius, const DirectX::XMFLOAT3& scale)
{
    return SafeSize(radius * (std::max)({ std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z) }));
}
inline f32 EffectiveCapsuleRadius(f32 radius, const DirectX::XMFLOAT3& scale)
{
    return SafeSize(radius * (std::max)(std::fabs(scale.x), std::fabs(scale.z)));
}
inline f32 EffectiveCapsuleHalfHeight(f32 halfHeight, const DirectX::XMFLOAT3& scale)
{
    return SafeSize(halfHeight * scale.y);
}
inline DirectX::XMFLOAT3 EffectiveFallbackHalfExtents(const DirectX::XMFLOAT3& scale)
{
    return { SafeSize(scale.x * 0.5f), SafeSize(scale.y * 0.5f), SafeSize(scale.z * 0.5f) };
}
// Jolt の BoxShape は「ハーフサイズ >= 角の丸め半径(既定 0.05)」が前提。薄い板（半厚 0.01 など）で
// 既定の半径を渡すと芯が裏返る（debug では assert）ので、最小ハーフサイズに合わせて丸め半径を下げる。
inline f32 BoxConvexRadius(const DirectX::XMFLOAT3& he, f32 defaultRadius = 0.05f)
{
    return (std::min)({ defaultRadius, he.x, he.y, he.z });
}

// コライダーの offset は「エンティティのローカル空間」で解釈する（Unity の Collider.center と同じ）。
// ＝ワールドへは 回転 と スケール を通る。ボディの原点はエンティティの原点に固定し、offset は形状の内側へ入れる
// （Jolt の RotatedTranslatedShape）ので、剛体の回転軸・Transform への書き戻し・physics:setPosition が全部揃う。
// 符号つきの scale を掛ける（負スケール = 鏡像では offset も反対側へ行く）。
inline DirectX::XMFLOAT3 ScaledOffset(const DirectX::XMFLOAT3& offset, const DirectX::XMFLOAT3& scale)
{
    return { offset.x * scale.x, offset.y * scale.y, offset.z * scale.z };
}

// メッシュコライダーなどへ渡す ScaledShape 用のスケール。符号は残す（鏡像は Jolt が扱う）。
// 0 / NaN の軸は 1mm へ（0 のまま渡すと IsValidScale が偽 → 原寸の形が出る「見えない壁」になっていた）。
inline DirectX::XMFLOAT3 SafeMeshScale(const DirectX::XMFLOAT3& s)
{
    auto one = [](f32 v) -> f32 {
        if (!(std::fabs(v) >= kMinColliderSize)) return kMinColliderSize;   // 0 / NaN
        if (std::fabs(v) > kMaxColliderSize) return v < 0.0f ? -kMaxColliderSize : kMaxColliderSize;
        return v;
    };
    return { one(s.x), one(s.y), one(s.z) };
}

// ===== RigidBody の数値の検証（Jolt が壊れる値を入口で潰す）=====
// 質量 0 / 負 / NaN の動的剛体は Jolt の質量計算が FLT_INVALID_OPERATION でプロセスごと落ちる
// （静的で作った剛体を Inspector で動的へ切り替えるだけで成立していた）。負の摩擦も同様。
constexpr f32 kDefaultBodyMass = 1.0f;
inline f32 SafeMass(f32 m)
{
    if (!(m > 0.0f)) return kDefaultBodyMass;            // 0 / 負 / NaN
    if (m < 1.0e-6f) return 1.0e-6f;
    return m > 1.0e12f ? 1.0e12f : m;
}
inline f32 SafeFriction(f32 f, f32 fallback = 0.3f)
{
    if (!(f == f)) return fallback;                       // NaN
    return f < 0.0f ? 0.0f : (f > 1000.0f ? 1000.0f : f);
}
inline f32 SafeRestitution(f32 r, f32 fallback = 0.0f)
{
    if (!(r == r)) return fallback;
    return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
}
inline f32 SafeDamping(f32 d, f32 fallback = 0.0f)
{
    if (!(d == d)) return fallback;
    return d < 0.0f ? 0.0f : (d > 1000.0f ? 1000.0f : d);
}

// ===== ワールド行列の分解（せん断・0 スケールでも必ず値を返す）=====
// XMMatrixDecompose は、非一様スケールの親 + 回転した子（= せん断を含む行列）や 0 スケールで失敗する。
// 失敗時に既定値（原点・恒等）を返していたせいで、そういう子の判定が世界の原点に出ていた。
// 失敗したら 位置 = 行 3、スケール = 各行の長さ、回転 = グラム・シュミット直交化（せん断は捨てる）で近似する。
struct WorldDecomposed
{
    DirectX::XMFLOAT3 pos{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 rot{ 0.0f, 0.0f, 0.0f, 1.0f };
    DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
    bool              approximated = false;   // せん断を捨てた近似
};

inline WorldDecomposed DecomposeWorld(const DirectX::XMMATRIX& m)
{
    using namespace DirectX;
    WorldDecomposed out;
    XMVECTOR s, q, p;
    if (XMMatrixDecompose(&s, &q, &p, m))
    {
        XMStoreFloat3(&out.pos, p);
        XMStoreFloat3(&out.scale, s);
        XMStoreFloat4(&out.rot, q);
        const bool finite = std::isfinite(out.pos.x) && std::isfinite(out.pos.y) && std::isfinite(out.pos.z)
                         && std::isfinite(out.rot.x) && std::isfinite(out.rot.y) && std::isfinite(out.rot.z)
                         && std::isfinite(out.rot.w);
        if (finite) return out;
    }

    out.approximated = true;
    XMFLOAT4X4 f;
    XMStoreFloat4x4(&f, m);
    out.pos = { f._41, f._42, f._43 };
    const XMVECTOR X = XMVectorSet(f._11, f._12, f._13, 0.0f);
    const XMVECTOR Y = XMVectorSet(f._21, f._22, f._23, 0.0f);
    const XMVECTOR Z = XMVectorSet(f._31, f._32, f._33, 0.0f);
    const f32 sx = XMVectorGetX(XMVector3Length(X));
    const f32 sy = XMVectorGetX(XMVector3Length(Y));
    f32 sz = XMVectorGetX(XMVector3Length(Z));
    constexpr f32 eps = 1.0e-6f;

    const XMVECTOR ex = sx > eps ? XMVectorScale(X, 1.0f / sx) : XMVectorSet(1, 0, 0, 0);
    XMVECTOR yo = XMVectorSubtract(Y, XMVectorScale(ex, XMVectorGetX(XMVector3Dot(Y, ex))));
    f32 yl = XMVectorGetX(XMVector3Length(yo));
    if (!(yl > eps))
    {
        // Y が X と平行 / 0: X に直交する任意の軸を作る
        const XMVECTOR a = std::fabs(XMVectorGetY(ex)) < 0.9f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(0, 0, 1, 0);
        yo = XMVectorSubtract(a, XMVectorScale(ex, XMVectorGetX(XMVector3Dot(a, ex))));
        yl = XMVectorGetX(XMVector3Length(yo));
    }
    const XMVECTOR ey = XMVectorScale(yo, 1.0f / yl);
    const XMVECTOR ez = XMVector3Cross(ex, ey);
    if (XMVectorGetX(XMVector3Dot(Z, ez)) < 0.0f) sz = -sz;   // 鏡像（負スケール）

    XMMATRIX r = XMMatrixIdentity();
    r.r[0] = ex; r.r[1] = ey; r.r[2] = ez;
    XMStoreFloat4(&out.rot, XMQuaternionNormalize(XMQuaternionRotationMatrix(r)));
    out.scale = { sx, sy, sz };
    return out;
}

// ===== 凸包の頂点の間引き（ConvexHullCollider::points を maxN 個以下にする）=====
// 以前は「step おきの頂点」を取るだけで、凸包の外周（極値の頂点）を保てなかった（実測: 7,925 頂点のシャンデリアで
// 凸包が描画 AABB より片側 2.4cm 小さい）。今は【方向ごとの極値の頂点】を残す: 6 軸 + フィボナッチ球の方向で
// その方向へ最も遠い頂点を選ぶ（凸包の頂点はいずれかの方向の極値になるので外形が保たれる）。足りない分は等間隔で埋める。
// 決定論（同じ入力 → 同じ出力）。読み込み時 / autoCollider / Inspector が使う。maxN 以下なら入力をそのまま返す。
inline std::vector<DirectX::XMFLOAT3> ReduceHullPoints(const std::vector<DirectX::XMFLOAT3>& pts, size_t maxN = 256)
{
    if (pts.size() <= maxN || maxN < 16) return pts.size() <= maxN ? pts : std::vector<DirectX::XMFLOAT3>(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(maxN));
    std::vector<DirectX::XMFLOAT3> dirs = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
    const size_t nd = maxN - maxN / 4;
    for (size_t i = 0; i < nd; ++i)
    {
        const f32 y = 1.0f - 2.0f * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(nd);
        const f32 r = std::sqrt((std::max)(0.0f, 1.0f - y * y));
        const f32 phi = static_cast<f32>(i) * 2.3999632f;   // 黄金角
        dirs.push_back({ std::cos(phi) * r, y, std::sin(phi) * r });
    }
    std::vector<char> pick(pts.size(), 0);
    size_t count = 0;
    for (const auto& d : dirs)
    {
        size_t best = 0; f32 bv = -1e30f;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const f32 v = pts[i].x * d.x + pts[i].y * d.y + pts[i].z * d.z;
            if (v > bv) { bv = v; best = i; }
        }
        if (!pick[best]) { pick[best] = 1; ++count; }
    }
    // 残りは等間隔で埋める（極値だけだと面の途中の頂点が薄くなる）
    const size_t step = (std::max<size_t>)(1, pts.size() / maxN);
    for (size_t i = 0; i < pts.size() && count < maxN; i += step)
        if (!pick[i]) { pick[i] = 1; ++count; }
    std::vector<DirectX::XMFLOAT3> out;
    out.reserve(count);
    for (size_t i = 0; i < pts.size(); ++i) if (pick[i]) out.push_back(pts[i]);
    return out;
}

// ---- Trigger の内外判定（物理ではなく ScriptEngine が行う。規約は物理と同じ「ローカル + スケール」）----
// world = Trigger のワールド行列（親・回転・スケール込み）。point = 判定する点（ワールド）。
// 点を world の逆行列でローカルへ引いて半幅と比べるので、回転・親のスケール・負スケールが全部効く。
// （以前は「ワールド位置 + offset を軸平行の箱」で比べていたので、回した箱は回らず、負スケールは常に外だった）
inline bool TriggerBoxContains(const DirectX::XMFLOAT3& halfExtents, const DirectX::XMFLOAT3& offset,
                               const DirectX::XMMATRIX& world, const DirectX::XMFLOAT3& point)
{
    using namespace DirectX;
    XMVECTOR det;
    const XMMATRIX inv = XMMatrixInverse(&det, world);
    if (!(std::fabs(XMVectorGetX(det)) > 1.0e-20f)) return false;   // スケール 0（つぶれた箱）: 何も入らない
    const XMVECTOR l = XMVector3TransformCoord(XMLoadFloat3(&point), inv);
    const f32 dx = XMVectorGetX(l) - offset.x, dy = XMVectorGetY(l) - offset.y, dz = XMVectorGetZ(l) - offset.z;
    return std::fabs(dx) <= std::fabs(halfExtents.x)
        && std::fabs(dy) <= std::fabs(halfExtents.y)
        && std::fabs(dz) <= std::fabs(halfExtents.z);
}
// 球: 中心 = world * offset、半径 = radius × 各軸スケールの最大（物理の球と同じ規約）。
inline bool TriggerSphereContains(f32 radius, const DirectX::XMFLOAT3& offset,
                                  const DirectX::XMMATRIX& world, const DirectX::XMFLOAT3& point)
{
    using namespace DirectX;
    const XMVECTOR c = XMVector3TransformCoord(XMLoadFloat3(&offset), world);
    const f32 sx = XMVectorGetX(XMVector3Length(world.r[0]));
    const f32 sy = XMVectorGetX(XMVector3Length(world.r[1]));
    const f32 sz = XMVectorGetX(XMVector3Length(world.r[2]));
    const f32 r  = std::fabs(radius) * (std::max)({ sx, sy, sz });
    const XMVECTOR d = XMVectorSubtract(XMLoadFloat3(&point), c);
    return XMVectorGetX(XMVector3LengthSq(d)) <= r * r;
}

} // namespace dx12e::collider
