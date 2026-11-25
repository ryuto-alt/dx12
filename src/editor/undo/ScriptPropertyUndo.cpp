#include "editor/undo/ScriptPropertyUndo.h"

namespace dx12e
{

bool ScriptPropValueEquals(const ScriptProp& a, const ScriptProp& b)
{
    return a.num == b.num && a.b == b.b && a.str == b.str && a.guid == b.guid
        && a.vec.x == b.vec.x && a.vec.y == b.vec.y && a.vec.z == b.vec.z;
}

namespace
{
entt::entity Resolve(entt::registry& reg, const ScriptPropertyChange& c)
{
    if (c.entityGuid != 0)
    {
        const entt::entity f = FindEntityByGuid(reg, c.entityGuid);
        if (f != entt::null) return f;
        return entt::null;   // GUID が引けない = 消えたエンティティ。ハンドルは再利用されうるので使わない
    }
    return reg.valid(c.entityHandle) ? c.entityHandle : entt::null;
}
} // namespace

void ScriptPropertyUndo::Apply(const ScriptProp& v)
{
    if (!m_reg) return;
    const entt::entity e = Resolve(*m_reg, m_change);
    if (e == entt::null || !m_reg->all_of<LuaScript>(e)) return;
    auto& ls = m_reg->get<LuaScript>(e);
    for (auto& p : ls.props)
    {
        if (p.name != m_change.propName) continue;
        if (p.type != v.type) return;   // ホットリロードで型が変わった: 何もしない
        p.num = v.num; p.b = v.b; p.str = v.str; p.vec = v.vec; p.guid = v.guid;
        return;
    }
    // 名前が無い（ホットリロードで消えた）: 何もしない
}

bool MakeScriptPropertyChange(entt::registry& reg, entt::entity e, const std::string& name,
                              const ScriptProp& before, const ScriptProp& after,
                              ScriptPropertyChange& out)
{
    if (!reg.valid(e) || !reg.all_of<LuaScript>(e)) return false;
    if (ScriptPropValueEquals(before, after)) return false;
    out = {};
    if (const auto* g = reg.try_get<EntityGuid>(e)) out.entityGuid = g->value;
    out.entityHandle = e;
    out.propName = name;
    out.oldValue = before;
    out.newValue = after;
    return true;
}

std::unique_ptr<IUndoCommand> MakeScriptPropertiesResetCommand(entt::registry& reg, entt::entity e,
                                                               const std::vector<ScriptProp>& before)
{
    if (!reg.valid(e) || !reg.all_of<LuaScript>(e)) return nullptr;
    const auto& props = reg.get<LuaScript>(e).props;
    auto comp = std::make_unique<CompositeCommand>("Reset Script Properties");
    for (const auto& b : before)
    {
        for (const auto& a : props)
        {
            if (a.name != b.name || a.type != b.type) continue;
            ScriptPropertyChange c;
            if (MakeScriptPropertyChange(reg, e, a.name, b, a, c))
                comp->Add(std::make_unique<ScriptPropertyUndo>(&reg, std::move(c)));
            break;
        }
    }
    if (comp->Empty()) return nullptr;
    return comp;
}

} // namespace dx12e
