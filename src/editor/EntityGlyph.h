#pragma once

// ===== エンティティの種別 → アイコングリフ + 種別カラー =====
// ヒエラルキーの行頭とインスペクタの見出し（エンティティ名）で共用する。
// 色は「控えめな色味」: メッシュは中性グレー、ライト=琥珀、カメラ=青、UI=マゼンタ、物理=緑 …
// （全部同じ濃青チップだった旧 PNG アイコンをやめ、モノクロのグリフ + 種別ごとの色にした）。

#include "editor/EditorTheme.h"
#include "editor/IconTable.h"
#include "editor/EditorIcons.h"
#include "ecs/Components.h"

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

#include <entt/entt.hpp>

namespace dx12e
{

struct EntityGlyph
{
    const char*   glyph;   // ICON_* （UTF-8 の Lucide グリフ）
    const ImVec4* tint;    // theme::Type*
};

// hasChildren: コンポーネントを持たない空の親を「グループ」（フォルダ）アイコンにする。
// 判定の実体は IconTable.h（icons::EntityId）。ここは従来の呼び出し口（glyph + テーマ色のポインタ）を保つ薄い橋渡し。
inline EntityGlyph PickEntityGlyph(entt::registry& reg, entt::entity e, bool hasChildren)
{
    const icons::KindVisual v = icons::ForEntity(reg, e, hasChildren);
    return { v.glyph, v.tint };
}

} // namespace dx12e
