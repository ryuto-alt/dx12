# 単体テスト。
#
# このディレクトリは2通りで使える:
#  1) ルート CMakeLists から add_subdirectory(tests) 経由 (エンジンと一緒にビルド)
#  2) スタンドアロン: cmake -S tests -B build-tests
#     -> vcpkg も VS2026 も不要。依存ゼロなので CI(GitHub Actions)が緑になる。
cmake_minimum_required(VERSION 3.20)

# スタンドアロン構成時のみ project() を宣言 (サブディレクトリ取り込み時は親に従う)
if(NOT DEFINED PROJECT_NAME OR PROJECT_NAME STREQUAL "")
    project(DX12EngineTests LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_executable(IndexAllocatorTests test_index_allocator.cpp)

# core/IndexAllocator.h, core/Types.h を解決するため src/ をインクルードパスへ
target_include_directories(IndexAllocatorTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

if(MSVC)
    target_compile_options(IndexAllocatorTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(IndexAllocatorTests PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME IndexAllocatorTests COMMAND IndexAllocatorTests)

# NetWriter/NetReader 往復テスト（GPU非依存・ヘッダオンリー、vcpkg/entt不要なのでスタンドアロンでも動く）。
# 当たり判定の実効サイズの規約（src/physics/ColliderShape.h）。ヘッダオンリーの純粋関数なので
# Jolt も entt も GPU も要らない＝依存ゼロの CI でも回る。
# ★判定を作る側と線で描く側が同じ規則を使っていることの安全網。片方だけ変わると
#   「見えている線と当たる場所が違う」デバッグ表示になり、目では気づけない。
add_executable(ColliderShapeTests collider_shape_test.cpp)
target_include_directories(ColliderShapeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
# ★DirectXMath は Windows SDK に同梱されているので、vcpkg 抜きのテストツリー単体構成
#   (cmake -S tests -B build-tests) でもそのまま通る。find_package(REQUIRED) を書くと
#   依存ゼロの CI がここで落ちるので書かない。エンジンと一緒にビルドするときだけ
#   vcpkg 版のターゲットへ繋ぐ（使うのは XMFLOAT3(POD) だけなのでどちらでも同じ）。
if(TARGET Microsoft::DirectXMath)
    target_link_libraries(ColliderShapeTests PRIVATE Microsoft::DirectXMath)
endif()
if(MSVC)
    target_compile_options(ColliderShapeTests PRIVATE /W4 /WX /permissive- /utf-8)
endif()
add_test(NAME ColliderShapeTests COMMAND ColliderShapeTests)

# 物理の判断だけの純粋関数（補間・テレポート判定・キャラと動的剛体の押し合い規約）。
# ヘッダオンリー・Jolt/entt 不要。
add_executable(PhysicsLogicTests physics_logic_test.cpp)
target_include_directories(PhysicsLogicTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(PhysicsLogicTests PRIVATE /W4 /WX /permissive- /utf-8)
endif()
add_test(NAME PhysicsLogicTests COMMAND PhysicsLogicTests)

add_executable(NetBufferTests net_buffer_test.cpp)
target_include_directories(NetBufferTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(NetBufferTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(NetBufferTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME NetBufferTests COMMAND NetBufferTests)

# 知覚層（dx12_perceive）の集計ロジック（renderer/PerceptionStats.{h,cpp}）。
# ID 画素と表示色の配列 → エンティティごとの占有率・周囲との輝度比・遮蔽・灯りの向き。
# GPU も DirectXMath も使わない純関数なので、依存ゼロのスタンドアロン構成でも回る。
# ★TS 側（tools/mcp-server/perceive.ts）の言葉の境界はここの定義が前提。黙って変えさせない。
add_executable(PerceptionStatsTests perception_stats_test.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/renderer/PerceptionStats.cpp)
target_include_directories(PerceptionStatsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(PerceptionStatsTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(PerceptionStatsTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME PerceptionStatsTests COMMAND PerceptionStatsTests)

# クラスタードライティング（Forward+）の Z 指数分割とカリング判定のテスト。
# renderer/ClusterMath.h はヘッダオンリー・GPU/DirectXMath 非依存なのでスタンドアロンでも動く。
# HLSL 側（shaders/forward/ClusterCommon.hlsli）は同じ式を手で写しているので、
# 片方を直したら必ず両方直すこと。
add_executable(ClusterMathTests cluster_math_test.cpp)
target_include_directories(ClusterMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(ClusterMathTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(ClusterMathTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME ClusterMathTests COMMAND ClusterMathTests)

# IES (LM-63) の解析と縦角プロファイルへの焼き込み（renderer/IesProfile.h。ヘッダオンリー・GPU 不要）。
# 同梱の assets/engine/light/*.ies も読めることを確かめる（tools/gen_light_assets.py の出力）。
add_executable(IesProfileTests ies_profile_test.cpp)
target_include_directories(IesProfileTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(IesProfileTests PRIVATE DX12_TEST_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../assets/")
if(MSVC)
    target_compile_options(IesProfileTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(IesProfileTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME IesProfileTests COMMAND IesProfileTests)

# 仮想ジオメトリ(Nanite 風)の `.vgeo` 形式（renderer/vg/VgeoFormat.h + VgsrcFormat.h + VgeoBvh.h）。
# ヘッダオンリー・標準ライブラリのみなので、依存ゼロのスタンドアロン構成（CI）でも回る。
# 構造体のサイズ/オフセット固定・往復・境界値・破損入力の拒否・決定論・手書き 3 レベル DAG の不変条件を見る。
# 仕様の正本は docs/VGEO_SPEC.md。形式を変えたらここが落ちる（＝意図した変更か確認する）。
add_executable(VgeoFormatTests vgeo_format_test.cpp)
target_include_directories(VgeoFormatTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(VgeoFormatTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(VgeoFormatTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME VgeoFormatTests COMMAND VgeoFormatTests)
# 診断用: 同じテスト(ファズ含む)を AddressSanitizer で回す。既定ビルドには入れない（EXCLUDE_FROM_ALL）。
#   pwsh tools/build.ps1 -Target VgeoFormatTestsAsan → build/release/tests/VgeoFormatTestsAsan.exe
#   実行には MSVC の clang_rt.asan_dynamic-x86_64.dll が PATH に要る（vcvars64 を読んだシェルで動かす）。
# 未署名の平置きファイルを読むローダなので、壊れた入力での範囲外アクセスをここで確かめる。
if(MSVC)
    add_executable(VgeoFormatTestsAsan EXCLUDE_FROM_ALL vgeo_format_test.cpp)
    target_include_directories(VgeoFormatTestsAsan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_options(VgeoFormatTestsAsan PRIVATE /W4 /WX /permissive- /utf-8 /fsanitize=address)
endif()

# 音の純粋ロジック（src/audio/AudioMath.h: 奪う相手の選び方 / 仮想化のヒステリシス /
# dB 換算 / フィルタ係数 / スナップショットとリバーブ域の補間）。ヘッダオンリー・依存ゼロ
# （XAudio2 も音声デバイスも要らない）なのでスタンドアロンでも動く。
add_executable(AudioMathTests audio_math_test.cpp)
target_include_directories(AudioMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(AudioMathTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(AudioMathTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME AudioMathTests COMMAND AudioMathTests)

# ストリーミング再生のリングとチャンク詰め（src/audio/AudioStreamRing.h）。偽デコーダで
# ループ点・終端・「鳴っているチャンクを上書きしない」を確かめる。依存ゼロ。
add_executable(AudioStreamTests audio_stream_test.cpp)
target_include_directories(AudioStreamTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(AudioStreamTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(AudioStreamTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME AudioStreamTests COMMAND AudioStreamTests)

# 起動画面（スプラッシュ）の純ロジック。ヘッダオンリー・依存ゼロ（Win32 も GPU も音デバイスも使わない）なので
# スタンドアロン構成でも回る。
#   SplashMotionTests … イージング/スプリング/タイムライン状態機械（早着・定刻・遅着）/進捗の重み付け/
#                       startup.json の EMA とクランプ/Tips ローテーション（core/SplashMotion.h, SplashProgress.h）
#   SplashMixerTests  … 起動音ミキサー（頂点の整列/クロスフェード/ループ保持/クリック無し。core/SplashMixer.h）
#   SplashSyncDump    … 検証用（テストではない）: ミキサーの計算結果を 3 シナリオ分 WAV に書き出す。音デバイスは開かない
add_executable(SplashMotionTests splash_motion_test.cpp)
target_include_directories(SplashMotionTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_executable(SplashMixerTests splash_mixer_test.cpp)
target_include_directories(SplashMixerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_executable(SplashSyncDump splash_sync_dump.cpp)
target_include_directories(SplashSyncDump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
foreach(t SplashMotionTests SplashMixerTests SplashSyncDump)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME SplashMotionTests COMMAND SplashMotionTests)
add_test(NAME SplashMixerTests COMMAND SplashMixerTests)

# 自動更新の体験（更新内容データ・GitHub 本文の整形・この版を飛ばす・進捗の表示）の純ロジック。標準ライブラリだけ。
#   データの整合（版の並び・必須項目・先頭 == kEngineVersion）/ 前回→今の版の範囲 / Markdown 書き出し /
#   GitHub 本文の簡易整形 / 「この版を飛ばす」の判定 / 速度と残り時間（core/ReleaseNotes.*, UpdateLogic.*, Version.cpp）。
add_executable(UpdateUxTests update_ux_test.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/ReleaseNotes.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/UpdateLogic.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/Version.cpp)
target_include_directories(UpdateUxTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(UpdateUxTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(UpdateUxTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME UpdateUxTests COMMAND UpdateUxTests)

# UIText リッチテキスト(インラインタグ)パーサのテスト(ヘッダオンリー・依存ゼロなのでスタンドアロンでも動く)。
add_executable(UiRichTextTests ui_rich_text_test.cpp)
target_include_directories(UiRichTextTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(UiRichTextTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(UiRichTextTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME UiRichTextTests COMMAND UiRichTextTests)

# Sprite2D フリップブック/UVスクロールの UV 計算(renderer/SpriteAnim.h)テスト。
# ヘッダオンリー・依存ゼロなのでスタンドアロンでも動く。
add_executable(SpriteAnimTests sprite_anim_test.cpp)
target_include_directories(SpriteAnimTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(SpriteAnimTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(SpriteAnimTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME SpriteAnimTests COMMAND SpriteAnimTests)

# ブレンドツリーの重み計算(animation/BlendTree.h)テスト。
# 1D 区間線形補間 + 2D Gradient Band（Johansen 論文 §6.3/§6.3.1）。
# ヘッダオンリー・依存ゼロなのでスタンドアロン構成でも動く。
add_executable(BlendTreeTests blend_tree_test.cpp)
target_include_directories(BlendTreeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(BlendTreeTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(BlendTreeTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME BlendTreeTests COMMAND BlendTreeTests)

# UIアニメクリップ(animation/UiAnimAsset.h)の評価テスト。
# 評価とイージングは純関数(nlohmann も entt も要らない)なのでスタンドアロンでも動く。
add_executable(UiAnimTests ui_anim_test.cpp)
target_include_directories(UiAnimTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(UiAnimTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(UiAnimTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME UiAnimTests COMMAND UiAnimTests)

# スプライトシート(animation/SpriteAnimAsset.h)の任意セル順/可変フレーム長の畳み込みテスト。
# こちらも純関数・依存ゼロ。
add_executable(SpriteAnimAssetTests sprite_anim_asset_test.cpp)
target_include_directories(SpriteAnimAssetTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(SpriteAnimAssetTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(SpriteAnimAssetTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME SpriteAnimAssetTests COMMAND SpriteAnimAssetTests)

# OGG Vorbis デコード(third_party/stb_vorbis.c、AudioClip .ogg 対応の土台)テスト。
# 埋め込み OGG(ogg_test_data.h)をメモリからデコードして PCM 長を確認する。依存ゼロ。
# stb_vorbis はサードパーティ実装をそのまま取り込むため、警告のエラー化はしない。
add_executable(OggDecodeTests ogg_decode_test.cpp)
target_include_directories(OggDecodeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
if(MSVC)
    target_compile_options(OggDecodeTests PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(OggDecodeTests PRIVATE -Wall -Wextra)
endif()
add_test(NAME OggDecodeTests COMMAND OggDecodeTests)

# シリアライズ往復テスト（安全網）。Scene ライブラリ（+ 推移依存）をリンクするため、
# エンジン本体と一緒にビルドする構成でのみ有効。
# tests を単体（cmake -S tests, vcpkg/VS不要）で構成したときは Scene が無いのでスキップし、
# 依存ゼロの CI を壊さない。
if(TARGET Scene)
    add_executable(SerializeRoundtripTests serialize_roundtrip_test.cpp)

    # Scene が PUBLIC で src/ インクルードと全依存（entt/nlohmann/DirectXMath/...）を伝播する。
    target_link_libraries(SerializeRoundtripTests PRIVATE Scene)
    # 封印ランタイムの中立公開ヘッダ（include/engine/...）。
    target_include_directories(SerializeRoundtripTests PRIVATE ${CMAKE_SOURCE_DIR}/include)

    if(MSVC)
        # エンジン本体と同一のフラグ（/external:W0 で外部ヘッダ警告を /WX から除外）。
        target_compile_options(SerializeRoundtripTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SerializeRoundtripTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()

    add_test(NAME SerializeRoundtripTests COMMAND SerializeRoundtripTests)
endif()

# ==== lane-A begin ====
# --- 検査（lint）の登録（EXEC_W05_W1 W05-01）---
# GpuLabelLint: D3D12CreateDevice / GraphicsDevice を使うテスト exe に gpu ラベルがあるか。
#   コンフィグの最後（DEFER）に、tests/ の各テスト exe の自前ソースを見て、違反の一覧をファイルへ書く。
#   ctest は cmake/lint/GpuLabelLint.cmake でそのファイルを読んで落とす。
#   同じソースを GPU 無しで使うテスト（GPU 部分が #if で外れる）は下の IGNORE へ。
# リポジトリのルート。CMAKE_SOURCE_DIR は使わない（CI は `cmake -S tests` で単独構成するので tests/ になる）。
get_filename_component(DX12_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(DX12_GPU_LABEL_LINT_IGNORE AtmosphereMathTests)
function(dx12_gpu_label_lint_collect)
    get_property(_tgts DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    set(_bad "")
    foreach(_t IN LISTS _tgts)
        if(NOT TEST ${_t} OR _t IN_LIST DX12_GPU_LABEL_LINT_IGNORE)
            continue()
        endif()
        get_target_property(_srcs ${_t} SOURCES)
        set(_uses FALSE)
        foreach(_s IN LISTS _srcs)
            if(NOT IS_ABSOLUTE "${_s}")
                set(_s "${CMAKE_CURRENT_SOURCE_DIR}/${_s}")
            endif()
            # tests/ 直下の自前ソースだけを見る（ライブラリ側の GraphicsDevice の参照は数えない）
            if(_s MATCHES "/tests/[^/]+\\.cpp$" AND EXISTS "${_s}")
                file(READ "${_s}" _txt)
                if(_txt MATCHES "D3D12CreateDevice" OR _txt MATCHES "GraphicsDevice")
                    set(_uses TRUE)
                endif()
            endif()
        endforeach()
        if(_uses)
            get_test_property(${_t} LABELS _labels)
            if(NOT "gpu" IN_LIST _labels)
                string(APPEND _bad "  ${_t}\n")
            endif()
        endif()
    endforeach()
    file(WRITE "${CMAKE_BINARY_DIR}/gpu_label_lint_report.txt" "${_bad}")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL dx12_gpu_label_lint_collect)
add_test(NAME GpuLabelLint
    COMMAND ${CMAKE_COMMAND} -DREPORT=${CMAKE_BINARY_DIR}/gpu_label_lint_report.txt
            -P ${DX12_REPO_ROOT}/cmake/lint/GpuLabelLint.cmake)
set_tests_properties(GpuLabelLint PROPERTIES TIMEOUT 60)

# NoPaidApi: 有料の Anthropic API の SDK・キーの参照が無いこと（決定 3。検索語は cmake/lint/NoPaidApi.cmake）。
add_test(NAME NoPaidApi
    COMMAND ${CMAKE_COMMAND} -DREPO_ROOT=${DX12_REPO_ROOT} -P ${DX12_REPO_ROOT}/cmake/lint/NoPaidApi.cmake)
set_tests_properties(NoPaidApi PROPERTIES TIMEOUT 120)

# AnchorLint: tools/check_anchors.mjs（W05-31、M0 で統合済み）を --strict で直接実行する。
# node が見つからないときも、黙って登録を省かず、必ず失敗するテストとして登録する。
# GenBudgetsCheck: gen_budgets.mjs（W05-34、M5）がまだ無ければ、ラッパーが終了コード 77 を返して SKIP になる。
find_program(DX12_NODE_EXE node)
if(DX12_NODE_EXE)
    set(DX12_NODE_IF_EXISTS ${DX12_REPO_ROOT}/cmake/lint/run_if_exists.mjs)
    add_test(NAME AnchorLint COMMAND ${DX12_NODE_EXE}
             ${DX12_REPO_ROOT}/tools/check_anchors.mjs --strict
             WORKING_DIRECTORY ${DX12_REPO_ROOT})
    set_tests_properties(AnchorLint PROPERTIES TIMEOUT 120)
    add_test(NAME GenBudgetsCheck COMMAND ${DX12_NODE_EXE} ${DX12_NODE_IF_EXISTS}
             ${DX12_REPO_ROOT}/tools/gen_budgets.mjs --check
             WORKING_DIRECTORY ${DX12_REPO_ROOT})
    set_tests_properties(GenBudgetsCheck PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
else()
    # node が無い環境: 失敗として見えるようにする（cmake -E false は常に終了コード 1）。
    add_test(NAME AnchorLint COMMAND ${CMAKE_COMMAND} -E false)
endif()
# W05-02（BUG-06）: 人の未保存の変更を自動保存が本体に書かない（編集の起点・自動保存の守り・退避の世代）。エンジン非リンク。
add_executable(AutosaveOriginTests autosave_origin_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/SceneBackup.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/AtomicFile.cpp)
target_include_directories(AutosaveOriginTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(AutosaveOriginTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(AutosaveOriginTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME AutosaveOriginTests COMMAND AutosaveOriginTests)

# --- W05-03: Agility SDK（F0-1）---
# gpu ラベルのテスト exe（デバイスを作るもの）には、Agility の export とランタイム（D3D12\）と AgilityTestGuard を足す。
# スタンドアロン構成（cmake -S tests。CI の unit-tests）では uno_agility_* が無いので何もしない。
if(COMMAND uno_agility_test_exe)
    function(dx12_agility_wire_gpu_tests)
        get_property(_tgts DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_t IN LISTS _tgts)
            if(NOT TEST ${_t})
                continue()
            endif()
            get_test_property(${_t} LABELS _labels)
            if("gpu" IN_LIST _labels)
                uno_agility_test_exe(${_t})
            endif()
        endforeach()
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL dx12_agility_wire_gpu_tests)

    # AgilityLoadTests（gpu）: テスト exe から作ったデバイスが Agility のランタイムで動いている。
    add_executable(AgilityLoadTests agility_load_test.cpp)
    target_include_directories(AgilityLoadTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(AgilityLoadTests PRIVATE Graphics)
    if(MSVC)
        target_compile_options(AgilityLoadTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(AgilityLoadTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AgilityLoadTests COMMAND AgilityLoadTests)
    set_tests_properties(AgilityLoadTests PROPERTIES LABELS gpu TIMEOUT 120)

    # DeviceCapsTests（gpu）: caps の全項目が取れ、SM が 6.9（ドライバが 595 未満なら 6.8 を許容）で、
    #   独立に問い合わせた値と一致する。
    add_executable(DeviceCapsTests device_caps_test.cpp)
    target_include_directories(DeviceCapsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    # GraphicsDevice::Initialize が Window& を取る（Window.cpp が InputSystem を参照）
    target_link_libraries(DeviceCapsTests PRIVATE Graphics Input)
    if(MSVC)
        target_compile_options(DeviceCapsTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(DeviceCapsTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME DeviceCapsTests COMMAND DeviceCapsTests)
    set_tests_properties(DeviceCapsTests PROPERTIES LABELS gpu TIMEOUT 120)

    # DredReportTests（gpu）: W05-05。DRED の既定の判定、BeginEvent("TestPass") → RemoveDevice で JSON が書かれ、
    #   removedReason が入る。リソースの SetName（Buffer / RenderTarget / Texture）。gameMode=true なら exe 名に関わらず既定 OFF。
    #   ★実デバイスは、決して Signal しないフェンスでキューを待たせて未完了のリストを残し、RemoveDevice 後に
    #   コンテキスト "TestPass"・コマンドリスト名 DredTestList・lastCompleted の整合を必須で検査する（この PC のドライバは RemoveDevice で DRED を埋めない: その場合は SKIP_RETURN_CODE 77 で環境スキップ。合成データの検査は常に必須）。
    add_executable(DredReportTests dred_report_test.cpp)
    target_include_directories(DredReportTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(DredReportTests PRIVATE Graphics Input)
    if(MSVC)
        target_compile_options(DredReportTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(DredReportTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME DredReportTests COMMAND DredReportTests)
    set_tests_properties(DredReportTests PROPERTIES LABELS gpu TIMEOUT 120 SKIP_RETURN_CODE 77)

    # AgilityVersionPolicy（GPU 不要）: lock の SDK が 600 以上 700 未満（プレビューを出荷しない）、
    #   ビルドした exe 2 本の export と同梱 DLL の export と lock の 3 つが一致、D3D12SDKPath、DLL の SHA256。
    add_executable(AgilityPolicyCheck agility_policy_test.cpp)
    target_include_directories(AgilityPolicyCheck PRIVATE "${UNO_AGILITY_GEN_DIR}")
    if(MSVC)
        target_compile_options(AgilityPolicyCheck PRIVATE /W4 /WX /permissive- /utf-8)
    endif()
    add_test(NAME AgilityVersionPolicy
        COMMAND AgilityPolicyCheck $<TARGET_FILE:DX12Engine> $<TARGET_FILE:GameRuntime>)
    set_tests_properties(AgilityVersionPolicy PROPERTIES TIMEOUT 60)

    # AgilityGuardNegative（gpu）: AgilityTestGuard が実際に落とす（D3D12\ 無しで 98、継承した UNO_DEBUG_LAYER_FAILED は無視）。
    find_program(DX12_PWSH_EXE pwsh)
    if(DX12_PWSH_EXE)
        add_test(NAME AgilityGuardNegative
            COMMAND ${DX12_PWSH_EXE} -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/agility_guard_negative.ps1
                    -ExeDir $<TARGET_FILE_DIR:AgilityLoadTests>)
        set_tests_properties(AgilityGuardNegative PROPERTIES LABELS gpu TIMEOUT 180)
    endif()

    # AgilityMissingRedist（夜間）: D3D12\ を抜いた一時コピーを --headless で動かし、縮退か決まったコード（0x2A）で終わる。
    find_program(DX12_PWSH_EXE pwsh)
    if(DX12_PWSH_EXE)
        # エンジンの MCP ポートはレーンごとの固定値（worktree_setup.ps1 の表: lane-a 8851 / b 8852 / c 8853 / d 8855）。
        # 夜間に 2 レーンが同時に回っても衝突しないよう、worktree のフォルダ名から決める（本体は 8859）。
        get_filename_component(_uno_wt_name "${CMAKE_SOURCE_DIR}" NAME)
        set(_uno_lane_port 8859)
        if(_uno_wt_name STREQUAL "lane-a")
            set(_uno_lane_port 8851)
        elseif(_uno_wt_name STREQUAL "lane-b")
            set(_uno_lane_port 8852)
        elseif(_uno_wt_name STREQUAL "lane-c")
            set(_uno_lane_port 8853)
        elseif(_uno_wt_name STREQUAL "lane-d")
            set(_uno_lane_port 8855)
        endif()
        add_test(NAME AgilityMissingRedist
            COMMAND ${DX12_PWSH_EXE} -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/agility_missing_redist.ps1
                    -ExeDir $<TARGET_FILE_DIR:DX12Engine> -McpPort ${_uno_lane_port})
        set_tests_properties(AgilityMissingRedist PROPERTIES LABELS "nightly;gpu" TIMEOUT 180)
    endif()
endif()

# W05-06（AUD-04）: ディスクリプタの遅延返却（GPU 不要。DescriptorHeap の管理表だけを使う）。
if(TARGET Graphics)
    add_executable(DescriptorDeferredFreeTests descriptor_deferred_free_test.cpp)
    target_include_directories(DescriptorDeferredFreeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(DescriptorDeferredFreeTests PRIVATE Graphics)
    if(MSVC)
        target_compile_options(DescriptorDeferredFreeTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(DescriptorDeferredFreeTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME DescriptorDeferredFreeTests COMMAND DescriptorDeferredFreeTests)
    set_tests_properties(DescriptorDeferredFreeTests PROPERTIES TIMEOUT 60)
endif()

# W05-07: DLSS の配線。DlssBackend::CompiledIn() が UNO_DLSS_ACTIVE と一致し、exe の隣の nvngx_dlss.dll の有無とも一致すること。
#   installer/build.ps1 の -Dlss auto|require の判定もここで確かめる（pwsh があるときだけ）。GPU は使わない。
if(TARGET Renderer)   # Renderer があるのは本体と一緒に構成したときだけ。DX12Engine はこの後に定義されるので $<TARGET_FILE_DIR:> で遅延参照する
    add_executable(DlssCompiledInTests dlss_compiled_in_test.cpp)
    target_include_directories(DlssCompiledInTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(DlssCompiledInTests PRIVATE Renderer)
    if(UNO_DLSS_ACTIVE)
        target_compile_definitions(DlssCompiledInTests PRIVATE UNO_DLSS_EXPECT_COMPILED_IN=1)
    else()
        target_compile_definitions(DlssCompiledInTests PRIVATE UNO_DLSS_EXPECT_COMPILED_IN=0)
    endif()
    if(MSVC)
        target_compile_options(DlssCompiledInTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(DlssCompiledInTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    # テスト exe の出力先を DX12Engine と同じ場所にして、DLL の隣の探索（NGX）と条件を揃える必要は無い。検査は ENGINE_DIR で行う。
    find_program(DX12_PWSH_EXE pwsh)
    add_test(NAME DlssWiringCheck
        COMMAND ${CMAKE_COMMAND}
            -DENGINE_DIR=$<TARGET_FILE_DIR:DX12Engine>
            -DCOMPILED_IN_EXE=$<TARGET_FILE:DlssCompiledInTests>
            -DREPO_ROOT=${DX12_REPO_ROOT}
            -DPWSH=${DX12_PWSH_EXE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/dlss_wiring_check.cmake)
    set_tests_properties(DlssWiringCheck PROPERTIES TIMEOUT 120)
endif()

# --- UI 書体のサブセット（W1-A1）。生成物と字の集合の検査。GPU 不要 ---
if(TARGET Gui)
    add_executable(FontSubsetTests font_subset_test.cpp)
    target_link_libraries(FontSubsetTests PRIVATE Gui)
    target_compile_definitions(FontSubsetTests PRIVATE FONT_TEST_ROOT="${DX12_REPO_ROOT}")
    if(MSVC)
        target_compile_options(FontSubsetTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(FontSubsetTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME FontSubsetTests COMMAND FontSubsetTests)
endif()
# tools/subset_fonts.py --check（標準ライブラリだけ。charset と生成物の SHA256 と上限）
# Windows Store の python.exe スタブ（実体が無い）を拾わないよう、実行して確かめる FindPython3 を使う。見つからなければ警告を出す（黙って登録しない）
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME FontSubsetCheck COMMAND ${Python3_EXECUTABLE} -I ${DX12_REPO_ROOT}/tools/subset_fonts.py --check)
    set_tests_properties(FontSubsetCheck PROPERTIES TIMEOUT 120)
else()
    message(WARNING "Python3 が見つからないため FontSubsetCheck を登録できません（tools/subset_fonts.py --check は標準ライブラリだけで動く）")
endif()

# --- Graphite Gauge のトークン（W1-A2）。生成物と tokens.json の突き合わせ。GPU 不要 ---
if(TARGET Gui)
    add_executable(GgTokensGenTests gg_tokens_gen_test.cpp)
    target_link_libraries(GgTokensGenTests PRIVATE Gui)
    target_include_directories(GgTokensGenTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(GgTokensGenTests PRIVATE GG_TEST_ROOT="${DX12_REPO_ROOT}")
    if(MSVC)
        target_compile_options(GgTokensGenTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(GgTokensGenTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME GgTokensGenTests COMMAND GgTokensGenTests)
endif()
# tools/theme/gen_gg_tokens.py --check（標準ライブラリだけ。生成物が最新か / tokens の SHA256 が lock と一致か / integrate_check が全緑か）
if(Python3_Interpreter_FOUND)
    add_test(NAME GgTokensGenCheck COMMAND ${Python3_EXECUTABLE} -I ${DX12_REPO_ROOT}/tools/theme/gen_gg_tokens.py --check)
    set_tests_properties(GgTokensGenCheck PROPERTIES TIMEOUT 60)
endif()

# --- 直書き色の lint（W1-A3 / RM-A5）。src/editor・src/gui の IM_COL32 / 色の ImVec4 をファイルごとの件数の棚で止め、黄系は 0 件 ---
if(TARGET Gui)
    add_executable(UiTokenLintTests ui_token_lint_test.cpp)
    target_link_libraries(UiTokenLintTests PRIVATE Gui)
    target_include_directories(UiTokenLintTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(UiTokenLintTests PRIVATE UI_LINT_ROOT="${DX12_REPO_ROOT}")
    if(MSVC)
        target_compile_options(UiTokenLintTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(UiTokenLintTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME UiTokenLintTests COMMAND UiTokenLintTests)
    set_tests_properties(UiTokenLintTests PROPERTIES TIMEOUT 60)
endif()

# --- ui::anim（W1-A4 / GG-3）。曲線・ばね・retarget・Instant・Reduce Motion・状態の掃除。GPU 不要 ---
if(TARGET Gui)
    add_executable(UiMotionTests ui_motion_test.cpp)
    target_link_libraries(UiMotionTests PRIVATE Gui)
    target_include_directories(UiMotionTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(UiMotionTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(UiMotionTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME UiMotionTests COMMAND UiMotionTests)
    set_tests_properties(UiMotionTests PROPERTIES TIMEOUT 60)
endif()

# --- text::EllipsizeEnd と進捗の刻み送り（W1-A5 / RM-A3）。GPU 不要 ---
if(TARGET Gui)
    add_executable(TextFitTests text_fit_test.cpp)
    target_link_libraries(TextFitTests PRIVATE Gui)
    target_include_directories(TextFitTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(TextFitTests PRIVATE UI_LINT_ROOT="${DX12_REPO_ROOT}")
    if(MSVC)
        target_compile_options(TextFitTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(TextFitTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME TextFitTests COMMAND TextFitTests)
    set_tests_properties(TextFitTests PROPERTIES TIMEOUT 60)
endif()

# --- 裏に回ったときのフレーム間引きの状態遷移（W1-A11 / MOD-ED-11）。純ロジック。GPU 不要 ---
add_executable(FrameThrottleTests frame_throttle_test.cpp)
target_include_directories(FrameThrottleTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(FrameThrottleTests PRIVATE UI_LINT_ROOT="${DX12_REPO_ROOT}")
if(MSVC)
    target_compile_options(FrameThrottleTests PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(FrameThrottleTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME FrameThrottleTests COMMAND FrameThrottleTests)
set_tests_properties(FrameThrottleTests PROPERTIES TIMEOUT 60)

# --- ワンショット放出器の寿命と再トリガー（W1-A12 / BUG-12）。純ロジック。GPU 不要 ---
if(TARGET Scene)   # Components.h は entt 等が要る。Scene が依存を伝播する（単体構成ではスキップ）
add_executable(ParticleOneShotTests particle_oneshot_test.cpp)
target_link_libraries(ParticleOneShotTests PRIVATE Scene)
target_compile_definitions(ParticleOneShotTests PRIVATE UI_LINT_ROOT="${DX12_REPO_ROOT}")
if(MSVC)
    target_compile_options(ParticleOneShotTests PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(ParticleOneShotTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME ParticleOneShotTests COMMAND ParticleOneShotTests)
set_tests_properties(ParticleOneShotTests PROPERTIES TIMEOUT 60)

# --- パーティクルエディタの「適用」の確認と Undo（W1-A15 / BUG-21 の暫定策）。純ロジック。GPU 不要 ---
add_executable(ParticleApplyUndoTests particle_apply_undo_test.cpp ../src/editor/UndoSystem.cpp)
target_link_libraries(ParticleApplyUndoTests PRIVATE Scene)
target_include_directories(ParticleApplyUndoTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(ParticleApplyUndoTests PRIVATE UI_LINT_ROOT="${DX12_REPO_ROOT}")
if(MSVC)
    target_compile_options(ParticleApplyUndoTests PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(ParticleApplyUndoTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME ParticleApplyUndoTests COMMAND ParticleApplyUndoTests)
set_tests_properties(ParticleApplyUndoTests PROPERTIES TIMEOUT 60)
endif()

# ==== lane-A end ====

# シーンファイル形式 v2（既定値の省略・最短 float・整形・補完）の純関数テスト。GPU 不要。
# 生成モード: SceneFormatV2Tests --emit-defaults <out.json> で src/scene/scene_defaults_v2.json の初期値を作る。
if(TARGET Scene)
    add_executable(SceneFormatV2Tests scene_format_v2_test.cpp)
    target_link_libraries(SceneFormatV2Tests PRIVATE Scene)
    target_include_directories(SceneFormatV2Tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(SceneFormatV2Tests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SceneFormatV2Tests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SceneFormatV2Tests COMMAND SceneFormatV2Tests)
endif()

# シーンの分割保存（docs/SCENE_FORMAT_DESIGN.md §4.3）: セル割り当て・並び順（seq）・往復・変更したファイルだけ書く・空セルの掃除・参照の書き換え。GPU 不要。
if(TARGET Scene)
    add_executable(ScenePartitionTests scene_partition_test.cpp)
    target_link_libraries(ScenePartitionTests PRIVATE Scene)
    target_include_directories(ScenePartitionTests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(ScenePartitionTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ScenePartitionTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ScenePartitionTests COMMAND ScenePartitionTests)
endif()

# シーンの安全な保存: 保存の途中の失敗（置き換え・ディスク満杯・検証）で一式が前の版のまま / 途中で死んでも次に開くと新しい版で揃う /
# 壊れた・空のシーンは開けない / 世代つきバックアップ（世代数の上限・復元・生成物の除外）。GPU 不要。
if(TARGET Scene)
    add_executable(SceneSafeSaveTests scene_safe_save_test.cpp)
    target_link_libraries(SceneSafeSaveTests PRIVATE Scene)
    target_include_directories(SceneSafeSaveTests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(SceneSafeSaveTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SceneSafeSaveTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SceneSafeSaveTests COMMAND SceneSafeSaveTests)
endif()

# 保存中の強制終了テストの補助ツール（ctest には登録しない。gen: 分割シーンを作る / verify: 止められたシーンが完全に開けるか確かめる）。
if(TARGET Scene)
    add_executable(SceneSafeSaveTool scene_safe_save_tool.cpp)
    target_link_libraries(SceneSafeSaveTool PRIVATE Scene)
    target_include_directories(SceneSafeSaveTool PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(SceneSafeSaveTool PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SceneSafeSaveTool PRIVATE ${DX12_COMPILE_DEFS})
    endif()
endif()

# 物理（PhysicsSystem / Jolt）のヘッドレス回帰テスト。箱・球・カプセルだけ（Mesh は GPU が要るので使わない）。
# 質量 0 でも落ちない・offset・負スケール・静的の追従・上限・キャラ・接触イベント・時間の扱い…（PHYSICS_FIX2_REPORT.md）。
if(TARGET Physics)
    add_executable(PhysicsSystemTests physics_system_test.cpp)
    target_link_libraries(PhysicsSystemTests PRIVATE Physics)
    target_include_directories(PhysicsSystemTests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(PhysicsSystemTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(PhysicsSystemTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME PhysicsSystemTests COMMAND PhysicsSystemTests)
endif()

# インスタンス群（InstanceGroup。docs/SCENE_FORMAT_DESIGN.md §4.1 の 4-1）。サイドカーの往復・壊れた行・コピーオンライト・保存/読み込み。GPU 不要。
if(TARGET Scene)
    add_executable(InstanceGroupTests instance_group_test.cpp)
    target_link_libraries(InstanceGroupTests PRIVATE Scene)
    target_include_directories(InstanceGroupTests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(InstanceGroupTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(InstanceGroupTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME InstanceGroupTests COMMAND InstanceGroupTests)
endif()

# 名前付きシェーダーパラメーターの解決とトゥイーン（Trigger の SetShaderParam /
# AnimShaderParam の土台）。「オフセット → 添字」を 1 つ間違えると静かに隣のスロットを
# 壊し、症状は Play 中の見た目にしか出ないので機械で押さえる。
# リフレクションは使わず Param を手で積むので DXC もデバイスも不要。
if(TARGET Scene)
    add_executable(ShaderParamTweenTests shader_param_tween_test.cpp)
    target_link_libraries(ShaderParamTweenTests PRIVATE Scene)
    target_include_directories(ShaderParamTweenTests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(ShaderParamTweenTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ShaderParamTweenTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ShaderParamTweenTests COMMAND ShaderParamTweenTests)
endif()

# ==== lane-B begin ====
# 保存まわり（W05-12）: シーン名の検証 / 既存ファイルの検出 / SaveLastOpenedScene が実在しないパスを記録しない。
if(TARGET ProjectLib)
    add_executable(SceneSavePathsTests scene_save_paths_test.cpp)
    target_include_directories(SceneSavePathsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(SceneSavePathsTests PRIVATE ProjectLib)
    if(MSVC)
        target_compile_options(SceneSavePathsTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SceneSavePathsTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SceneSavePathsTests COMMAND SceneSavePathsTests)
    set_tests_properties(SceneSavePathsTests PROPERTIES TIMEOUT 120)
endif()
# Play -> Stop で Undo 履歴と選択を保つ（W05-11）: "_eid" の往復で entt id が一致 / TruncateTo。GPU 不要。
if(TARGET Scene)
    add_executable(PlayStopUndoTests play_stop_undo_test.cpp)
    target_link_libraries(PlayStopUndoTests PRIVATE Scene)
    target_include_directories(PlayStopUndoTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(PlayStopUndoTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(PlayStopUndoTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME PlayStopUndoTests COMMAND PlayStopUndoTests)
    set_tests_properties(PlayStopUndoTests PROPERTIES TIMEOUT 120)
endif()
# Undo コマンドは実行時ハンドル（Mesh* / bodyId / Lua 環境）を持たない（W05-11 恒久策）。Stop をまたぐ往復。GPU 不要。
if(TARGET Scene)
    add_executable(UndoHandleFreeTests undo_handle_free_test.cpp ../src/editor/UndoSystem.cpp)
    target_link_libraries(UndoHandleFreeTests PRIVATE Scene)
    target_include_directories(UndoHandleFreeTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(UndoHandleFreeTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(UndoHandleFreeTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME UndoHandleFreeTests COMMAND UndoHandleFreeTests)
    set_tests_properties(UndoHandleFreeTests PROPERTIES TIMEOUT 120)
endif()
# Lua 公開プロパティの Undo（W05-14）: 値の往復 / GUID で引く / ホットリロードで例外なし / 既定値に戻すは複合 1 件。GPU 不要。
if(TARGET ECS)
    add_executable(ScriptPropertyUndoTests script_property_undo_test.cpp ../src/editor/undo/ScriptPropertyUndo.cpp)
    target_link_libraries(ScriptPropertyUndoTests PRIVATE ECS)
    target_include_directories(ScriptPropertyUndoTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(ScriptPropertyUndoTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ScriptPropertyUndoTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ScriptPropertyUndoTests COMMAND ScriptPropertyUndoTests)
    set_tests_properties(ScriptPropertyUndoTests PROPERTIES TIMEOUT 120)
endif()
# コンポーネントのシリアライザ登録表と PostProcess の V2 マクロ（W05-15）: 未知の型の往復 / skipOnSave / 既定なら保存もハッシュもしない。GPU 不要。
if(TARGET Scene)
    add_executable(ComponentRegistryTests component_registry_test.cpp)
    target_link_libraries(ComponentRegistryTests PRIVATE Scene)
    target_include_directories(ComponentRegistryTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(ComponentRegistryTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ComponentRegistryTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ComponentRegistryTests COMMAND ComponentRegistryTests)
    set_tests_properties(ComponentRegistryTests PROPERTIES TIMEOUT 120)
endif()
# McpUndoTracker のレジストリ経由のフォールバック（W05-15）: 編集 / 追加 / 削除の Undo・Redo が JSON まで戻る。GPU 不要。
if(TARGET Core AND TARGET Gui AND TARGET Editor AND TARGET ProjectLib)   # エンジン本体と同じリンク集合（McpUndoTrack.cpp が Core の奥まで引くため）
    add_executable(McpUndoRegistryTests mcp_undo_registry_test.cpp)
    target_link_libraries(McpUndoRegistryTests PRIVATE Core Graphics Renderer Resource Animation ECS Audio Input Hardware Terrain Nav AI Scene Scripting Physics Network Gui Ui Editor ProjectLib)
    target_include_directories(McpUndoRegistryTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(McpUndoRegistryTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(McpUndoRegistryTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME McpUndoRegistryTests COMMAND McpUndoRegistryTests)
    set_tests_properties(McpUndoRegistryTests PROPERTIES TIMEOUT 120)
endif()
# Lua 束縛の登録表（W05-15）: post.* の往復 / names / setMany / 不明キー。GPU 不要。
if(TARGET Scripting)
    add_executable(LuaBindingRegistryTests lua_binding_registry_test.cpp)
    target_link_libraries(LuaBindingRegistryTests PRIVATE Scripting)
    target_include_directories(LuaBindingRegistryTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src ${LUA_INCLUDE_DIR})
    if(MSVC)
        target_compile_options(LuaBindingRegistryTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(LuaBindingRegistryTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME LuaBindingRegistryTests COMMAND LuaBindingRegistryTests)
    set_tests_properties(LuaBindingRegistryTests PROPERTIES TIMEOUT 120)
endif()
# 起動音 Calibrate（W1-BD1）: イベント表（生成されたヘッダ）の形の検査。GPU 不要。
add_executable(SplashCalibrateEventsTests splash_calibrate_events_test.cpp)
target_include_directories(SplashCalibrateEventsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(SplashCalibrateEventsTests PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(SplashCalibrateEventsTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME SplashCalibrateEventsTests COMMAND SplashCalibrateEventsTests)
# 起動画面 Graphite Gauge の動きの純ロジック（W1-BD2。core/SplashGaugeMotion.h）。ヘッダオンリー・GPU 不要。
add_executable(SplashGaugeMotionTests splash_gauge_motion_test.cpp)
target_include_directories(SplashGaugeMotionTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(SplashGaugeMotionTests PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(SplashGaugeMotionTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME SplashGaugeMotionTests COMMAND SplashGaugeMotionTests)
# tools/gen_boot_sound_calibrate.py: --check-assets は標準ライブラリだけ（コミット済みの資産の検査）。
# --check は再合成（numpy が要る。tools/requirements-audio.txt）。numpy が無い python では登録しない（警告ではなく STATUS。標準の環境には numpy が無い）
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME CalibrateSoundAssets COMMAND ${Python3_EXECUTABLE} -I ${DX12_REPO_ROOT}/tools/gen_boot_sound_calibrate.py --check-assets)
    set_tests_properties(CalibrateSoundAssets PROPERTIES TIMEOUT 120)
    execute_process(COMMAND ${Python3_EXECUTABLE} -I -c "import numpy" RESULT_VARIABLE _calib_np_rc OUTPUT_QUIET ERROR_QUIET)
    if(_calib_np_rc EQUAL 0)
        add_test(NAME CalibrateSoundCheck COMMAND ${Python3_EXECUTABLE} -I ${DX12_REPO_ROOT}/tools/gen_boot_sound_calibrate.py --check)
        set_tests_properties(CalibrateSoundCheck PROPERTIES TIMEOUT 300)
    else()
        message(STATUS "numpy が無い python のため CalibrateSoundCheck（再合成）は登録しません。venv に tools/requirements-audio.txt を入れると有効になります")
    endif()
else()
    message(WARNING "Python3 が見つからないため CalibrateSoundAssets を登録できません")
endif()
# AUD-05（W1-BD9）: pak の並列読み取りがスレッドセーフ（8 スレッド x 1000 回で直列の結果と一致）。
if(TARGET Core)
    add_executable(PakParallelReadTests pak_parallel_read_test.cpp)
    target_link_libraries(PakParallelReadTests PRIVATE Core)
    target_include_directories(PakParallelReadTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(PakParallelReadTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(PakParallelReadTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME PakParallelReadTests COMMAND PakParallelReadTests)
    set_tests_properties(PakParallelReadTests PROPERTIES TIMEOUT 120)
endif()
# UnoFX（W1-BD4）: .uvfx の読み書き・検証・命令列 / 放出の算術・カール場の CPU 参照。GPU 不要。ソースを直接コンパイルして Renderer に依存しない。
find_package(nlohmann_json CONFIG REQUIRED)
find_package(directxmath CONFIG REQUIRED)
add_executable(VfxAssetTests vfx_asset_test.cpp ../src/renderer/vfx/VfxAsset.cpp)
target_include_directories(VfxAssetTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(VfxAssetTests PRIVATE nlohmann_json::nlohmann_json)
add_executable(VfxSpawnMathTests vfx_spawn_math_test.cpp ../src/renderer/vfx/VfxCurl.cpp ../src/renderer/vfx/VfxAsset.cpp)
target_include_directories(VfxSpawnMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(VfxSpawnMathTests PRIVATE nlohmann_json::nlohmann_json Microsoft::DirectXMath)
if(MSVC)
    foreach(_t VfxAssetTests VfxSpawnMathTests)
        target_compile_options(${_t} PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(${_t} PRIVATE ${DX12_COMPILE_DEFS})
    endforeach()
endif()
add_test(NAME VfxAssetTests COMMAND VfxAssetTests
    ${DX12_REPO_ROOT}/assets/engine/vfx_core/p_core_fire.uvfx ${DX12_REPO_ROOT}/assets/engine/vfx_core/p_core_burst.uvfx)
add_test(NAME VfxSpawnMathTests COMMAND VfxSpawnMathTests)
set_tests_properties(VfxAssetTests VfxSpawnMathTests PROPERTIES TIMEOUT 120)
# VfxGpuTests（gpu）: カール場の Texture3D を窓なしの D3D12 で引き、CPU 参照（SampleCurl）と一致（テクセル中心は 1e-5、フィルタ込みは 2.5e-3 以内）することを確かめる。GPU が無ければ SKIP。
if(TARGET Renderer)
    add_executable(VfxGpuTests vfx_gpu_test.cpp)
    target_include_directories(VfxGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(VfxGpuTests PRIVATE Renderer Microsoft::DirectXMath)
    target_compile_definitions(VfxGpuTests PRIVATE DX12E_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
    if(TARGET Shaders)
        add_dependencies(VfxGpuTests Shaders)
    endif()
    if(MSVC)
        target_compile_options(VfxGpuTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VfxGpuTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME VfxGpuTests COMMAND VfxGpuTests)
    set_tests_properties(VfxGpuTests PROPERTIES LABELS gpu TIMEOUT 120)
endif()
# ==== lane-B end ====

# .dxmat(マテリアルアセット)JSON往復テスト。Resource(GPU非依存の純ロジック部分のみ使用)をリンクするため、
# エンジン本体と一緒にビルドする構成でのみ有効(スタンドアロン構成では Resource が無いのでスキップ)。
if(TARGET Resource)
    add_executable(DxmatRoundtripTests dxmat_roundtrip_test.cpp)
    target_link_libraries(DxmatRoundtripTests PRIVATE Resource)
    if(MSVC)
        target_compile_options(DxmatRoundtripTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(DxmatRoundtripTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME DxmatRoundtripTests COMMAND DxmatRoundtripTests)
endif()

# BC7/BC5 テクスチャ圧縮の形式選択 + DirectXTex の圧縮/伸長往復テスト。
# Resource が Microsoft::DirectXTex を PUBLIC でリンクしているので、GPU 無しで CPU コーデックを叩ける。
if(TARGET Resource)
    add_executable(TextureCompressTests texture_compress_test.cpp)
    target_link_libraries(TextureCompressTests PRIVATE Resource)
    if(MSVC)
        target_compile_options(TextureCompressTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(TextureCompressTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME TextureCompressTests COMMAND TextureCompressTests)
endif()

# ゲームビルドの裏ジョブの進捗モデル（core/BuildJob.h・ヘッダのみ）。
if(TARGET Resource)
    add_executable(BuildJobTests build_job_test.cpp)
    target_link_libraries(BuildJobTests PRIVATE Resource)
    if(MSVC)
        target_compile_options(BuildJobTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(BuildJobTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME BuildJobTests COMMAND BuildJobTests)
endif()

# entt::meta コア部品フィールド反映テスト（Phase 2 土台）。ECS（+ entt/DirectXMath）をリンク。
if(TARGET ECS)
    add_executable(ComponentMetaTests component_meta_test.cpp)
    target_link_libraries(ComponentMetaTests PRIVATE ECS)
    if(MSVC)
        target_compile_options(ComponentMetaTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ComponentMetaTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ComponentMetaTests COMMAND ComponentMetaTests)

    # QuaternionToEulerDegrees の往復テスト。
    # 規約を取り違えると「保存すると少しだけ姿勢がずれる」形で出る（気づきにくい）。
    add_executable(QuatEulerTests quat_euler_test.cpp)
    target_link_libraries(QuatEulerTests PRIVATE ECS)
    if(MSVC)
        target_compile_options(QuatEulerTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(QuatEulerTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME QuatEulerTests COMMAND QuatEulerTests)
endif()

# Camera 投影行列（透視/正射）テスト（Phase 4: ortho 能力）。
# Camera.cpp は純 DirectXMath（GPU非依存）なので直接ビルドして検証する。
# 完全ビルド時のみ（directxmath が要るため Renderer ターゲットの有無で判定）。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(CameraProjectionTests
        camera_projection_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/renderer/Camera.cpp)
    target_include_directories(CameraProjectionTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(CameraProjectionTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(CameraProjectionTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(CameraProjectionTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME CameraProjectionTests COMMAND CameraProjectionTests)

    # 速度バッファ（モーションベクター）と TAA ジッタの数式テスト。
    # 「静止時に速度が 0」「ジッタは NDC の平行移動」を守る。
    # renderer/TaaJitter.h はヘッダオンリー・GPU 非依存なので .cpp のビルドは不要。
    add_executable(VelocityMathTests velocity_math_test.cpp)
    target_include_directories(VelocityMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(VelocityMathTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(VelocityMathTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VelocityMathTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME VelocityMathTests COMMAND VelocityMathTests)

    # 時間方向アップスケーラ（DLSS）の純関数（位相数・ミップバイアス・ジッタのピクセル変換・品質モード）。
    # renderer/UpscalerMath.h はヘッダオンリー・GPU 非依存。
    add_executable(UpscalerMathTests upscaler_math_test.cpp)
    target_include_directories(UpscalerMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(UpscalerMathTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(UpscalerMathTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME UpscalerMathTests COMMAND UpscalerMathTests)

    # AnimationClip::NormalizeToSeconds（ticks→秒正規化。glTF=1000ticks/s 由来の
    # ポーズ凍結/スロー再生の再発防止）テスト。AnimationClip.cpp は純ロジックなので直接ビルド。
    add_executable(AnimClipNormalizeTests
        anim_clip_normalize_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimationClip.cpp)
    target_include_directories(AnimClipNormalizeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(AnimClipNormalizeTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(AnimClipNormalizeTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AnimClipNormalizeTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AnimClipNormalizeTests COMMAND AnimClipNormalizeTests)

    # PickRestClip（頂点の焼き込みと inverseRest が同じクリップを選ぶこと）のテスト。
    # 食い違うと inv(rest)*current の残差が余分な変換として絵に出る。
    add_executable(RestClipPickTests
        rest_clip_pick_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/NodeAnimationClip.cpp)
    target_include_directories(RestClipPickTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(RestClipPickTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(RestClipPickTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(RestClipPickTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME RestClipPickTests COMMAND RestClipPickTests)

    # Animator の再生速度倍率(SetSpeed。speed=2.0 で進行2倍、0 で一時停止)テスト。
    # Animator/Skeleton/AnimationClip は純ロジック(GPU非依存)なので直接ビルド。
    add_executable(AnimatorSpeedTests
        animator_speed_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Animator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimPose.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimationClip.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Skeleton.cpp)
    target_include_directories(AnimatorSpeedTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(AnimatorSpeedTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(AnimatorSpeedTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AnimatorSpeedTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AnimatorSpeedTests COMMAND AnimatorSpeedTests)

    # CrossFadeTo を毎フレーム同じ行き先で呼ぶ（ドキュメント推奨の書き方）と
    # ブレンドが永久に完了しなかった件の回帰テスト。
    add_executable(AnimatorCrossfadeTests
        animator_crossfade_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Animator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimPose.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimationClip.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Skeleton.cpp)
    target_include_directories(AnimatorCrossfadeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(AnimatorCrossfadeTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(AnimatorCrossfadeTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AnimatorCrossfadeTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AnimatorCrossfadeTests COMMAND AnimatorCrossfadeTests)

    # ポーズ空間ブレンド（animation/AnimPose.h）テスト。クロスフェードのバグ B1
    # 「最終スキニング行列を要素ごとに lerp すると骨が縮む」の回帰テストが本体。
    # AnimPose/Animator/Skeleton/AnimationClip は純ロジック（GPU非依存）なので直接ビルド。
    add_executable(AnimPoseTests
        anim_pose_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimPose.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Animator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimationClip.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Skeleton.cpp)
    target_include_directories(AnimPoseTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(AnimPoseTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(AnimPoseTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AnimPoseTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AnimPoseTests COMMAND AnimPoseTests)

    # アニメイベント収集（animation/AnimEvent.h）+ ステートマシン（AnimGraphAsset.h）テスト。
    # どちらも純関数（GPU/entt/Jolt 非依存）。AnimationClip.cpp だけ実体が要る。
    add_executable(AnimGraphTests
        anim_graph_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimationClip.cpp)
    target_include_directories(AnimGraphTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(AnimGraphTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(AnimGraphTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AnimGraphTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AnimGraphTests COMMAND AnimGraphTests)

# .animfsm のブレンドツリー宣言のパース検証。nlohmann を使う AnimGraphAsset.cpp を
# リンクするので、エンジン本体と一緒にビルドする構成でのみ有効
# （tests 単体構成は依存ゼロを保つ。上の SerializeRoundtripTests と同じ作法）。
if(TARGET Animation)
    add_executable(AnimBlendTreeAssetTests anim_blendtree_asset_test.cpp)
    target_link_libraries(AnimBlendTreeAssetTests PRIVATE Animation)
    target_include_directories(AnimBlendTreeAssetTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(AnimBlendTreeAssetTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AnimBlendTreeAssetTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AnimBlendTreeAssetTests COMMAND AnimBlendTreeAssetTests)
endif()

    # ボーンマスク（animation/BoneMask.h）+ レイヤー合成テスト。
    # 「マスク外のボーンが下位レイヤーとビット単位で同一」を守る。
    add_executable(BoneMaskTests
        bone_mask_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimPose.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimationClip.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Skeleton.cpp)
    target_include_directories(BoneMaskTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(BoneMaskTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(BoneMaskTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(BoneMaskTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME BoneMaskTests COMMAND BoneMaskTests)

    # 2 ボーン解析 IK（animation/TwoBoneIK.h）テスト。フット IK の心臓部。
    # 「解いたあと FK し直すと足首が目標に乗る」「ボーン長が保存される」を数値で守る。
    # ヘッダオンリー・純 DirectXMath。
    add_executable(TwoBoneIKTests two_bone_ik_test.cpp)
    target_include_directories(TwoBoneIKTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(TwoBoneIKTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(TwoBoneIKTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(TwoBoneIKTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME TwoBoneIKTests COMMAND TwoBoneIKTests)

    # Frustum（視錐台抽出＋球カリング）テスト。ヘッダオンリー・純 DirectXMath。
    add_executable(FrustumTests frustum_test.cpp)
    target_include_directories(FrustumTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(FrustumTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(FrustumTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(FrustumTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME FrustumTests COMMAND FrustumTests)

    # Hi-Z オクルージョンカリングの数学（renderer/HiZMath.h: AABB→スクリーン矩形投影 /
    # ミップ選択 / 遮蔽判定）テスト。ヘッダオンリー・純 DirectXMath。
    # 深度規約（標準 Z）と「選んだミップで矩形が 2x2 に収まる」不変条件を固定する。
    add_executable(HiZMathTests hiz_math_test.cpp)
    target_include_directories(HiZMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(HiZMathTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(HiZMathTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(HiZMathTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME HiZMathTests COMMAND HiZMathTests)

    # 拡張シェーディング（shaders/forward/BrdfExt.hlsli）の CPU 写し: 分布の正規化・相反性・白い炉のエネルギー上限。
    # ヘッダオンリー（renderer/ShadingExt.h）。GPU 不要。
    add_executable(ShadingBrdfTests shading_brdf_test.cpp)
    target_include_directories(ShadingBrdfTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(ShadingBrdfTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ShadingBrdfTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ShadingBrdfTests COMMAND ShadingBrdfTests)

    # 精密ピッキングの土台（editor/RayGeometry.h: レイ-三角形/AABB/球 + ヒット順ソート）テスト。
    # ヘッダオンリー・純 DirectXMath（entt も GPU も要らない）。
    add_executable(RayPickTests ray_pick_test.cpp)
    target_include_directories(RayPickTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(RayPickTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(RayPickTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(RayPickTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME RayPickTests COMMAND RayPickTests)

    # SnapshotBuffer(ネットワーク補間バッファ)テスト。ヘッダオンリー・純DirectXMath。
    add_executable(InterpolationTests interpolation_test.cpp)
    target_include_directories(InterpolationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(InterpolationTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(InterpolationTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(InterpolationTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME InterpolationTests COMMAND InterpolationTests)

    # RpcValue/RpcArgs(net:rpc引数)シリアライズ往復テスト。ヘッダオンリー・純DirectXMath。
    add_executable(RpcArgsTests rpc_args_test.cpp)
    target_include_directories(RpcArgsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(RpcArgsTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(RpcArgsTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(RpcArgsTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME RpcArgsTests COMMAND RpcArgsTests)

    # IsRelevant(興味管理の距離判定)テスト。ヘッダオンリー・純DirectXMath。
    add_executable(InterestFilterTests interest_filter_test.cpp)
    target_include_directories(InterestFilterTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(InterestFilterTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(InterestFilterTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(InterestFilterTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME InterestFilterTests COMMAND InterestFilterTests)
endif()

# 地形（ハイトフィールドのサンプリング / スカルプトブラシ / .hf 往復 / レイキャスト）テスト。
# HeightField.cpp / TerrainBrush.cpp は純ロジック（GPU も entt も vfs も不要）なので
# Terrain ライブラリをリンクせず直接ビルドする（CameraProjectionTests と同じ流儀）。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(TerrainTests
        terrain_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/HeightField.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/TerrainBrush.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/TerrainSplatMap.cpp)
    target_include_directories(TerrainTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(TerrainTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(TerrainTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(TerrainTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME TerrainTests COMMAND TerrainTests)
endif()

# ==== lane-C begin ====
# 新規シーンの太陽の既定（editor/LightMath.h の数式）。W05-22。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(NewSceneDefaultsTests new_scene_defaults_test.cpp)
    target_include_directories(NewSceneDefaultsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(NewSceneDefaultsTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(NewSceneDefaultsTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(NewSceneDefaultsTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME NewSceneDefaultsTests COMMAND NewSceneDefaultsTests)
endif()
# b1（PerFrameConstants）の CPU 側の定義のレイアウト表。W05-23。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(FrameConstantsLayoutTests frame_constants_layout_test.cpp)
    target_include_directories(FrameConstantsLayoutTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(FrameConstantsLayoutTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(FrameConstantsLayoutTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(FrameConstantsLayoutTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME FrameConstantsLayoutTests COMMAND FrameConstantsLayoutTests)
endif()
# slot11（クラスタのディスクリプタテーブル）の宣言表。W05-24。
if(TARGET Renderer)
    add_executable(ClusterTableLayoutTests cluster_table_layout_test.cpp)
    target_include_directories(ClusterTableLayoutTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(ClusterTableLayoutTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ClusterTableLayoutTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ClusterTableLayoutTests COMMAND ClusterTableLayoutTests)
endif()
# ステージフック / FeatureRegistry（ヘッダだけで完結）。W05-25。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    find_package(directx-headers CONFIG REQUIRED)
    add_executable(RenderStageHooksTests render_stage_hooks_test.cpp)
    target_include_directories(RenderStageHooksTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(RenderStageHooksTests PRIVATE Microsoft::DirectXMath Microsoft::DirectX-Headers)
    if(MSVC)
        target_compile_options(RenderStageHooksTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(RenderStageHooksTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME RenderStageHooksTests COMMAND RenderStageHooksTests)
endif()
# GpuTimer のスコープ表（64 本化・葉と親・別名）。GPU は使わない。W05-26。
if(TARGET Renderer)
    add_executable(GpuTimerScopeTests gpu_timer_scope_test.cpp)
    target_include_directories(GpuTimerScopeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(GpuTimerScopeTests PRIVATE Renderer)
    if(MSVC)
        target_compile_options(GpuTimerScopeTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(GpuTimerScopeTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME GpuTimerScopeTests COMMAND GpuTimerScopeTests)
endif()
# GpuScope / UNO_GPU_SCOPE（PIX イベント + GpuTimer を 1 行で）。GPU は使わない。W05-27。
if(TARGET Renderer)
    add_executable(GpuMarkerTests gpu_marker_test.cpp)
    target_include_directories(GpuMarkerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(GpuMarkerTests PRIVATE Renderer)
    if(MSVC)
        target_compile_options(GpuMarkerTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(GpuMarkerTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME GpuMarkerTests COMMAND GpuMarkerTests)
endif()
# 品質プリセットの口（生成表が読まれていること・既定値が従来どおり）。ヘッダだけで完結。W1-C1。
if(TARGET Renderer)
    add_executable(QualityPresetTableTests quality_preset_table_test.cpp)
    target_include_directories(QualityPresetTableTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(QualityPresetTableTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(QualityPresetTableTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME QualityPresetTableTests COMMAND QualityPresetTableTests)
endif()
# UNO_SHADING_V2 の軸（宣言表 ⇔ V1、結線、空の枠、ForwardGraph のキー）の静的な検査。GPU / DXC 不要。W1-C3。
add_executable(ShadingV2AxisTests shading_v2_axis_test.cpp)
target_compile_definitions(ShadingV2AxisTests PRIVATE DX12E_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
if(MSVC)
    target_compile_options(ShadingV2AxisTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
    target_compile_definitions(ShadingV2AxisTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME ShadingV2AxisTests COMMAND ShadingV2AxisTests)
# IBL 焼き直しのきっかけと撮影の待ちの判定表（ヘッダだけ）。W1-C9。
add_executable(IblRebakeTriggerTests ibl_rebake_trigger_test.cpp)
target_include_directories(IblRebakeTriggerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(IblRebakeTriggerTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
    target_compile_definitions(IblRebakeTriggerTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME IblRebakeTriggerTests COMMAND IblRebakeTriggerTests)
# MCP の純ロジック（シーンパス / create_entity の引数検査 / DLSS 警告。ヘッダのみ）
add_executable(McpLogicTests mcp_logic_test.cpp)
target_include_directories(McpLogicTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(McpLogicTests PRIVATE /W4 /WX /permissive- /utf-8)
endif()
add_test(NAME McpLogicTests COMMAND McpLogicTests)
# perf_stats の gpuFrameMs（p50/p95/p99/max）と throttleTier / alwaysRealtime の配線。FU-E-22。純ロジック
add_executable(PerfStatsGpuTests perf_stats_gpu_test.cpp)
target_include_directories(PerfStatsGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(PerfStatsGpuTests PRIVATE UI_LINT_ROOT="${DX12_REPO_ROOT}")
if(MSVC)
    target_compile_options(PerfStatsGpuTests PRIVATE /W4 /WX /permissive- /utf-8)
endif()
add_test(NAME PerfStatsGpuTests COMMAND PerfStatsGpuTests)
# 速度プリパスの前提（前ワールドの追跡 / 前ワールド VB の確保）。FU-E-23。純ロジック（ヘッダのみ）
add_executable(VelocityPolicyTests velocity_policy_test.cpp)
target_include_directories(VelocityPolicyTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(VelocityPolicyTests PRIVATE /W4 /WX /permissive- /utf-8)
endif()
add_test(NAME VelocityPolicyTests COMMAND VelocityPolicyTests)
# 時間を止めて 1 フレームずつ進める口（freeze_time / frame_step）の引数検証。ヘッダのみ。FU-E-24。
add_executable(FrameStepLogicTests frame_step_logic_test.cpp)
target_include_directories(FrameStepLogicTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(FrameStepLogicTests PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(FrameStepLogicTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME FrameStepLogicTests COMMAND FrameStepLogicTests)
# 水 / 植生 / VG が shadingRevision=2 のとき *V2_PS.cso を選ぶ純ロジック（ヘッダのみ）。FU-E-25。
add_executable(ShadingRevisionPickTests shading_revision_pick_test.cpp)
target_include_directories(ShadingRevisionPickTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(ShadingRevisionPickTests PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(ShadingRevisionPickTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME ShadingRevisionPickTests COMMAND ShadingRevisionPickTests)
# UNO_SHADING_V2 のマイクロシャドウ / GTSO / 地平線の CPU 写しと、シェーダとの配線の静的検査（ヘッダだけ。GPU 不要）。W1-C5。
add_executable(ShadingV2MathTests shading_v2_math_test.cpp)
target_include_directories(ShadingV2MathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(ShadingV2MathTests PRIVATE DX12E_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
if(MSVC)
    target_compile_options(ShadingV2MathTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
    target_compile_definitions(ShadingV2MathTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME ShadingV2MathTests COMMAND ShadingV2MathTests)
# Epic の動的 CSM（D16 x3、pancaking、受光側の法線オフセット、バイアス）の純関数と配線の静的検査（GPU 不要）。W1-C6。
add_executable(ShadowBiasMathTests shadow_bias_math_test.cpp)
target_include_directories(ShadowBiasMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(ShadowBiasMathTests PRIVATE DX12E_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
if(MSVC)
    target_compile_options(ShadowBiasMathTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
    target_compile_definitions(ShadowBiasMathTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME ShadowBiasMathTests COMMAND ShadowBiasMathTests)
# 品質プリセットの本体（計画 / 縮退 / 生成表との一致 / 予算の合計 / VRAM の上限 / Scene への適用の往復。GPU 不要）。W1-C7。
if(TARGET Scene)
    add_executable(QualityPresetTests quality_preset_test.cpp)
    target_link_libraries(QualityPresetTests PRIVATE Scene)
    target_include_directories(QualityPresetTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(QualityPresetTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(QualityPresetTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME QualityPresetTests COMMAND QualityPresetTests)
endif()
# ライティング・プリセットと SceneLookProfile の一本化（空 / IBL / 露出補正 / フォグ / 大気の時刻、往復、冪等。GPU 不要）。W1-C10。
if(TARGET Scene)
    add_executable(SceneLookProfileTests scene_look_profile_test.cpp)
    target_link_libraries(SceneLookProfileTests PRIVATE Scene)
    target_include_directories(SceneLookProfileTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(SceneLookProfileTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(SceneLookProfileTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SceneLookProfileTests COMMAND SceneLookProfileTests)
    # W1-FU-E-27: shadingRevision / V2 の欄 / LS-02a の欄の保存・読み込みの往復と Undo の写し。GPU 不要。
    add_executable(SceneShadingPersistTests scene_shading_persist_test.cpp)
    target_link_libraries(SceneShadingPersistTests PRIVATE Scene)
    target_include_directories(SceneShadingPersistTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(SceneShadingPersistTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(SceneShadingPersistTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SceneShadingPersistTests COMMAND SceneShadingPersistTests)
endif()
# ==== lane-C end ====
# ==== lane-D begin ====
# VFX-02a 焼き込み(tools/vfx_bake)の Python テスト。numpy / pytest が無い環境ではスキップ扱い。
# ゲートでは cmake -DVFX_BAKE_PYTHON=<pytest と numpy 入りの python> -DVFX_BAKE_REQUIRED=ON を渡すと、走らなければ失敗になる。
add_test(NAME VfxBakePyTests
    COMMAND ${CMAKE_COMMAND} -DVFX_BAKE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../tools/vfx_bake -DVFX_BAKE_PYTHON=${VFX_BAKE_PYTHON} -DVFX_BAKE_REQUIRED=${VFX_BAKE_REQUIRED} -P ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vfx_bake/run_pytests.cmake)
set_tests_properties(VfxBakePyTests PROPERTIES
    SKIP_REGULAR_EXPRESSION "VFXBAKE_SKIP"
    ENVIRONMENT "PYTHONUTF8=1"
    TIMEOUT 900)
# ==== lane-D end ====

# 頂点スカルプト（素体生成 / 溶接・隣接 / 各ブラシ / 法線の部分再計算 / 対称 / .smsh 往復）テスト。
# SculptMesh.cpp も純ロジック（重み関数とノイズだけ TerrainBrush と共有）なので、
# TerrainTests と同じくライブラリをリンクせず直接ビルドする。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(SculptTests
        sculpt_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/SculptMesh.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/TerrainBrush.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/HeightField.cpp)
    target_include_directories(SculptTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(SculptTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(SculptTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SculptTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SculptTests COMMAND SculptTests)
endif()

# SpatialIndex（一様グリッド）テスト（Phase 5: RTS 空間クエリ）。ヘッダオンリー、entt のみ依存。
if(TARGET ECS)
    find_package(EnTT CONFIG REQUIRED)
    add_executable(SpatialIndexTests spatial_index_test.cpp)
    target_include_directories(SpatialIndexTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(SpatialIndexTests PRIVATE EnTT::EnTT)
    if(MSVC)
        target_compile_options(SpatialIndexTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SpatialIndexTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SpatialIndexTests COMMAND SpatialIndexTests)
endif()

# EventBus（汎用イベントバス）テスト（Phase 3 中核）。ヘッダオンリー、entt のみ依存。
if(TARGET ECS)
    find_package(EnTT CONFIG REQUIRED)
    add_executable(EventBusTests event_bus_test.cpp)
    target_include_directories(EventBusTests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(EventBusTests PRIVATE EnTT::EnTT)
    if(MSVC)
        target_compile_options(EventBusTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(EventBusTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME EventBusTests COMMAND EventBusTests)
endif()

# ActionMap（データ駆動入力）テスト（Phase 4 能力カタログ）。ヘッダオンリー、DirectXMath のみ依存。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(ActionMapTests action_map_test.cpp)
    target_include_directories(ActionMapTests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(ActionMapTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(ActionMapTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ActionMapTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ActionMapTests COMMAND ActionMapTests)
endif()

# VFS（Crypto / Compression / Pak）往復テスト（GPU 非依存）。Core（+ bcrypt/cabinet）をリンク。
# Core は STATIC なので PRIVATE 依存（bcrypt.lib/Cabinet.lib）も実行ファイルへ伝播する。
if(TARGET Core)
    add_executable(VfsTests vfs_test.cpp)
    target_link_libraries(VfsTests PRIVATE Core)
    target_include_directories(VfsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(VfsTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VfsTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME VfsTests COMMAND VfsTests)
endif()

# CrashHandler 統合テスト（子プロセスをわざとアクセス違反で落とし、
# dx12_crash.log / dx12_crash.dmp の生成とレポート内容を確認）。
if(TARGET Core)
    add_executable(CrashHandlerTests crash_handler_test.cpp)
    target_link_libraries(CrashHandlerTests PRIVATE Core)
    target_include_directories(CrashHandlerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(CrashHandlerTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(CrashHandlerTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME CrashHandlerTests COMMAND CrashHandlerTests)
endif()

# GitIntegration::ToWebUrl（remote URL → ブラウザ用 https URL）の文字列変換テスト。
# プレフィックス文字数のオフバイワン再発防止（実際に "ryuto-alt" → "yuto-alt" になった事例）。
if(TARGET ProjectLib)
    add_executable(GitIntegrationTests git_integration_test.cpp)
    target_link_libraries(GitIntegrationTests PRIVATE ProjectLib)
    if(MSVC)
        target_compile_options(GitIntegrationTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(GitIntegrationTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME GitIntegrationTests COMMAND GitIntegrationTests)
endif()

# ENet ループバック疎通テスト（同一プロセス内でサーバー/クライアントを立てて往復確認）。
# Network（+ vcpkg unofficial-enet, Core）が必要なので統合ビルドでのみ有効。
if(TARGET Network)
    add_executable(EnetLoopbackTests enet_loopback_test.cpp)
    target_link_libraries(EnetLoopbackTests PRIVATE Network)
    if(MSVC)
        target_compile_options(EnetLoopbackTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(EnetLoopbackTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME EnetLoopbackTests COMMAND EnetLoopbackTests)
endif()

# editor/LightMath.h（太陽の方位/高度 ⇔ direction、時刻カーブ、色温度、コーン角、
# ワールド→スクリーン投影、レイ幾何）のテスト。シーンビューのライト直接操作の心臓部。
# ヘッダオンリー・純 DirectXMath（ImGui も entt も要らない）。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(LightMathTests light_math_test.cpp)
    target_include_directories(LightMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(LightMathTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(LightMathTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(LightMathTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME LightMathTests COMMAND LightMathTests)
endif()

# renderer/SplitScreenLayout.h（画面分割の矩形計算）のテスト。ヘッダオンリー。
if(TARGET Renderer)
    add_executable(SplitScreenLayoutTests split_screen_layout_test.cpp)
    target_include_directories(SplitScreenLayoutTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(SplitScreenLayoutTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SplitScreenLayoutTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SplitScreenLayoutTests COMMAND SplitScreenLayoutTests)
endif()

# scripting/ScriptUdp（Lua net.udpOpen の 127.0.0.1 専用 UDP）。ループバックで 2 本つないで往復する。
if(TARGET Scripting)
    add_executable(ScriptUdpTests script_udp_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/scripting/ScriptUdp.cpp)
    target_include_directories(ScriptUdpTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(ScriptUdpTests PRIVATE ws2_32.lib)
    if(MSVC)
        target_compile_options(ScriptUdpTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ScriptUdpTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ScriptUdpTests COMMAND ScriptUdpTests)
endif()

# スタブ .vgeo 生成器（tools/vgeo_stub/VgeoStubGen.h）の出力が VgeoFormat の検証を通ること・往復・決定論。
# meshoptimizer が要るので統合ビルドでのみ有効（Renderer が meshoptimizer をリンクしている）。
if(TARGET Renderer)
    add_executable(VgeoStubTests vgeo_stub_test.cpp)
    target_include_directories(VgeoStubTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vgeo_stub)
    target_link_libraries(VgeoStubTests PRIVATE meshoptimizer::meshoptimizer)
    if(MSVC)
        target_compile_options(VgeoStubTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VgeoStubTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    # meshoptimizer.dll をテスト exe の隣へ（コピー先 = tests/ の出力先。ルートの DLL 配布とは独立に単体ビルドでも動くように）
    add_custom_command(TARGET VgeoStubTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:VgeoStubTests>")
    add_test(NAME VgeoStubTests COMMAND VgeoStubTests)
endif()

# 仮想ジオメトリ P1: オフライン cooker（tools/vgeo_cook）のテスト。クラックなし（距離スイープ + ランダムな一貫カット + 陰性対照）/ 誤差の単調性 /
# クラスタ上限 / LOD0 カバレッジ / 決定論（スレッド数 1・2・5・8）/ 検証 / GPU 写像（仕様 §14 の HLSL デコードの移植）/ 回帰（縮退・非多様体・
# 継ぎ目・極端な座標・NONVG）/ 不正入力 / グループ化 4 方式 / ベンチ生成 / スタブ互換。meshoptimizer が要るので統合ビルドでのみ有効。
if(TARGET Renderer AND TARGET VgeoCookLib)
    add_executable(VgeoCookTests vgeo_cook_test.cpp)
    target_include_directories(VgeoCookTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vgeo_stub ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vgeo_cook)
    target_link_libraries(VgeoCookTests PRIVATE VgeoCookLib meshoptimizer::meshoptimizer)
    target_compile_definitions(VgeoCookTests PRIVATE DX12E_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    if(MSVC)
        target_compile_options(VgeoCookTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VgeoCookTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_custom_command(TARGET VgeoCookTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:VgeoCookTests>")
    add_test(NAME VgeoCookTests COMMAND VgeoCookTests)
    # 診断用: cooker 本体ごと AddressSanitizer で回す（範囲外アクセス検出）。既定ビルドには入れない（EXCLUDE_FROM_ALL）。
    #   pwsh tools/build.ps1 -Target VgeoCookTestsAsan → build/release/tests/VgeoCookTestsAsan.exe（実行は vcvars64 済みのシェルで）
    if(MSVC)
        add_executable(VgeoCookTestsAsan EXCLUDE_FROM_ALL vgeo_cook_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vgeo_cook/VgeoCook.cpp)
        target_include_directories(VgeoCookTestsAsan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vgeo_stub ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vgeo_cook)
        target_link_libraries(VgeoCookTestsAsan PRIVATE meshoptimizer::meshoptimizer psapi)
        target_compile_options(VgeoCookTestsAsan PRIVATE ${DX12_COMPILE_OPTIONS} /fsanitize=address)
        target_compile_definitions(VgeoCookTestsAsan PRIVATE ${DX12_COMPILE_DEFS} _CRT_SECURE_NO_WARNINGS DX12E_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    endif()
endif()

# 仮想ジオメトリ P2: GPU カリング。
#   VgLodMathTests … LOD 数式 / 錐台 / コーン / 変換の純関数（GPU 不要。親の射影誤差 >= 子 の単調性を実 DAG で確かめる）
#   VgCullGpuTests … 窓なしの D3D12 デバイスで compute を実行し、GPU の可視クラスタ集合 = CPU 参照（VgCullReference.h）を確かめる。
#                    GPU が無い / SM 6.6・Tier 3 が無い環境では「SKIP」を出して 0 で終わる。シェーダ（.cso）は Shaders ターゲットが作る。
if(TARGET Renderer AND TARGET VgeoCookLib)
    add_executable(VgLodMathTests vg_lod_math_test.cpp)
    target_include_directories(VgLodMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vgeo_cook)
    target_link_libraries(VgLodMathTests PRIVATE VgeoCookLib meshoptimizer::meshoptimizer Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(VgLodMathTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VgLodMathTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_custom_command(TARGET VgLodMathTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:VgLodMathTests>")
    add_test(NAME VgLodMathTests COMMAND VgLodMathTests)

    add_executable(VgCullGpuTests vg_cull_gpu_test.cpp)
    target_include_directories(VgCullGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vgeo_cook)
    target_link_libraries(VgCullGpuTests PRIVATE Renderer VgeoCookLib meshoptimizer::meshoptimizer Microsoft::DirectXMath)
    target_compile_definitions(VgCullGpuTests PRIVATE DX12E_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
    if(TARGET Shaders)
        add_dependencies(VgCullGpuTests Shaders)
    endif()
    if(MSVC)
        target_compile_options(VgCullGpuTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VgCullGpuTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_custom_command(TARGET VgCullGpuTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:VgCullGpuTests>")
    add_test(NAME VgCullGpuTests COMMAND VgCullGpuTests)
    set_tests_properties(VgCullGpuTests PROPERTIES LABELS gpu)

    # 仮想ジオメトリ P3: メッシュシェーダのラスタ。GPU の可視性バッファ + 深度 = CPU の三角形ラスタ / 全 LOD0 の素朴な参照（穴・はみ出しの検出）。
    #   GPU が無い / メッシュシェーダ Tier 1 が無い環境では「SKIP」を出して 0 で終わる。
    add_executable(VgRasterGpuTests vg_raster_gpu_test.cpp)
    target_include_directories(VgRasterGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vgeo_cook)
    target_link_libraries(VgRasterGpuTests PRIVATE Renderer VgeoCookLib meshoptimizer::meshoptimizer Microsoft::DirectXMath)
    target_compile_definitions(VgRasterGpuTests PRIVATE DX12E_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
    if(TARGET Shaders)
        add_dependencies(VgRasterGpuTests Shaders)
    endif()
    if(MSVC)
        target_compile_options(VgRasterGpuTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VgRasterGpuTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_custom_command(TARGET VgRasterGpuTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:VgRasterGpuTests>")
    add_test(NAME VgRasterGpuTests COMMAND VgRasterGpuTests)
    set_tests_properties(VgRasterGpuTests PROPERTIES LABELS gpu)

    # 仮想ジオメトリ P4: 可視性バッファからの属性復元（重心座標・1 画素差分・UV 勾配 = resolve の SampleGrad の入力）/ G-Buffer + 速度 /
    #   決定論（stableOrder で 2 回の可視性バッファがビット一致）/ 両面材質のコーン棄却。CPU の独立参照（レイと三角形の交点）と比較。
    add_executable(VgResolveGpuTests vg_resolve_gpu_test.cpp)
    target_include_directories(VgResolveGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vgeo_cook)
    target_link_libraries(VgResolveGpuTests PRIVATE Renderer VgeoCookLib meshoptimizer::meshoptimizer Microsoft::DirectXMath)
    target_compile_definitions(VgResolveGpuTests PRIVATE DX12E_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
    if(TARGET Shaders)
        add_dependencies(VgResolveGpuTests Shaders)
    endif()
    if(MSVC)
        target_compile_options(VgResolveGpuTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VgResolveGpuTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_custom_command(TARGET VgResolveGpuTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:VgResolveGpuTests>")
    add_test(NAME VgResolveGpuTests COMMAND VgResolveGpuTests)
    set_tests_properties(VgResolveGpuTests PROPERTIES LABELS gpu)
endif()

# モデルのインポートサイズ回帰テスト（FBX の cm→m 正規化）。ModelLoader::Probe は GPU を
# 触らないが Resource ライブラリ(assimp)が要るので統合ビルドでのみ有効。
if(TARGET Resource AND TARGET Renderer)
    add_executable(ModelScaleTests model_scale_test.cpp)
    target_link_libraries(ModelScaleTests PRIVATE Resource Renderer)
    target_compile_definitions(ModelScaleTests PRIVATE
        DX12E_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    if(MSVC)
        target_compile_options(ModelScaleTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ModelScaleTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ModelScaleTests COMMAND ModelScaleTests)
endif()

# 仮想ジオメトリの自動 cook（resource/VgAutoCook）と .vgeo v1.1（AO / 拡張シェーディング 'MSHD'）。GPU 不要。
if(TARGET Resource AND TARGET Renderer AND TARGET VgeoCookLib)
    add_executable(VgAutoCookTests vg_auto_cook_test.cpp)
    target_link_libraries(VgAutoCookTests PRIVATE Resource Renderer VgeoCookLib)
    target_compile_definitions(VgAutoCookTests PRIVATE DX12E_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    if(MSVC)
        target_compile_options(VgAutoCookTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VgAutoCookTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_custom_command(TARGET VgAutoCookTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:VgAutoCookTests>")
    add_test(NAME VgAutoCookTests COMMAND VgAutoCookTests)
endif()

# モデル取り込み: import.json（ModelImportOptions）と albedo アルファの透明判定（TextureLoader::HasTransparentAlpha）。GPU 不要。
if(TARGET Resource AND TARGET Renderer)
    add_executable(ModelImportAlphaTests model_import_alpha_test.cpp)
    target_link_libraries(ModelImportAlphaTests PRIVATE Resource Renderer)
    target_compile_definitions(ModelImportAlphaTests PRIVATE
        DX12E_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    if(MSVC)
        target_compile_options(ModelImportAlphaTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ModelImportAlphaTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ModelImportAlphaTests COMMAND ModelImportAlphaTests)
endif()

# 拡張シェーディング（クリアコート / 異方性 / シーン / スペキュラ・IOR）: Pack/Unpack・Resolve・JSON・glTF 取り込み（Blender 製 glb）。GPU 不要。
if(TARGET Resource AND TARGET Renderer)
    add_executable(ShadingExtTests shading_ext_test.cpp)
    target_link_libraries(ShadingExtTests PRIVATE Resource Renderer)
    target_compile_definitions(ShadingExtTests PRIVATE
        DX12E_SHADING_TEST_GLB="${CMAKE_CURRENT_SOURCE_DIR}/data/shading_ext.glb")
    if(MSVC)
        target_compile_options(ShadingExtTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ShadingExtTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ShadingExtTests COMMAND ShadingExtTests)
endif()

# マテリアル AO（ORM の R / glTF occlusionTexture）。ModelLoader::ProbeOcclusion / TextureLoader::ComposeOrmPng / Material.h の詰め方。GPU 不要。
if(TARGET Resource AND TARGET Renderer)
    add_executable(AoMaterialTests ao_material_test.cpp)
    target_link_libraries(AoMaterialTests PRIVATE Resource Renderer)
    target_compile_definitions(AoMaterialTests PRIVATE
        DX12E_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    if(MSVC)
        target_compile_options(AoMaterialTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AoMaterialTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AoMaterialTests COMMAND AoMaterialTests)
endif()

# MCP ディスパッチ表（mcp/ApplicationMcp*.cpp の McpDefine）の「申告キー表 ⊇ 本文が読むキー」を
# 機械的に見張るテスト。dx12_describe_mcp_params が嘘をつかないことの保証（#20-7 / #30）。
# エンジンをリンクせずソースをテキストとして読むだけ。依存ゼロ。
# ★登録はテーマ別 TU に散っているので '|' 区切りの一覧を渡す。ここを手書きの列挙にすると
#   TU を 1 本新設した瞬間にその中の登録を黙って見逃す（TS 側の schemaDrift.test.ts が
#   同じ理由で決め打ちをやめている）。CONFIGURE_DEPENDS でファイル追加時に再 glob させる。
file(GLOB DX12E_MCP_TUS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/mcp/ApplicationMcp*.cpp")
if(NOT DX12E_MCP_TUS)
    message(FATAL_ERROR "McpParamSpecTests: src/core/mcp/ApplicationMcp*.cpp が 1 本も見つからない")
endif()
list(JOIN DX12E_MCP_TUS "|" DX12E_MCP_SOURCES)
# ★/utf-8 を明示する。DX12_COMPILE_OPTIONS はルート CMakeLists が定義するので、
# CI と同じ `cmake -S tests -B build-tests`（テストツリー単体）では**空**になる。
# ソースは UTF-8 で日本語コメントを含むため、既定コードページが CP932 の環境では
# マルチバイトの後続バイトが `{` を飲み込んで C2447 でビルドできなくなる
# （英語ロケールの CI は CP1252 で 1 バイト単位なので偶然通っていた）。
add_executable(McpParamSpecTests mcp_param_spec_test.cpp)
target_compile_definitions(McpParamSpecTests PRIVATE
    DX12E_APPLICATION_CPP="${DX12E_MCP_SOURCES}")
if(MSVC)
    target_compile_options(McpParamSpecTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
    target_compile_definitions(McpParamSpecTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME McpParamSpecTests COMMAND McpParamSpecTests)

# MCP マニフェスト（method ごとのメタ情報）の整合性: McpDefine の全 method = データ表（ApplicationMcpManifestData.inc）、
# 各 meta の妥当性、FNV-1a / paramSpec パーサ / 近い名前の提案 / McpErr のコード値。
# McpParamSpecTests と同じくソースをテキストで読むだけ＋依存ゼロのヘッダ（McpMeta.h / McpManifestBuild.h）と
# 生成物のデータ表を include するので、エンジンをリンクしない。
add_executable(McpManifestTests mcp_manifest_test.cpp)
target_include_directories(McpManifestTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(McpManifestTests PRIVATE
    DX12E_MCP_SOURCES="${DX12E_MCP_SOURCES}"
    DX12E_INTERNAL_H="${CMAKE_CURRENT_SOURCE_DIR}/../src/core/ApplicationInternal.h")
if(MSVC)
    target_compile_options(McpManifestTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(McpManifestTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME McpManifestTests COMMAND McpManifestTests)

# MCP の副作用の安全性（M5）: 冪等ストア / guarded 確認トークン（core/mcp/McpSafety.h）と、ファイル書き込みジャーナル（core/mcp/McpJournal.cpp）。
# ヘッダオンリー + std::filesystem だけなのでエンジンをリンクしない。時計を注入して決定論。
add_executable(McpSafetyTests mcp_safety_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/mcp/McpJournal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/AtomicFile.cpp)   # McpJournal の保存が原子的書き込みを使う
target_include_directories(McpSafetyTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(McpSafetyTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(McpSafetyTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME McpSafetyTests COMMAND McpSafetyTests)

# 原子的な書き込み（core/AtomicFile.cpp）: ディスク満杯・フラッシュ/検証/置き換えの失敗・読み取り専用・ロック・
# 複数ファイルのコミット途中の失敗と死（ロールバック / RecoverPending）。元のファイルが無傷で一時ファイルが残らないこと。
add_executable(AtomicFileTests atomic_file_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/AtomicFile.cpp)
target_include_directories(AtomicFileTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(AtomicFileTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(AtomicFileTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME AtomicFileTests COMMAND AtomicFileTests)

# MCP（AI）の編集を Undo に積む規則（editor/McpUndoRouter.h + editor/UndoCore.h）。
# 呼び出し 1 回 = 「AI: <method>」1 エントリ / onlyAi は人の編集を戻さない /
# トランザクションは 1 エントリ・rollback は逆順。ヘッダオンリー・依存ゼロなのでスタンドアロンでも動く。
add_executable(McpUndoTests mcp_undo_test.cpp)
target_include_directories(McpUndoTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(McpUndoTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(McpUndoTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME McpUndoTests COMMAND McpUndoTests)

# フリート運用の自己終了（core/mcp/FleetGuard.h: --owner-pid / --idle-exit）。時計とプロセス生存判定を差し替えて検査する。
# ヘッダオンリー・エンジン非リンク。
add_executable(FleetGuardTests fleet_guard_test.cpp)
target_include_directories(FleetGuardTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(FleetGuardTests PRIVATE /W4 /WX /permissive- /utf-8)
    target_link_libraries(FleetGuardTests PRIVATE d3d12 dxgi)
else()
    target_compile_options(FleetGuardTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME FleetGuardTests COMMAND FleetGuardTests)

# Lua 束縛 ⇔ dx12_describe_lua_api のドリフト検出（display / net がテーブルごと
# 未ドキュメントだったのを二度と起こさないため）。McpParamSpecTests と同じく
# ソースをテキストとして読むだけなのでエンジンをリンクしない。
# ★束縛は ScriptEngine.cpp 以外（ScriptAudioBindings.cpp など担当別の TU）にもあるので、
#   src/scripting/*.cpp を全部 '|' 区切りで渡す。決め打ちにすると新しい TU の束縛を黙って見逃す。
file(GLOB DX12E_SCRIPTING_TUS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/../src/scripting/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/../src/scripting/bind/*.cpp")
list(JOIN DX12E_SCRIPTING_TUS "|" DX12E_SCRIPTING_SOURCES)
add_executable(LuaApiDocTests lua_api_doc_test.cpp)
target_compile_definitions(LuaApiDocTests PRIVATE
    DX12E_SCRIPT_ENGINE_CPP="${DX12E_SCRIPTING_SOURCES}"
    DX12E_APP_INTERNAL_CPP="${CMAKE_CURRENT_SOURCE_DIR}/../src/core/ApplicationInternal.cpp")
if(MSVC)
    target_compile_options(LuaApiDocTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
    target_compile_definitions(LuaApiDocTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME LuaApiDocTests COMMAND LuaApiDocTests)

# シェーダーホットリロードの手書き依存表 ⇔ 実際の #include のドリフト検出。
# 表から漏れると「保存しても再コンパイルされないのに、別ファイルの成功ログだけ出る」。
add_executable(ShaderIncludeDepsTests shader_include_deps_test.cpp)
target_compile_definitions(ShaderIncludeDepsTests PRIVATE
    DX12E_SHADER_REGISTRY_CPP="${CMAKE_CURRENT_SOURCE_DIR}/../src/resource/ShaderRegistry.cpp"
    DX12E_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shaders")
if(MSVC)
    target_compile_options(ShaderIncludeDepsTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
    target_compile_definitions(ShaderIncludeDepsTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME ShaderIncludeDepsTests COMMAND ShaderIncludeDepsTests)

# マテリアルグラフ G2a: フォワードのシェーディング尾部（shaders/forward/ForwardShade.hlsli）の検査。GPU 不要。
#   ・UnoSurface.hlsli の契約構造体 = G1 の参照実装（UnoMatContractRef.hlsli）
#   ・Forward / ForwardSkinned / Terrain に尾部の複製が復活していない
#   ・ShaderRegistry の全バリアント + ALPHA_TEST 派生 + G2b 予行 probe（ps_6_6 + ResourceDescriptorHeap[]）が DXC を通る
#   dxcompiler.dll は実行時ロード（無い環境では DXC の検査だけスキップして 0）。ctest ラベル: dxc
#   DXIL が外出し前後で同値かは tools/shader_dxil_equiv.ps1（変更前ツリーが要るので ctest 外。手順: docs/MATGRAPH_G2A.md）。
if(TARGET Microsoft::DirectXShaderCompiler)
    get_target_property(FORWARD_SHADE_DXC_INC Microsoft::DirectXShaderCompiler INTERFACE_INCLUDE_DIRECTORIES)
    add_executable(ForwardShadeTests forward_shade_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/resource/ShaderRegistry.cpp)
    target_include_directories(ForwardShadeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(ForwardShadeTests SYSTEM PRIVATE ${FORWARD_SHADE_DXC_INC})
    target_compile_definitions(ForwardShadeTests PRIVATE
        DX12E_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shaders"
        DX12E_MATGRAPH_HLSL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../src/renderer/matgraph/hlsl"
        DX12E_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    if(MSVC)
        target_compile_options(ForwardShadeTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(ForwardShadeTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    if(DIRECTX_DXC_TOOL)
        get_filename_component(FORWARD_SHADE_DXC_DIR "${DIRECTX_DXC_TOOL}" DIRECTORY)
        add_custom_command(TARGET ForwardShadeTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FORWARD_SHADE_DXC_DIR}/dxcompiler.dll" "${FORWARD_SHADE_DXC_DIR}/dxil.dll" "$<TARGET_FILE_DIR:ForwardShadeTests>"
            COMMENT "Bundling dxcompiler.dll / dxil.dll next to ForwardShadeTests.exe")
    endif()
    add_test(NAME ForwardShadeTests COMMAND ForwardShadeTests)
    set_tests_properties(ForwardShadeTests PROPERTIES LABELS dxc TIMEOUT 300)
endif()

# マテリアルグラフ G2a: メイン RS のバインドレスフラグの実機検証（フラグ / DWORD / 既存 PSO / probe PSO / SRV 添字の永続性 + 実 GPU で
# ResourceDescriptorHeap[添字] を引く）。ハードウェアアダプタが要る。GPU が無い / SM 6.6 + Tier 3 が無い環境では SKIP して 0 で終わる。
# dxcompiler.dll は Resource が delay-load でリンクしているのを同じフォルダへ置く。ctest ラベル: gpu
if(TARGET Renderer AND TARGET Resource AND TARGET Microsoft::DirectXShaderCompiler)
    get_target_property(MAIN_RS_DXC_INC Microsoft::DirectXShaderCompiler INTERFACE_INCLUDE_DIRECTORIES)
    add_executable(MainRsBindlessTests main_rs_bindless_test.cpp)
    # Core の Window.cpp が InputSystem を参照する（GraphicsDevice::Initialize が Window& を取るため Window を作る）
    target_link_libraries(MainRsBindlessTests PRIVATE Renderer Resource Input)
    target_include_directories(MainRsBindlessTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(MainRsBindlessTests SYSTEM PRIVATE ${MAIN_RS_DXC_INC})
    target_compile_definitions(MainRsBindlessTests PRIVATE
        DX12E_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shaders"
        DX12E_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    if(MSVC)
        target_compile_options(MainRsBindlessTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(MainRsBindlessTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    if(DIRECTX_DXC_TOOL)
        get_filename_component(MAIN_RS_DXC_DIR "${DIRECTX_DXC_TOOL}" DIRECTORY)
        add_custom_command(TARGET MainRsBindlessTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MAIN_RS_DXC_DIR}/dxcompiler.dll" "${MAIN_RS_DXC_DIR}/dxil.dll" "$<TARGET_FILE_DIR:MainRsBindlessTests>"
            COMMENT "Bundling dxcompiler.dll / dxil.dll next to MainRsBindlessTests.exe")
    endif()
    add_test(NAME MainRsBindlessTests COMMAND MainRsBindlessTests)
    set_tests_properties(MainRsBindlessTests PROPERTIES LABELS gpu TIMEOUT 300)

    # マテリアルグラフ G2b: GraphMaterialSystem の実機検証（初回ビルドの非同期 / パラメータプールのオフセット / 値の編集は再コンパイルしない /
    # 構造の編集は旧版のまま新版へ / コンパイルエラーで描画が止まらない + nodeId 逆引き / 変種の共有 / DXIL ディスクキャッシュ）。
    add_executable(GraphMaterialTests graph_material_test.cpp)
    target_link_libraries(GraphMaterialTests PRIVATE Renderer Resource Input MatGraph)
    target_include_directories(GraphMaterialTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(GraphMaterialTests PRIVATE DX12E_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shaders")
    if(MSVC)
        target_compile_options(GraphMaterialTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(GraphMaterialTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    if(DIRECTX_DXC_TOOL)
        get_filename_component(GRAPH_MAT_DXC_DIR "${DIRECTX_DXC_TOOL}" DIRECTORY)
        add_custom_command(TARGET GraphMaterialTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GRAPH_MAT_DXC_DIR}/dxcompiler.dll" "${GRAPH_MAT_DXC_DIR}/dxil.dll" "$<TARGET_FILE_DIR:GraphMaterialTests>"
            COMMENT "Bundling dxcompiler.dll / dxil.dll next to GraphMaterialTests.exe")
    endif()
    add_test(NAME GraphMaterialTests COMMAND GraphMaterialTests)
    set_tests_properties(GraphMaterialTests PROPERTIES LABELS gpu TIMEOUT 300)
endif()

# オートセーブ退避スロット（core/AutosaveSlot.h）のテスト。ヘッダオンリーだが
# meta.json の読み取りに nlohmann を使うので、統合ビルドでのみ有効（Core をリンク）。
if(TARGET Core)
    add_executable(AutosaveSlotTests autosave_test.cpp)
    target_link_libraries(AutosaveSlotTests PRIVATE Core)
    target_include_directories(AutosaveSlotTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(AutosaveSlotTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AutosaveSlotTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AutosaveSlotTests COMMAND AutosaveSlotTests)
endif()

# 自作ナビメッシュ（ボクセル化 → 領域分割 → 輪郭 → ポリゴン化 → A*/ファネル）のテスト。
# nav/ は純ロジック（GPU も entt も DirectXMath も vfs も不要）なので、
# TerrainTests と同じくライブラリをリンクせず .cpp を直接ビルドする＝スタンドアロン CI でも動く。
add_executable(NavMeshTests
    navmesh_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavRasterize.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavRegion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavContour.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavPolyMesh.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavMesh.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavQuery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavCorridor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavBuilder.cpp)
target_include_directories(NavMeshTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(NavMeshTests PRIVATE /W4 /WX /permissive- /utf-8 /bigobj)
    # 生成の内部トレースが要るときは DX12_NAV_TRACE を足す（段階ごとの件数が stdout に出る）
else()
    target_compile_options(NavMeshTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME NavMeshTests COMMAND NavMeshTests)

# 群衆（NavCrowd: 操舵・分離・速度サンプリング・押し出し・通路の上で確定）のテスト。
# NavMeshTests と同じく nav/ の .cpp を直接ビルドする（依存ゼロ）。
add_executable(NavCrowdTests
    nav_crowd_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavRasterize.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavRegion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavContour.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavPolyMesh.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavMesh.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavQuery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavCorridor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavCrowd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nav/NavBuilder.cpp)
target_include_directories(NavCrowdTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(NavCrowdTests PRIVATE /W4 /WX /permissive- /utf-8 /bigobj)
else()
    target_compile_options(NavCrowdTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME NavCrowdTests COMMAND NavCrowdTests)

# ゲーム AI（Brain）の純ロジック（黒板 / 応答カーブ / ユーティリティ評価 / 知覚の判定 / 記憶 / 乱数）。
# ECS も Lua も物理も要らないので .cpp を直接ビルドする。
add_executable(AiBrainTests
    ai_brain_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ai/Blackboard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ai/Utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ai/Perception.cpp)
target_include_directories(AiBrainTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(AiBrainTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(AiBrainTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME AiBrainTests COMMAND AiBrainTests)






# セーブファイル（core/save/SaveFile.h / UserData.h）: スロット名の検査・往復・一覧・フォルダ名。
# ファイル操作は偽物（メモリ上）で回して失敗を注入する。実ファイルでも 1 往復だけ確かめる。
if(TARGET Core)
    add_executable(SaveFileTests save_file_test.cpp)
    target_link_libraries(SaveFileTests PRIVATE Core)
    target_include_directories(SaveFileTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(SaveFileTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SaveFileTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SaveFileTests COMMAND SaveFileTests)
endif()

# Lua の値 ⇔ JSON（scripting/LuaJson.h）の往復。セーブ本体の心臓部。
# LuaJson.cpp は Lua / sol2 / nlohmann / DirectXMath だけに依存するので、Scripting を丸ごと
# リンクせず直接ビルドする（エンジンの他の部分を巻き込まない）。統合ビルドでのみ有効。
if(TARGET Scripting)
    find_package(Lua REQUIRED)
    find_package(sol2 CONFIG REQUIRED)
    find_package(directxmath CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    add_executable(LuaJsonTests lua_json_test.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../src/scripting/LuaJson.cpp)
    target_include_directories(LuaJsonTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${LUA_INCLUDE_DIR})
    target_link_libraries(LuaJsonTests PRIVATE ${LUA_LIBRARIES} sol2::sol2 Microsoft::DirectXMath
                                               nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(LuaJsonTests PRIVATE ${DX12_COMPILE_OPTIONS} /bigobj)
        target_compile_definitions(LuaJsonTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME LuaJsonTests COMMAND LuaJsonTests)
endif()

# ============================================================================
# 仮想入力モード（AI が OS のカーソル/フォーカス/キーボードを奪わずエディタ UI を操作する仕組み）
# ----------------------------------------------------------------------------
# VirtualInputTests … 純粋ロジック（input/VirtualInput.h + core/BackgroundMode.h）。
#   キューの順序 / down と up の間隔 / drag の補間 / 座標変換 / 実入力メッセージの遮断判定 /
#   キー名の解釈 / --background の値の解釈。ヘッダオンリー・依存ゼロなのでスタンドアロンでも回る。
# VirtualInputImGuiTests … 実際の ImGui コンテキスト（窓も GPU も無し）に仮想入力を流し込み、
#   クリック / キー / 文字入力 / ドラッグ / ホイール / 実入力の破棄 / 仮想カーソル描画 / find を確かめる。
#   Gui（ImGui）が要るので統合ビルドでのみ有効。
# ============================================================================
add_executable(VirtualInputTests virtual_input_test.cpp)
target_include_directories(VirtualInputTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(VirtualInputTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(VirtualInputTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME VirtualInputTests COMMAND VirtualInputTests)

if(TARGET Gui)
    add_executable(VirtualInputImGuiTests virtual_input_imgui_test.cpp)
    target_include_directories(VirtualInputImGuiTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(VirtualInputImGuiTests PRIVATE Gui)
    if(MSVC)
        target_compile_options(VirtualInputImGuiTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VirtualInputImGuiTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME VirtualInputImGuiTests COMMAND VirtualInputImGuiTests)
endif()

# ============================================================================
# エディタ操作性の基盤（第2波）: コマンド表 / 作成表 / ファジー検索 / トーストのキュー。
# すべてヘッダオンリー・標準ライブラリだけ（ImGui も GPU も不要）なのでスタンドアロンでも回る。
# ★ショートカット表の衝突・欠落（「Ctrl+O と書いてあるのに効かない」の再発）をここで起動前に落とす。
# ============================================================================
add_executable(EditorUxTests editor_ux_test.cpp)
target_include_directories(EditorUxTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(EditorUxTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(EditorUxTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME EditorUxTests COMMAND EditorUxTests)

# ヒエラルキーの親替えでワールド変換が保たれる（editor/TransformMath.h）。ECS（entt + Transform）が要る。
if(TARGET ECS)
    add_executable(ReparentTests reparent_test.cpp)
    target_link_libraries(ReparentTests PRIVATE ECS)
    target_include_directories(ReparentTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(ReparentTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ReparentTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ReparentTests COMMAND ReparentTests)
endif()

# 表示倍率（DPI）: core/DpiScale.h の純ロジック + editor/EditorTheme.h の theme::Px / ScaleStyleFrom（基準スタイルから
# 再計算するので累積せず、1.0 -> 1.5 -> 2.0 -> 1.0 の往復で元に戻る）。imgui.h の型だけ使う（コンテキスト不要）。
if(TARGET Gui)
    add_executable(DpiScaleTests dpi_scale_test.cpp)
    target_link_libraries(DpiScaleTests PRIVATE Gui)
    target_include_directories(DpiScaleTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(DpiScaleTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(DpiScaleTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME DpiScaleTests COMMAND DpiScaleTests)
endif()

# エディタのテーマ・バリアント（アイデンティティ候補 A/B/C）: editor/ThemeVariants.h + EditorTheme.h の純データ。
# Default が現行値のままであること / 全案の文字コントラスト AA / 案でレイアウトが動かないこと / SetVariant の往復。
if(TARGET Gui)
    add_executable(ThemeVariantTests theme_variant_test.cpp)
    target_link_libraries(ThemeVariantTests PRIVATE Gui)
    target_include_directories(ThemeVariantTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(ThemeVariantTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ThemeVariantTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ThemeVariantTests COMMAND ThemeVariantTests)
endif()

# プロジェクトランチャーの純ロジック（project/LauncherLogic.h・LauncherMotion.h）。
#   テンプレート登録表（ID 一意・GetFiles に実体・画像ファイルの実在）/ 名前・保存場所・Git URL の検証 /
#   最近のプロジェクトの並び・検索・ピン留め / サムネイルの切り出しと縮小 / 補間・スプリング・背景の粒。
# 依存は std だけ（+ ProjectTemplates.cpp）なのでスタンドアロン構成でも回る。
add_executable(LauncherTests launcher_logic_test.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/project/ProjectTemplates.cpp)
target_include_directories(LauncherTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(LauncherTests PRIVATE LAUNCHER_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../assets/editor/launcher")
if(MSVC)
    target_compile_options(LauncherTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(LauncherTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME LauncherTests COMMAND LauncherTests)

# ビューポート周りの純ロジック（editor/ViewportLogic.h。フェーズ 1a）:
#   スナップ量 / アスペクト比のはめ込み / ビューキューブ（向き・投影・クリック判定）/ カメラ姿勢の補間 /
#   ブックマークの往復 / 選択アウトラインの CPU 版（マスク → 縁）/ 矩形選択。依存は標準ライブラリだけ。
add_executable(ViewportLogicTests viewport_logic_test.cpp)
target_include_directories(ViewportLogicTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(ViewportLogicTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(ViewportLogicTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME ViewportLogicTests COMMAND ViewportLogicTests)

# ============================================================================
# マテリアルグラフ G0: 汎用ノードグラフ UI（src/editor/nodegraph/）。
#   NodeGraphTests      … ImGui 非依存の純ロジック（座標変換・空間分割・モデルの不変条件・Undo/Redo ファズ・
#                          JSON 往復・クリップボード・パレット検索）。JSON に nlohmann を使うので統合ビルドでのみ有効。
#   NodeGraphImGuiTests … 窓も GPU も無しの ImGui コンテキストで GraphView を実際に描く（性能・描画・入力の統合テスト）。
# ============================================================================
if(TARGET Editor)
    find_package(nlohmann_json CONFIG REQUIRED)
    set(NODEGRAPH_CORE_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/nodegraph/GraphLayout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/nodegraph/GraphDocument.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/nodegraph/GraphIO.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/nodegraph/GraphPalette.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/nodegraph/SandboxGraphModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/nodegraph/SandboxSamples.cpp)
    add_executable(NodeGraphTests nodegraph_test.cpp ${NODEGRAPH_CORE_SRC})
    target_include_directories(NodeGraphTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(NodeGraphTests PRIVATE nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(NodeGraphTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(NodeGraphTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME NodeGraphTests COMMAND NodeGraphTests)
endif()

# マテリアルグラフ G1（src/renderer/matgraph/）。型システム・モデル API・診断・HLSL コード生成（決定論・CSE・スロット）・
# CPU 参照実装・.dxmg 往復・ゴールデン（tests/data/matgraph/*.dxmg ↔ *.hlsl.expected）。GPU / DXC 不要の純ロジック。
# ゴールデンの更新: MATGRAPH_UPDATE_GOLDEN=1 で実行 → git diff を目視。
if(TARGET MatGraph)
    add_executable(MatGraphTests matgraph_test.cpp)
    target_link_libraries(MatGraphTests PRIVATE MatGraph)
    target_compile_definitions(MatGraphTests PRIVATE
        DX12E_MATGRAPH_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/matgraph" _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(MatGraphTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(MatGraphTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME MatGraphTests COMMAND MatGraphTests)

    # 生成 HLSL を DXC で ps_6_6 / cs_6_6 へコンパイルし、WARP（ソフトウェア D3D12）で CPU 参照実装と数値比較する。
    # dxcompiler.dll は実行時ロード（無い環境ではスキップして終了コード 0）。ctest ラベル: dxc
    if(TARGET Microsoft::DirectXShaderCompiler)
        get_target_property(MATGRAPH_DXC_INC Microsoft::DirectXShaderCompiler INTERFACE_INCLUDE_DIRECTORIES)
        add_executable(MatGraphDxcTests matgraph_dxc_test.cpp)
        target_link_libraries(MatGraphDxcTests PRIVATE MatGraph d3d12 dxgi)
        target_include_directories(MatGraphDxcTests SYSTEM PRIVATE ${MATGRAPH_DXC_INC})
        target_compile_definitions(MatGraphDxcTests PRIVATE
            DX12E_MATGRAPH_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/matgraph"
            DX12E_MATGRAPH_HLSL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../src/renderer/matgraph/hlsl")
        if(MSVC)
            target_compile_options(MatGraphDxcTests PRIVATE ${DX12_COMPILE_OPTIONS})
            target_compile_definitions(MatGraphDxcTests PRIVATE ${DX12_COMPILE_DEFS})
        endif()
        if(DIRECTX_DXC_TOOL)
            get_filename_component(MATGRAPH_DXC_DIR "${DIRECTX_DXC_TOOL}" DIRECTORY)
            add_custom_command(TARGET MatGraphDxcTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MATGRAPH_DXC_DIR}/dxcompiler.dll" "${MATGRAPH_DXC_DIR}/dxil.dll" "$<TARGET_FILE_DIR:MatGraphDxcTests>"
                COMMENT "Bundling dxcompiler.dll / dxil.dll next to MatGraphDxcTests.exe")
        endif()
        add_test(NAME MatGraphDxcTests COMMAND MatGraphDxcTests)
        set_tests_properties(MatGraphDxcTests PROPERTIES LABELS "gpu;dxc" TIMEOUT 300)
    endif()
endif()

# マテリアルグラフ G2b の純ロジック: .dxmat の graph キー（旧ファイルのバイト一致往復）/ パラメータプールのオフセット /
# DXIL ディスクキャッシュ / グラフ化（従来 PBR → 標準テンプレート + インスタンス。12 構造・旧経路の式との CPU 評価一致・.bak）。
# Resource（MaterialAssetIO / MaterialGraphize）と MatGraph だけ。GPU / DXC 不要。
if(TARGET Resource AND TARGET MatGraph)
    add_executable(MatGraphRuntimeTests matgraph_runtime_test.cpp)
    target_link_libraries(MatGraphRuntimeTests PRIVATE Resource MatGraph)
    if(MSVC)
        target_compile_options(MatGraphRuntimeTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(MatGraphRuntimeTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME MatGraphRuntimeTests COMMAND MatGraphRuntimeTests)
endif()

# NodeGraphImGuiTests: 窓も GPU も無しの ImGui コンテキストで GraphView を描き、入力（ImGui の IO へ注入）・DPI・性能（500 ノード / 800 ワイヤ）を検証する。
# Editor（GraphView / UiWidgets）をリンクするだけで、Application も D3D12 デバイスも作らない。
if(TARGET Editor)
    add_executable(NodeGraphImGuiTests nodegraph_imgui_test.cpp)
    target_include_directories(NodeGraphImGuiTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(NodeGraphImGuiTests PRIVATE Editor)
    if(MSVC)
        target_compile_options(NodeGraphImGuiTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(NodeGraphImGuiTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME NodeGraphImGuiTests COMMAND NodeGraphImGuiTests)
endif()

# シーケンサー(.dxseq)の純ロジック(src/sequencer/ = SequencerCore)。S0: データモデル・カーブ/クォータニオン補間・評価・イベント発火・
# SeqOp(Undo/Redo ファズ)・シリアライズ往復・旧 sequence_author 台本の変換・バインディング解決・評価性能。
# 設計は docs/SEQUENCER_DESIGN.md、仕様は docs/DXSEQ_FORMAT.md。GPU / ECS / ImGui 非依存(標準ライブラリ + nlohmann のみ)。
# ゴールデンの更新: SEQUENCER_UPDATE_GOLDEN=1 で実行 → git diff を目視。統合ビルドでのみ有効(nlohmann を使うため)。
if(TARGET SequencerCore)
    # ★SequencerCore のリンク先は nlohmann_json だけであること(ECS / GPU / ImGui / Core を引き込んだら構成時に落とす)
    get_target_property(SEQUENCER_CORE_LIBS SequencerCore LINK_LIBRARIES)
    foreach(_lib ${SEQUENCER_CORE_LIBS})
        if(NOT _lib STREQUAL "nlohmann_json::nlohmann_json")
            message(FATAL_ERROR "SequencerCore must stay pure (std + nlohmann_json only), but links: ${_lib}")
        endif()
    endforeach()

    add_executable(SequencerCoreTests sequencer_core_test.cpp)
    target_link_libraries(SequencerCoreTests PRIVATE SequencerCore)
    target_include_directories(SequencerCoreTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(SequencerCoreTests PRIVATE
        DX12E_SEQUENCER_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/sequencer"
        DX12E_SEQUENCER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../src/sequencer"
        _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(SequencerCoreTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SequencerCoreTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SequencerCoreTests COMMAND SequencerCoreTests)
endif()

# ============================================================================
# マテリアルグラフ G3（UI 接続）: アダプタ（G1 → G0）・エディタ文書・保存 / 読込・診断・パレット絞り込み・Undo ファズ（実グラフ）の純ロジック。
# ImGui / GPU 不要。G0 の文書層と G1 の MaterialGraph をそのままビルドしてリンクする。
# ============================================================================
if(TARGET Editor AND TARGET MatGraph)
    set(MATGRAPH_EDITOR_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/matgraph/MatGraphModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/matgraph/MatGraphEditor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/matgraph/HlslColorize.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/nodegraph/GraphLayout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/nodegraph/GraphDocument.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/nodegraph/GraphIO.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/nodegraph/GraphPalette.cpp)
    add_executable(MatGraphEditorTests matgraph_editor_test.cpp ${MATGRAPH_EDITOR_SRC})
    target_include_directories(MatGraphEditorTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(MatGraphEditorTests PRIVATE MatGraph nlohmann_json::nlohmann_json)
    target_compile_definitions(MatGraphEditorTests PRIVATE
        DX12E_MATGRAPH_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/matgraph" _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(MatGraphEditorTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(MatGraphEditorTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME MatGraphEditorTests COMMAND MatGraphEditorTests)
    set_tests_properties(MatGraphEditorTests PROPERTIES TIMEOUT 300)

    # マテリアルグラフ G2c の純ロジック: 部分グラフ出力（ノード内サムネイル）/ デバウンス / 段階の順序保証 / ジョブキュー / プレビュー設定 /
    # ノード内サムネイルの管理（可視のみ・上限・変更ノードとその下流だけ更新）。GPU / DXC / ImGui 不要。
    add_executable(MatGraphG2cTests matgraph_g2c_test.cpp ${MATGRAPH_EDITOR_SRC}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/matgraph/NodeThumbs.cpp)
    target_include_directories(MatGraphG2cTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(MatGraphG2cTests PRIVATE MatGraph nlohmann_json::nlohmann_json)
    target_compile_definitions(MatGraphG2cTests PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(MatGraphG2cTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(MatGraphG2cTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME MatGraphG2cTests COMMAND MatGraphG2cTests)
    set_tests_properties(MatGraphG2cTests PROPERTIES TIMEOUT 300)

    # マテリアルグラフ G2c の実機検証（実 GPU + DXC。無ければ SKIP）: ノード内サムネイルの GPU == CPU 評価 / プレビュー（4 形状 x 3 環境・決定論・値の変更）/
    # PreviewLite == 通常経路 / 段階コンパイルの順序保証 / 実測ゲート（編集 → 見た目更新のレイテンシ・値のみの変更・ワーカー稼働中のメインスレッド）。
    if(TARGET Renderer AND TARGET Resource)
        add_executable(MatGraphG2cGpuTests matgraph_g2c_gpu_test.cpp ${MATGRAPH_EDITOR_SRC}
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/matgraph/NodeThumbs.cpp)
        target_link_libraries(MatGraphG2cGpuTests PRIVATE Renderer Resource Input MatGraph nlohmann_json::nlohmann_json)
        target_include_directories(MatGraphG2cGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
        target_compile_definitions(MatGraphG2cGpuTests PRIVATE
            DX12E_SHADER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shaders"
            DX12E_SHADER_BIN_DIR="${CMAKE_BINARY_DIR}/shaders/"
            _CRT_SECURE_NO_WARNINGS)
        if(TARGET Shaders)
            add_dependencies(MatGraphG2cGpuTests Shaders)
        endif()
        if(MSVC)
            target_compile_options(MatGraphG2cGpuTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
            target_compile_definitions(MatGraphG2cGpuTests PRIVATE ${DX12_COMPILE_DEFS})
        endif()
        if(DIRECTX_DXC_TOOL)
            get_filename_component(G2C_DXC_DIR "${DIRECTX_DXC_TOOL}" DIRECTORY)
            add_custom_command(TARGET MatGraphG2cGpuTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${G2C_DXC_DIR}/dxcompiler.dll" "${G2C_DXC_DIR}/dxil.dll" "$<TARGET_FILE_DIR:MatGraphG2cGpuTests>"
                COMMENT "Bundling dxcompiler.dll / dxil.dll next to MatGraphG2cGpuTests.exe")
        endif()
        add_test(NAME MatGraphG2cGpuTests COMMAND MatGraphG2cGpuTests)
        set_tests_properties(MatGraphG2cGpuTests PROPERTIES LABELS gpu TIMEOUT 600 SKIP_RETURN_CODE 77)
    endif()
endif()

# ============================================================================
# DXR パストレーサー(地上真値レンダラ。パリティ基盤 Q1a)の検証。
#   PathTracerGpuTests … 窓なしの D3D12 デバイスで shaders/pt/PathTrace.hlsl を実行し、
#     (1) 解析解(白い炉 / GGX の方向アルベド / 点光源・太陽 + ランバート平面 / 閉じた発光箱 / 半透明・アルファテスト)
#     (2) 独立した CPU リファレンス(tests/pt/PtRef.h。倍精度・BVH・別のサンプリング戦略)との一致
#     (3) 収束(1/√N)/ 決定論 / 時間予算つき Record / 性能(1080p の ms/spp)
#     を確かめる。GPU が無い / DXR 1.1・SM 6.6・Tier 3 が無い環境では「SKIP」を出して 0 で終わる。
#     シェーダ(.cso)は Shaders ターゲットが作る。環境変数 DX12E_PT_TEST_QUICK=1 で重い比較を縮小 / DX12E_PT_TEST_DUMP=<dir> で画像を書き出す。
# ============================================================================
if(TARGET Renderer)
    add_executable(PathTracerGpuTests pt_reference_test.cpp)
    target_include_directories(PathTracerGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(PathTracerGpuTests PRIVATE Renderer Microsoft::DirectXMath)
    target_compile_definitions(PathTracerGpuTests PRIVATE DX12E_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
    if(TARGET Shaders)
        add_dependencies(PathTracerGpuTests Shaders)
    endif()
    if(MSVC)
        target_compile_options(PathTracerGpuTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(PathTracerGpuTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_custom_command(TARGET PathTracerGpuTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:PathTracerGpuTests>")
    add_test(NAME PathTracerGpuTests COMMAND PathTracerGpuTests)
    set_tests_properties(PathTracerGpuTests PROPERTIES LABELS gpu TIMEOUT 900)
endif()

# ============================================================================
# 物理ベース大気 A1(Hillaire 2020)の検証。tests/atmosphere_test.cpp 1 本から 2 つの実行ファイルを作る。
#   AtmosphereMathTests … CPU のみ(ヘッダオンリー)。太陽位置・時刻→光の色と照度・透過率・位相関数・プリセット・GPU 定数。
#   AtmosphereGpuTests  … 窓なしの D3D12 デバイスで実 .cso(Shaders ターゲットが作る)を動かし、LUT を読み戻して CPU 参照と突き合わせる。
#                         GPU が無ければ SKIP(0 で終わる)。DX12_D3D_DEBUG=1 でデバッグレイヤ。
# ============================================================================
add_executable(AtmosphereMathTests atmosphere_test.cpp)
target_include_directories(AtmosphereMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(AtmosphereMathTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
    target_compile_definitions(AtmosphereMathTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME AtmosphereMathTests COMMAND AtmosphereMathTests)
set_tests_properties(AtmosphereMathTests PROPERTIES TIMEOUT 120)

if(TARGET Renderer AND TARGET Resource)
    add_executable(AtmosphereGpuTests atmosphere_test.cpp)
    target_compile_definitions(AtmosphereGpuTests PRIVATE ATMOS_GPU_TESTS=1
        DX12E_SHADER_DIR_W=L\"${CMAKE_BINARY_DIR}/shaders/\")
    # Core の Window.cpp が InputSystem を参照する（GraphicsDevice::Initialize が Window& を取るため Window を作る）
    target_link_libraries(AtmosphereGpuTests PRIVATE Renderer Resource Input)
    target_include_directories(AtmosphereGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(TARGET Shaders)
        add_dependencies(AtmosphereGpuTests Shaders)
    endif()
    if(MSVC)
        target_compile_options(AtmosphereGpuTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(AtmosphereGpuTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AtmosphereGpuTests COMMAND AtmosphereGpuTests)
    set_tests_properties(AtmosphereGpuTests PROPERTIES LABELS gpu TIMEOUT 300)
endif()

# パリティ基盤(tools/parity)の Python テスト(pytest。指標・ノイズ床・ゲート・シーン仕様・偽エンジンでのランナー統合)。
# Python か依存(numpy / scikit-image / flip-evaluator など。pwsh tools/parity/setup.ps1)が無い環境ではスキップ扱い。
add_test(NAME ParityHarnessPyTests
    COMMAND ${CMAKE_COMMAND} -DPARITY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../tools/parity -P ${CMAKE_CURRENT_SOURCE_DIR}/../tools/parity/run_pytests.cmake)
set_tests_properties(ParityHarnessPyTests PROPERTIES
    SKIP_REGULAR_EXPRESSION "PARITY_SKIP"
    ENVIRONMENT "PYTHONUTF8=1"
    TIMEOUT 900)

# マテリアルグラフ G3: MatGraphModel を実際の GraphView で描き、ImGui の IO へ注入した入力で操作する統合テスト（UE 風ワンキー・値欄ドラッグ・
# ピンからのドロップ = 型で絞ったパレット・FocusNode・DPI）。窓も GPU も無し。Editor をリンクするだけ。
if(TARGET Editor)
    add_executable(MatGraphEditorImGuiTests matgraph_editor_imgui_test.cpp)
    target_include_directories(MatGraphEditorImGuiTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(MatGraphEditorImGuiTests PRIVATE Editor)
    if(MSVC)
        target_compile_options(MatGraphEditorImGuiTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(MatGraphEditorImGuiTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME MatGraphEditorImGuiTests COMMAND MatGraphEditorImGuiTests)
    set_tests_properties(MatGraphEditorImGuiTests PROPERTIES TIMEOUT 300)
endif()

# ============================================================================
# フェーズ 1b W: エディタ設定ストア（editor/EditorPrefsStore.h）とワークスペース / レイアウトの純ロジック（editor/WorkspaceLogic.h）。
# どちらも nlohmann と標準ライブラリだけ（ImGui も GPU も窓も要らない）。
# ============================================================================
if(TARGET Editor)
    find_package(nlohmann_json CONFIG REQUIRED)
    add_executable(EditorPrefsTests editor_prefs_test.cpp)
    target_include_directories(EditorPrefsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(EditorPrefsTests PRIVATE nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(EditorPrefsTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(EditorPrefsTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME EditorPrefsTests COMMAND EditorPrefsTests)

    add_executable(WorkspaceLogicTests workspace_logic_test.cpp)
    target_include_directories(WorkspaceLogicTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(WorkspaceLogicTests PRIVATE nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(WorkspaceLogicTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(WorkspaceLogicTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME WorkspaceLogicTests COMMAND WorkspaceLogicTests)
endif()

# ============================================================================
# インスペクタの純ロジック（editor/InspectorLogic.h。フェーズ 1b・担当 I）:
#   Add Component の目録と検索ランキング / 最近使った / 折りたたみ状態 / タグ集合 / 複数選択のバイト比較(Mixed・既定値との差) /
#   Transform の一括編集(触った軸だけ) / プロパティ検索 / 値のコピー・ペースト文字列。依存は標準ライブラリだけ。
# ============================================================================
add_executable(InspectorLogicTests inspector_logic_test.cpp)
target_include_directories(InspectorLogicTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(InspectorLogicTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(InspectorLogicTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME InspectorLogicTests COMMAND InspectorLogicTests)

# ============================================================================
# ヒエラルキーの純ロジック（editor/HierarchyLogic.h・HierarchyCommands.h。フェーズ 1b・担当 H）:
#   型の分類 / 検索クエリ(名前+コンポーネント名+タグ) / 親→子の索引(兄弟順) / 範囲選択 / ドロップ位置→親替え+兄弟順 /
#   hidden・locked の祖先伝播と Alt+クリック(これだけ表示) / EditorFlagCommand の Undo /
#   シーン JSON の互換(フラグ無しは 1 キーも増えない・旧 JSON・往復)。Scene(GPU 不要)をリンクする。
# ============================================================================
if(TARGET Scene)
    add_executable(HierarchyLogicTests hierarchy_logic_test.cpp)
    target_link_libraries(HierarchyLogicTests PRIVATE Scene)
    target_include_directories(HierarchyLogicTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(HierarchyLogicTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(HierarchyLogicTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME HierarchyLogicTests COMMAND HierarchyLogicTests)
endif()

# ============================================================================
# シーケンサー S1b: バインディング解決 + 適用層 + PreAnimatedState(非破壊スクラブ)+ Play 統合(core/SequencerHost.* ほか)。
#   Scene(registry だけ・GPU 不要)と SequencerHost を直接叩く: guid → 階層パス → 名前の解決 / 適用の決定論 / カットの選択 /
#   ポスト・DoF の描画時上書き / スクラブ中の保存にスクラブの値が混ざらない / Play 直前・文書を閉じる・シーン変更での復元 /
#   イベント(前進で 1 回・スクラブでは発火しない)/ 実時間の時計 / 物理との競合規則 / ops JSON・undo・保存・自動再生。
# ============================================================================
if(TARGET Scene AND TARGET SequencerCore)
    add_executable(SequencerBindTests sequencer_bind_test.cpp)
    target_link_libraries(SequencerBindTests PRIVATE Scene)
    target_include_directories(SequencerBindTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(SequencerBindTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SequencerBindTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SequencerBindTests COMMAND SequencerBindTests)
endif()

# アセットブラウザの純ロジック + 非同期インデックス（editor/AssetBrowserLogic.h / AssetIndex.cpp。フェーズ 1b A）:
#   拡張子の分類 / 自然順ソート / 走査と差分 / グリッドの可視範囲 / 複数選択 / パンくず・移動・取り込みの計画 /
#   参照の検索 / サムネイルのキャッシュキーと .utb / ワーカーの走査・変更監視。依存は標準ライブラリ + Win32 のみ（GPU / ImGui 不要）。
add_executable(AssetBrowserLogicTests asset_browser_logic_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/editor/AssetIndex.cpp)
target_include_directories(AssetBrowserLogicTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(AssetBrowserLogicTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(AssetBrowserLogicTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME AssetBrowserLogicTests COMMAND AssetBrowserLogicTests)
set_tests_properties(AssetBrowserLogicTests PROPERTIES TIMEOUT 120)

# ============================================================================
# パリティ Q2(校正): 光の物理単位・露出(EV100)・トーンマップ(UE Filmic / PBR Neutral / 線形)の純関数
#   renderer/PhotometricMath.h（ヘッダオンリー・標準ライブラリのみ）。EV100 ↔ 露出係数 / lm ↔ cd / 逆二乗と窓 /
#   トーンマップの参照値(tools/parity の numpy 実装と突き合わせ)と不変条件。HLSL 側(Tonemap.hlsli / Lighting.hlsli)はこの式の写し。
# ============================================================================
add_executable(PhotometricMathTests photometric_test.cpp)
target_include_directories(PhotometricMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(PhotometricMathTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(PhotometricMathTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME PhotometricMathTests COMMAND PhotometricMathTests)

# ============================================================================
# パリティ Q2(校正): 任意解像度のオフスクリーン出力(core/OffscreenShot.h。純ロジック)と線形 HDR 出力(PFM / EXR)の書式。
#   サイズの解釈 / 上限と VRAM の見積 / PFM・EXR の往復と決定論。パストレーサーと同じ writer(renderer/pt/PtImageIO.h)を使う。
# ============================================================================
add_executable(OffscreenShotTests offscreen_shot_test.cpp)
target_include_directories(OffscreenShotTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(OffscreenShotTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(OffscreenShotTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME OffscreenShotTests COMMAND OffscreenShotTests)

# ============================================================================
# M7 エディタ操作の全面公開（dx12_editor_command / dx12_editor_state）の土台:
#   editor/EditorCommandMcp（コマンド表 → 公開用の一覧・分類・可否・引数・実行）/ EditorStateMcp（選択・窓・レイアウト・モーダル・undo・トーストの JSON）。
#   表の全 id（kCommands / window.* / create.*）が一覧に出て実行できる・guarded の分類が黙って外れない・無効には必ず理由が付く・
#   ImGui のモーダル / ダイアログ / パレットを検知できる。窓も GPU も無し（Editor をリンクするだけ）。
# ============================================================================
if(TARGET Editor)
    add_executable(McpEditorCommandTests mcp_editor_command_test.cpp)
    target_include_directories(McpEditorCommandTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    # EditorPrefs.cpp が ProjectManager::LoadEditorSection を呼ぶので ProjectLib も要る(Editor は ProjectLib を公開リンクしていない)
    target_link_libraries(McpEditorCommandTests PRIVATE Editor ProjectLib)
    if(MSVC)
        target_compile_options(McpEditorCommandTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(McpEditorCommandTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME McpEditorCommandTests COMMAND McpEditorCommandTests)
endif()

# ============================================================================
# 植生 F1（FoliageLayer + GPU カリング + ExecuteIndirect + 風）
#   FoliageTests    … 純ロジック: 32B パック / チャンク / .dxfoliage 往復と破損の拒否 / ポアソンディスク散布 / カリングの CPU 参照 /
#                     風の決定論 / 100 万インスタンスの構築時間。GPU 不要。
#   FoliageGpuTests … 窓なしの D3D12 デバイスで compute を実行し、GPU のカリング結果 = CPU 参照 / 風 GPU = CPU / 間接引数の個数を確かめる。
#                     GPU が無い環境では「SKIP」を出して 0 で終わる。シェーダ（.cso）は Shaders ターゲットが作る。
# ============================================================================
if(TARGET Renderer)
    add_executable(FoliageTests foliage_test.cpp)
    target_include_directories(FoliageTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(FoliageTests PRIVATE Renderer Microsoft::DirectXMath)
    target_compile_definitions(FoliageTests PRIVATE DX12E_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
    if(MSVC)
        target_compile_options(FoliageTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(FoliageTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_custom_command(TARGET FoliageTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:FoliageTests>")
    add_test(NAME FoliageTests COMMAND FoliageTests)

    add_executable(FoliageGpuTests foliage_gpu_test.cpp)
    target_include_directories(FoliageGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(FoliageGpuTests PRIVATE Renderer Microsoft::DirectXMath)
    target_compile_definitions(FoliageGpuTests PRIVATE DX12E_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
    if(TARGET Shaders)
        add_dependencies(FoliageGpuTests Shaders)
    endif()
    if(MSVC)
        target_compile_options(FoliageGpuTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(FoliageGpuTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_custom_command(TARGET FoliageGpuTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:FoliageGpuTests>")
    add_test(NAME FoliageGpuTests COMMAND FoliageGpuTests)
    set_tests_properties(FoliageGpuTests PROPERTIES LABELS gpu)

    # GPU 駆動のインスタンス群（設計 §4.2 = 4-3）。純ロジック（チャンク / LOD の CPU 参照）+ 窓なしの D3D12 で compute を実行して GPU = CPU を確かめる。
    #   GPU が無い環境では GPU 部分だけ「SKIP」。
    add_executable(InstanceGpuTests instance_gpu_test.cpp)
    target_include_directories(InstanceGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(InstanceGpuTests PRIVATE Renderer Microsoft::DirectXMath)
    target_compile_definitions(InstanceGpuTests PRIVATE DX12E_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/")
    if(TARGET Shaders)
        add_dependencies(InstanceGpuTests Shaders)
    endif()
    if(MSVC)
        target_compile_options(InstanceGpuTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(InstanceGpuTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_custom_command(TARGET InstanceGpuTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:InstanceGpuTests>")
    add_test(NAME InstanceGpuTests COMMAND InstanceGpuTests)
    set_tests_properties(InstanceGpuTests PROPERTIES LABELS gpu)
endif()

# パリティ Q2: 新しいトーンマップ(shaders/post/Tonemap.hlsli)の HLSL を実際に動かして C++ 参照(PhotometricMath.h)と突き合わせる。
#   ハードウェア GPU を優先し、無ければ WARP。dxcompiler.dll / dxil.dll(exe の隣へ同梱)が無ければ SKIP。ラベル: gpu
if(TARGET Graphics AND TARGET Microsoft::DirectXShaderCompiler)
    get_target_property(TONEMAP_DXC_INC Microsoft::DirectXShaderCompiler INTERFACE_INCLUDE_DIRECTORIES)
    add_executable(TonemapGpuTests tonemap_gpu_test.cpp)
    target_link_libraries(TonemapGpuTests PRIVATE Graphics)
    target_include_directories(TonemapGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(TonemapGpuTests SYSTEM PRIVATE ${TONEMAP_DXC_INC})
    target_compile_definitions(TonemapGpuTests PRIVATE
        DX12E_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shaders"
        DX12E_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    if(MSVC)
        target_compile_options(TonemapGpuTests PRIVATE ${DX12_COMPILE_OPTIONS} /utf-8)
        target_compile_definitions(TonemapGpuTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    if(DIRECTX_DXC_TOOL)
        get_filename_component(TONEMAP_DXC_DIR "${DIRECTX_DXC_TOOL}" DIRECTORY)
        add_custom_command(TARGET TonemapGpuTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TONEMAP_DXC_DIR}/dxcompiler.dll" "${TONEMAP_DXC_DIR}/dxil.dll" "$<TARGET_FILE_DIR:TonemapGpuTests>"
            COMMENT "Bundling dxcompiler.dll / dxil.dll next to TonemapGpuTests.exe")
    endif()
    add_test(NAME TonemapGpuTests COMMAND TonemapGpuTests)
    set_tests_properties(TonemapGpuTests PROPERTIES LABELS gpu TIMEOUT 300)
endif()

# ============================================================================
# GI モード「新」の既定と移行（S5。scene/GiMigration.* ・ editor/GiMigrationCommand.h ・ テンプレート）:
#   DDGI 格子の自動フィット（1.0m 間隔 / 4096 個以下 / 軸 32 以下 / 被覆）・新の既定構成・DXR 非対応で不変・旧へ戻す・
#   移行の Undo/Redo が 1 操作・保存互換（旧は gi キーを書かない / 新は往復 / キー無しは旧）・テンプレートの GI 既定（2d は旧のまま）。
#   Scene（GPU 不要）をリンクし、ProjectTemplates.cpp は直接コンパイルする（LauncherTests と同じ）。
# ============================================================================
# ============================================================================
# DDGI のカメラ追従スクロール格子 + 更新スケジュール + ライトグリッド（GI S4。renderer/DdgiSchedule.h。ヘッダだけ・GPU 不要）:
#   窓の追従とヒステリシス・リングバッファ（残るプローブの記憶領域は不変・入ってきた列だけリセット）・ラウンドロビン・予算の制御・ライトグリッドの割り当て。
# ============================================================================
add_executable(DdgiScheduleTests ddgi_schedule_test.cpp)
target_include_directories(DdgiScheduleTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(DdgiScheduleTests PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(DdgiScheduleTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME DdgiScheduleTests COMMAND DdgiScheduleTests)

if(TARGET Scene)
    add_executable(GiMigrationTests gi_migration_test.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../src/project/ProjectTemplates.cpp)
    target_link_libraries(GiMigrationTests PRIVATE Scene)
    target_include_directories(GiMigrationTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(GiMigrationTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(GiMigrationTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME GiMigrationTests COMMAND GiMigrationTests)
endif()

# UI 自動テスト(ImGuiTestEngine)を窓なし・背景・仮想入力で全件流す（約 3.5 分）。既定では ctest に含めない。
#   cmake -DDX12_CTEST_UI=ON で有効化 → ctest -R UiTests。DX12Engine を事前にビルドしておくこと。
#   ユーザーの PC 操作を奪わない（前面ウィンドウが変わったら失敗にする）。詳細は tools/run_ui_tests.ps1。
option(DX12_CTEST_UI "ctest に UI 自動テスト(UiTests)を含める" OFF)
if(DX12_CTEST_UI)
    add_test(NAME UiTests COMMAND pwsh -NoProfile -File ${CMAKE_SOURCE_DIR}/tools/run_ui_tests.ps1)
    set_tests_properties(UiTests PROPERTIES TIMEOUT 1800 LABELS ui RUN_SERIAL TRUE)
endif()

# 再帰削除の最後の砦（core/SafeRemove.h）: ルート・ルート直下・ホームとその直下・システムのフォルダ・exe のフォルダは消さない。
# base の中だけに限る指定。危ないパスは判定だけを見る（RemoveAll は一時フォルダの中にしか呼ばない）。
# あわせて src/ 全体を走査し、SafeRemove.h 以外に std::filesystem の再帰削除を直接書いた箇所があれば落とす。
add_executable(SafeRemoveTests safe_remove_test.cpp)
target_include_directories(SafeRemoveTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(SafeRemoveTests PRIVATE DX12E_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../src")
if(MSVC)
    target_compile_options(SafeRemoveTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(SafeRemoveTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME SafeRemoveTests COMMAND SafeRemoveTests)

# ハードウェア連携プロトコル v1 の純ロジック（hardware/HwProtocol.cpp）: 行の組み立て（\r 無視・128 バイト超の破棄）・
# 各行種のパース・不正行・エンジン → デバイスの行の生成。Win32 にも Logger にも依存しないのでスタンドアロンでも動く。
add_executable(HardwareProtocolTests hardware_protocol_test.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/hardware/HwProtocol.cpp)
target_include_directories(HardwareProtocolTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(HardwareProtocolTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(HardwareProtocolTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME HardwareProtocolTests COMMAND HardwareProtocolTests)

# HardwareSystem の結合テスト（Loopback の仮想デバイスと台本トランスポートだけ。実 COM ポートは開かない）。
# 接続・値の正規化・Pressed・出力の合流・dangerous・心拍・再接続・校正・サンプリング・hardware.json の往復。
if(TARGET Hardware)
    add_executable(HardwareSystemTests hardware_system_test.cpp)
    target_link_libraries(HardwareSystemTests PRIVATE Hardware)
    target_include_directories(HardwareSystemTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(HardwareSystemTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(HardwareSystemTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME HardwareSystemTests COMMAND HardwareSystemTests)
    set_tests_properties(HardwareSystemTests PROPERTIES TIMEOUT 60)
endif()

# editor/McpClientConfig.h（AI クライアントの設定ファイルへ MCP サーバを足す純ロジック。Codex の TOML / JSON 系の
# 差し替え・冪等性・壊れたファイルを書かない、Node の版判定と SHASUMS の解釈）のテスト。ヘッダオンリー + nlohmann。
if(TARGET Core)
    add_executable(McpClientConfigTests mcp_client_config_test.cpp)
    target_link_libraries(McpClientConfigTests PRIVATE Core)
    target_include_directories(McpClientConfigTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(McpClientConfigTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(McpClientConfigTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME McpClientConfigTests COMMAND McpClientConfigTests)
endif()
