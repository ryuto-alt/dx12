#pragma once

// ===== ヒエラルキーの操作（Undo つき）=====
// パネルのボタン・右クリックメニュー・ショートカット・コマンドパレットが共通で呼ぶ。
//   ToggleFlag<T>   … 非表示 / ロック / フォルダを、対象すべてに同じ状態で付ける・外す（1 エントリの Undo）
//   ApplyIsolate<T> … Alt+クリック: 「これだけ残す」/ もう一度で全部降ろす
//   ApplyDrop       … ドロップ計画（親替え + 兄弟順）を適用して Undo を積む
//   CreateFolder    … 選択（無ければ空）を中に入れるフォルダを作る
// 純ロジックは HierarchyLogic.h。ここは registry + Undo に触る層（ImGui には依存しない）。

#include <memory>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "editor/EditorContext.h"
#include "editor/HierarchyCommands.h"
#include "editor/HierarchyLogic.h"
#include "editor/TransformMath.h"
#include "editor/UndoSystem.h"

namespace dx12e::hier
{

// 対象すべてを want にする。すでに want のものは触らない。積んだら true。
template <class T>
inline bool ToggleFlag(entt::registry& reg, EditorContext& ctx, const std::vector<entt::entity>& targets,
                       bool want, const char* undoName)
{
    std::vector<typename EditorFlagCommand<T>::Change> ch;
    for (entt::entity e : targets)
    {
        if (!reg.valid(e)) continue;
        const bool now = eflags::Self<T>(reg, e);
        if (now == want) continue;
        ch.push_back({e, now, want});
    }
    if (ch.empty()) return false;
    auto cmd = std::make_unique<EditorFlagCommand<T>>(&reg, std::move(ch), undoName);
    cmd->Redo();
    ctx.undoSystem.PushCommand(std::move(cmd));
    return true;
}

// Alt+クリック。今「e だけ」ならすべて降ろす / そうでなければ e だけ残す。
// 戻り値: 実際にやった動作（テストが見る）。
template <class T>
inline AltAction ApplyIsolate(entt::registry& reg, EditorContext& ctx, const Index& idx, entt::entity e, const char* undoName)
{
    const AltAction act = AltClickAction<T>(reg, idx, e);
    std::vector<typename EditorFlagCommand<T>::Change> ch;
    if (act == AltAction::ClearAll)
    {
        for (entt::entity x : PlanClearAll<T>(reg)) ch.push_back({x, true, false});
    }
    else
    {
        const FlagPlan<T> plan = PlanIsolate<T>(reg, idx, e);
        for (entt::entity x : plan.clear) ch.push_back({x, true, false});
        for (entt::entity x : plan.set)   ch.push_back({x, false, true});
    }
    if (!ch.empty())
    {
        auto cmd = std::make_unique<EditorFlagCommand<T>>(&reg, std::move(ch), undoName);
        cmd->Redo();
        ctx.undoSystem.PushCommand(std::move(cmd));
    }
    return act;
}

// ドロップ計画を適用する。keepWorld=true ならワールド位置を保って親を替える（Shift でローカル値そのまま）。
// 触った全エンティティの Transform（parent / siblingOrder / 位置…）を before / after で 1 エントリの Undo に積む。
// 戻り値: 何か変わったか。
inline bool ApplyDrop(entt::registry& reg, EditorContext& ctx, const DropPlan& plan, bool keepWorld)
{
    if (!plan.ok) return false;
    std::vector<std::pair<entt::entity, Transform>> before;
    auto remember = [&](entt::entity e) {
        for (const auto& p : before) if (p.first == e) return;
        before.emplace_back(e, reg.get<Transform>(e));
    };
    for (entt::entity m : plan.moving) remember(m);
    for (const OrderChange& o : plan.orders) remember(o.e);

    for (entt::entity m : plan.moving)
        if (reg.get<Transform>(m).parent != plan.newParent)
            xform::Reparent(reg, m, plan.newParent, keepWorld);
    for (const OrderChange& o : plan.orders)
        reg.get<Transform>(o.e).siblingOrder = o.order;

    auto composite = std::make_unique<CompositeCommand>("Reparent");
    for (const auto& [e, b] : before)
    {
        const Transform& a = reg.get<Transform>(e);
        const bool changed = b.parent != a.parent || b.siblingOrder != a.siblingOrder
            || b.position.x != a.position.x || b.position.y != a.position.y || b.position.z != a.position.z
            || b.rotation.x != a.rotation.x || b.rotation.y != a.rotation.y || b.rotation.z != a.rotation.z
            || b.scale.x != a.scale.x || b.scale.y != a.scale.y || b.scale.z != a.scale.z;
        if (changed) composite->Add(std::make_unique<TransformCommand>(&reg, e, b, a));
    }
    if (composite->Empty()) return false;
    ctx.undoSystem.PushCommand(std::move(composite));
    return true;
}

// フォルダを作る。members の最上位（祖先も選ばれている子は除く）を新しいフォルダの子にする。
// フォルダの親は、メンバーの元の親が全員同じならそこ（階層の位置を保つ）。Transform は原点・無回転・スケール 1 なので
// 子のワールド位置は動かない。戻り値は作ったフォルダ（作れなければ null）。
//   parentOverride が null でなければ、そこを親にして（メンバーの元の親は見ずに）空のフォルダを作る（右クリック「子を作成 > フォルダ」用）。
inline entt::entity CreateFolder(entt::registry& reg, EditorContext& ctx, const std::vector<entt::entity>& selection,
                                 const char* name = "Folder", entt::entity parentOverride = entt::null)
{
    auto ancestorSelected = [&](entt::entity e) {
        const auto* t = reg.try_get<Transform>(e);
        entt::entity cur = t ? t->parent : entt::null;
        for (int d = 0; cur != entt::null && reg.valid(cur) && d < 4096; ++d)
        {
            if (std::find(selection.begin(), selection.end(), cur) != selection.end()) return true;
            const auto* pt = reg.try_get<Transform>(cur);
            cur = pt ? pt->parent : entt::null;
        }
        return false;
    };
    std::vector<std::pair<entt::entity, entt::entity>> members;   // (子, 元の親)
    for (entt::entity e : selection)
    {
        if (!reg.valid(e) || !reg.all_of<Transform>(e) || reg.all_of<GridPlane>(e)) continue;
        if (ancestorSelected(e)) continue;
        members.emplace_back(e, reg.get<Transform>(e).parent);
    }
    entt::entity commonParent = entt::null;
    bool sameParent = false;
    if (parentOverride != entt::null && reg.valid(parentOverride) && reg.all_of<Transform>(parentOverride))
    {
        commonParent = parentOverride;
        sameParent = true;
    }
    else if (!members.empty())
    {
        commonParent = members.front().second;
        sameParent = std::all_of(members.begin(), members.end(), [&](const auto& m) { return m.second == commonParent; });
    }
    const entt::entity folder = reg.create();
    reg.emplace<NameTag>(folder, NameTag{name});
    Transform ft{};
    if (sameParent && commonParent != entt::null && reg.valid(commonParent)) ft.parent = commonParent;
    if (sameParent && !members.empty() && parentOverride == entt::null)
        ft.siblingOrder = reg.get<Transform>(members.front().first).siblingOrder;   // 元の位置あたりに置く
    reg.emplace<Transform>(folder, ft);
    reg.emplace<EditorFolder>(folder);
    for (const auto& [child, oldParent] : members)
    {
        (void)oldParent;
        reg.get<Transform>(child).parent = folder;
    }
    ctx.undoSystem.PushCommand(std::make_unique<FolderCommand>(
        &reg, name, folder, sameParent ? commonParent : entt::null, ft.siblingOrder, std::move(members)));
    return folder;
}

// 空の子を 1 つ作る（右クリック「子を作成」）。
inline entt::entity CreateEmptyChild(entt::registry& reg, EditorContext& ctx, entt::entity parent, const char* name = "Empty")
{
    if (parent != entt::null && (!reg.valid(parent) || !reg.all_of<Transform>(parent))) return entt::null;
    const entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{name});
    Transform t{};
    t.parent = parent;
    reg.emplace<Transform>(e, t);
    ctx.undoSystem.PushCommand(std::make_unique<CreateEmptyCommand>(&reg, name, e, parent));
    return e;
}

} // namespace dx12e::hier
