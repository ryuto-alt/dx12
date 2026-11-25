#pragma once

// Inspector の Lua 公開プロパティ編集を Undo/Redo できるようにするコマンド（W05-14 / BUG-11）。
//
// ・1 回のドラッグ（アクティブになった時点の値 → 離した時点の値）で 1 件。毎フレームは積まない。
// ・「既定値に戻す」は全プロパティを 1 件の複合コマンドにする（MakeScriptPropertiesResetCommand）。
// ・エンティティは GUID で引く（Play/Stop でハンドルが変わっても追従する）。GUID が 0（保存前）のときだけ
//   記録時のハンドルで引く。
// ・スクリプトのホットリロードでプロパティ名や型が変わっていたら、黙って何もしない（例外を出さない）。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "ecs/Components.h"
#include "editor/UndoCore.h"

namespace dx12e
{

// 値の入れ替えだけを表す 1 件分。oldValue / newValue は名前と型の一致する ScriptProp。
struct ScriptPropertyChange
{
    uint64_t   entityGuid  = 0;                    // 0 = 未設定（保存前のエンティティ。entityHandle で引く）
    entt::entity entityHandle = entt::null;        // GUID が 0 のときのフォールバック
    size_t     scriptIndex = 0;                    // 1 エンティティに付く LuaScript は 1 つ。0 固定（将来の複数対応用）
    std::string propName;
    ScriptProp oldValue;
    ScriptProp newValue;
};

class ScriptPropertyUndo : public IUndoCommand
{
public:
    ScriptPropertyUndo(entt::registry* reg, ScriptPropertyChange change)
        : m_reg(reg), m_change(std::move(change)) {}

    void Undo() override { Apply(m_change.oldValue); }
    void Redo() override { Apply(m_change.newValue); }
    const char* GetName() const override { return "Script Property"; }

    const ScriptPropertyChange& Change() const { return m_change; }

private:
    void Apply(const ScriptProp& v);
    entt::registry*      m_reg = nullptr;
    ScriptPropertyChange m_change;
};

// 値の保存対象が同じか（Float/Int の num、Bool の b、String/Entity の str と guid、Vec3/Color の vec）。
bool ScriptPropValueEquals(const ScriptProp& a, const ScriptProp& b);

// e の LuaScript から name のプロパティを探して Change を作る。見つからない / 値が同じなら false。
bool MakeScriptPropertyChange(entt::registry& reg, entt::entity e, const std::string& name,
                              const ScriptProp& before, const ScriptProp& after,
                              ScriptPropertyChange& out);

// 「既定値に戻す」: before（全プロパティの変更前）と現在の LuaScript::props を比べ、差のあるものを
// 1 件の複合コマンドにまとめる。差が無ければ nullptr。
std::unique_ptr<IUndoCommand> MakeScriptPropertiesResetCommand(entt::registry& reg, entt::entity e,
                                                               const std::vector<ScriptProp>& before);

} // namespace dx12e
