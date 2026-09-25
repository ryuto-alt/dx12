#pragma once
// Lua の `save` グローバル（セーブスロットへの保存・読込・一覧）の束縛。
//
// ScriptEngine.cpp から分けてあるのは、セーブの担当と他の担当（音 / ナビ / AI / 描画）が
// ScriptEngine.cpp を同時に触ってぶつからないようにするため。ScriptEngine::RegisterBindings が
// 1 回呼ぶだけ。束縛を足したら src/core/ApplicationInternal.cpp の McpLuaApi() の "save" にも
// 同じ名前を書くこと（tests/lua_api_doc_test.cpp が src/scripting/*.cpp を全部読んで見張る）。
//
// ファイルの形・壊れ対策・書き込み先は core/save/（SaveFile / SaveService / UserData）。
// ここは「Lua の値 ⇔ セーブ本体の JSON」と「エンティティの状態の取り込み / 戻し」だけ。

namespace sol { class state; }

namespace dx12e
{
class Scene;
class ScriptEngine;

void RegisterSaveBindings(sol::state& lua, Scene* scene, ScriptEngine* engine);
}
