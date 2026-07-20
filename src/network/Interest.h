#pragma once
#include "core/Types.h"

#include <DirectXMath.h>

namespace dx12e {

// 興味管理(Interest management): observer(接続の視点=通常はそのクライアントの所有エンティティ位置)
// から candidate までの距離が interestRadius 以内かどうかを判定する。
// interestRadius<=0 は「常に関連」(距離カリングなし)。ヘッダオンリーの純関数(単体テスト対象)。
inline bool IsRelevant(const DirectX::XMFLOAT3& observerPos, const DirectX::XMFLOAT3& candidatePos,
                       f32 interestRadius)
{
    if (interestRadius <= 0.0f) return true;
    const f32 dx = observerPos.x - candidatePos.x;
    const f32 dy = observerPos.y - candidatePos.y;
    const f32 dz = observerPos.z - candidatePos.z;
    return (dx * dx + dy * dy + dz * dz) <= (interestRadius * interestRadius);
}

} // namespace dx12e
