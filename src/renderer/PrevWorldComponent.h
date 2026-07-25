#pragma once

#include <DirectXMath.h>

namespace dx12e
{

// 速度バッファ(モーションベクター)生成用に、前フレームのワールド行列だけを保持する内部コンポーネント。
// BuildDrawList() が TAA 有効時のみ毎フレーム emplace_or_replace する。
//
// シーン JSON には保存しない（SceneSerializer は既知コンポーネントだけを見るので自動的に無視される）。
// エディタの Inspector にも出さない（ComponentMeta に登録しない）。
//
// なぜ unordered_map ではなく entt コンポーネントか:
//   10 万体規模で毎フレーム 10 万回のハッシュ探索/挿入は許容外。entt のスパースセットなら
//   O(1) の密配列アクセスで済む。エンティティ破棄→再生成による ID 再利用の世代管理も entt 任せにできる。
struct PrevWorldMatrix
{
    DirectX::XMFLOAT4X4 m;
};

} // namespace dx12e
