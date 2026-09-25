#pragma once
// Lua の `audio` グローバル（AudioSystem の usertype）の束縛。
//
// ScriptEngine.cpp から分けてあるのは、音の担当と他の担当（ナビ / AI / 描画）が
// ScriptEngine.cpp を同時に触ってぶつからないようにするため。ScriptEngine::RegisterBindings が
// 1 回呼ぶだけ。束縛を足したら src/core/ApplicationInternal.cpp の McpLuaApi() の "audio" にも
// 同じ名前を書くこと（tests/lua_api_doc_test.cpp が src/scripting/*.cpp を全部読んで見張る）。

namespace sol { class state; }

namespace dx12e
{
void RegisterAudioBindings(sol::state& lua);
}
