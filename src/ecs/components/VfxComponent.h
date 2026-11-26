#pragma once
// ===========================================================================
// VfxComponent（W1-BD4 / UnoFX）: .uvfx のエフェクトを 1 つ置くコンポーネント。
//   ★ecs/Components.h（135 TU が読む重ヘッダ）には足さない。このヘッダを直接 include する TU だけが使う。
//   シーンの保存 / 読み込みと MCP の Undo は scene/ComponentSerializerList.inc の登録（VfxComponentSerializer.cpp）で載る。
//   実行時の状態（シミュレーション時刻・粒子バッファの範囲・一時停止）はここに持たない
//   （Undo / 複製 / 保存の対象にしないため。renderer/vfx/VfxSystem が entity ごとに持つ）。
// ===========================================================================
#include <string>

#include "core/Types.h"

namespace dx12e
{

struct VfxComponent
{
    std::string asset;            // assets 相対の .uvfx のパス（例 "engine/vfx2/presets/p_campfire.uvfx"）
    bool        autoplay  = true; // false なら t=0 で止めて置く（vfx_seek の resume で動かす）
    float       timeScale = 1.0f; // 再生速度の倍率（0 以下は止まる）
    u32         seed      = 0;    // 乱数のシード（同じ seed と同じ時刻なら同じ絵になる）
};

} // namespace dx12e
