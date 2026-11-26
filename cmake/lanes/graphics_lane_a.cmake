# graphics のレーン A 用の追加ソース。
# 自分のレーンのファイルだけを編集する（他レーンのファイルは触らない）。
# W05-03: Agility のランタイム情報（AgilityExports.cpp / AgilityTestGuard.cpp は exe へ直接リンクするのでここに入れない）。
target_sources(Graphics PRIVATE
    AgilityRuntime.cpp
    DeviceCaps.cpp          # W05-04: デバイスの機能（caps）の判定
)
target_include_directories(Graphics PRIVATE "${UNO_AGILITY_GEN_DIR}")
