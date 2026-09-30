#pragma once

// ===== エディタ専用フラグのヘルパ（フェーズ 1b）=====
// データ型は ecs/Components.h の EditorHidden / EditorLocked / EditorFolder / EntityDisabled（中身の無いタグ型）。
// ・「祖先のどれかが立っている」を実効値とする（親を隠すと子も隠れる）。
// ・1 個も立っていないシーンでは storage の size() 判定だけで即 false（10 万体の描画リスト構築でも実質タダ）。
// ・純ロジック（entt::registry だけに依存）。tests/ から直接呼べる。

#include <entt/entt.hpp>
#include "ecs/Components.h"

namespace dx12e::eflags
{

// このシーンに T が 1 個でも立っているか（描画リスト構築の早期 return 用）。
template <class T>
inline bool AnyOf(const entt::registry& reg)
{
    const auto* s = reg.storage<T>();
    return s != nullptr && s->size() > 0;
}

// e 自身か、祖先のどれかに T が立っているか。壊れた親鎖でも止まる（深さ上限つき）。
template <class T>
inline bool SelfOrAncestor(const entt::registry& reg, entt::entity e)
{
    const auto* s = reg.storage<T>();
    if (s == nullptr || s->size() == 0) return false;
    for (int depth = 0; e != entt::null && reg.valid(e) && depth < 4096; ++depth)
    {
        if (s->contains(e)) return true;
        const auto* t = reg.try_get<Transform>(e);
        e = t ? t->parent : entt::null;
    }
    return false;
}

inline bool IsHidden(const entt::registry& reg, entt::entity e)   { return SelfOrAncestor<EditorHidden>(reg, e); }
inline bool IsLocked(const entt::registry& reg, entt::entity e)   { return SelfOrAncestor<EditorLocked>(reg, e); }
inline bool IsDisabled(const entt::registry& reg, entt::entity e) { return SelfOrAncestor<EntityDisabled>(reg, e); }

// e 自身にだけ立っているか（ヒエラルキーの目 / 鍵アイコンの「自分の状態」と「親から継承した状態」を描き分けるため）。
template <class T>
inline bool Self(const entt::registry& reg, entt::entity e)
{
    const auto* s = reg.storage<T>();
    return s != nullptr && s->contains(e);
}

} // namespace dx12e::eflags
