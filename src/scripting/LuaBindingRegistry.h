#pragma once
// Lua 束縛の登録表（W05-15 / R-00b）。
// 新しい束縛は scripting/bind/<名前>.cpp に `void BindXxx(sol::state&, ScriptContext&)` を書き、
// LuaBindList.inc に UNO_LUA_BIND(BindXxx) を 1 行足す（ScriptEngine.cpp は触らない）。
// ★自己登録（静的初期化子）は使わない。静的ライブラリでは .obj ごとリンカに捨てられるため。
#include <sol/forward.hpp>

namespace dx12e
{

class Scene;

// 束縛が ScriptEngine から受け取る文脈。メンバへのポインタで持つので、あとから差し替わる値にも追従する。
struct ScriptContext
{
    Scene* const* scene = nullptr;   // ScriptEngine::m_scene
};

// ★ctx は束縛の呼び出し中だけ有効（ScriptEngine の呼び出しのスタック上の値）。
//   必要な値はコピーして持ち出し、ctx（や &ctx）をラムダで捕まえてはいけない。
using LuaBindFn = void (*)(sol::state&, ScriptContext&);

// LuaBindList.inc の UNO_LUA_BIND(Fn) を書いた順に呼ぶ。
void RunAllLuaBindings(sol::state& lua, ScriptContext& ctx);

} // namespace dx12e
