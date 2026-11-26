#include "scripting/LuaBindingRegistry.h"

namespace dx12e
{

#define UNO_LUA_BIND(Fn) void Fn(sol::state& lua, ScriptContext& ctx);
#include "scripting/LuaBindList.inc"
#undef UNO_LUA_BIND

void RunAllLuaBindings(sol::state& lua, ScriptContext& ctx)
{
    (void)lua; (void)ctx;
#define UNO_LUA_BIND(Fn) Fn(lua, ctx);
#include "scripting/LuaBindList.inc"
#undef UNO_LUA_BIND
}

} // namespace dx12e
