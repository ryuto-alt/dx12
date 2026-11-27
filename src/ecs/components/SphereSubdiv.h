#pragma once
// ===========================================================================
// SphereSubdiv（W1-C13 / BUG-45）: 球プリミティブの分割数を持つコンポーネント。
//   ★ecs/Components.h（135 TU が読む重ヘッダ）には足さない。
//   既定（16x16）の球には付けない＝旧シーンの保存形式・見た目は 1 頂点も変わらない。
//   保存 / 読み込み / 複製 / Undo は scene/ComponentSerializerList.inc の登録
//   （SphereSubdivSerializer.cpp、JSON のキーは "sphereSubdiv"）で載る。
//   読み込み時は SpawnSphere が先に 16x16 で作るため applied=false で積み、
//   フレーム境界で Scene::ApplyPendingSphereSubdiv() がメッシュを作り直す。
// ===========================================================================
#include "core/Types.h"

namespace dx12e
{

struct SphereSubdiv
{
    u32  slices  = 16;
    u32  stacks  = 16;
    bool applied = true;   // false = まだメッシュに反映していない（読み込み直後）。保存しない
};

} // namespace dx12e
