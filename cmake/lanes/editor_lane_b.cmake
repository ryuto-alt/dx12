# editor のレーン B 用の追加ソース。
# 自分のレーンのファイルだけを編集する（他レーンのファイルは触らない）。
target_sources(Editor PRIVATE
    ${CMAKE_SOURCE_DIR}/src/editor/undo/ScriptPropertyUndo.cpp   # W05-14 Lua プロパティの Undo
)
