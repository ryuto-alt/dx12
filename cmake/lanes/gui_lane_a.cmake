# gui のレーン A 用の追加ソース。
# 自分のレーンのファイルだけを編集する（他レーンのファイルは触らない）。
target_sources(Gui PRIVATE
    uitests/lane_a.cpp
    ${CMAKE_SOURCE_DIR}/src/editor/ThemeGraphite.cpp   # Graphite Gauge の Spec（W1 A2）。ImGuiManager と同じ Gui に置き、テスト exe も引く
)
