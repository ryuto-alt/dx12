# editor のレーン A 用の追加ソース。
# 自分のレーンのファイルだけを編集する（他レーンのファイルは触らない）。
target_sources(Editor PRIVATE
    EditorChrome.cpp       # W1-A7 計器のクロム（主尺・キャリパー・ロゴの矩形）
    IconTable.cpp          # W1-A6 アイコンの種別表（Graphite 専用の色の取得口。ThemeGraphite.gen.h をここだけが読む）
    StatusBar.cpp          # W1-A7 ステータスバー（Graphite。黄の指針 ProgressRole::StatusJob の唯一の呼び出し元）
)
