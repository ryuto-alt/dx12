# renderer のレーン C 用の追加ソース。
# 自分のレーンのファイルだけを編集する（他レーンのファイルは触らない）。
target_sources(Renderer PRIVATE
    FeatureList.cpp   # W05-25: FeatureList.inc の登録と RegisterAllFeatures
)
# 試験用ビルド（既定 OFF）。ON のとき FeatureList.cpp だけが UNO_TESTING を定義し、
# 環境変数 UNO_TEST_STAGE_RECT=1 で「AfterTranslucent に 4x4 の矩形を描く」試験用の機能を登録する。
option(UNO_TESTING "試験用の描画フィーチャ（矩形）を FeatureList に含める" OFF)
if(UNO_TESTING)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/FeatureList.cpp PROPERTIES COMPILE_DEFINITIONS UNO_TESTING)
endif()
