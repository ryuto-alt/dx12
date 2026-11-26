# core のレーン B 用の追加ソース。
# 自分のレーンのファイルだけを編集する（他レーンのファイルは触らない）。
target_sources(Core PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/SplashGaugeRenderer.cpp   # 起動画面 Graphite Gauge の描画器（W1-BD3）
)
