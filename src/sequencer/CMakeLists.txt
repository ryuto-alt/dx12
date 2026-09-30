# シーケンサー(.dxseq)の純ロジック: データモデル・カーブ/クォータニオン補間・評価・イベント発火・SeqOp(Undo)・シリアライズ・
# バインディング解決(抽象)・旧 sequence_author 台本の変換。
# 標準ライブラリ + nlohmann-json だけ(GPU / D3D / ECS / ImGui 非依存。SequencerCoreTests がそれを検査する)。
# 設計は docs/SEQUENCER_DESIGN.md、仕様は docs/DXSEQ_FORMAT.md。統合(S1b)で Application が、UI(S1a)で Editor がリンクする。
find_package(nlohmann_json CONFIG REQUIRED)
add_library(SequencerCore STATIC
    SeqTypes.cpp
    SeqModel.cpp
    SeqCurve.cpp
    SeqEval.cpp
    SeqOps.cpp
    SeqSerialize.cpp
    SeqBinding.cpp
    SeqConvert.cpp
)
# "sequencer/Seq*.h" で include できるよう src/ を公開する
target_include_directories(SequencerCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(SequencerCore PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(SequencerCore PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(SequencerCore PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(SequencerCore PRIVATE ${DX12_COMPILE_DEFS})
endif()
