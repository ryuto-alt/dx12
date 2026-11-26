# core のレーン C 用の追加ソース。
# 自分のレーンのファイルだけを編集する（他レーンのファイルは触らない）。
target_sources(Core PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/QualityPresetApply.cpp
)
