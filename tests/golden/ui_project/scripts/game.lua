-- ui_project  (template: empty)

-- グローバルフック: Play 中に毎フレーム呼ばれる。
-- エンティティ単位の振る舞いは assets/components/*.lua に部品として書き、
-- エディタで D&D するかシーン JSON の luaScript で貼るのがおすすめ（例: Spinner.lua）。
-- 高レベル API の一覧は docs/SCRIPTING.md。
function OnUpdate(dt)
  -- 例: ui:text(24, 24, "Hello, Uno Engine!", 24, 1, 1, 1, 1)
end
