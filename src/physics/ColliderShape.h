#pragma once

#include "core/Types.h"

#include <DirectXMath.h>
#include <algorithm>

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

} // namespace dx12e::collider
