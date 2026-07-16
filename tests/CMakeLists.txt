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
add_executable(NetBufferTests net_buffer_test.cpp)
target_include_directories(NetBufferTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(NetBufferTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(NetBufferTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME NetBufferTests COMMAND NetBufferTests)

# UIText リッチテキスト(インラインタグ)パーサのテスト(ヘッダオンリー・依存ゼロなのでスタンドアロンでも動く)。
add_executable(UiRichTextTests ui_rich_text_test.cpp)
target_include_directories(UiRichTextTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(UiRichTextTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(UiRichTextTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME UiRichTextTests COMMAND UiRichTextTests)

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

# entt::meta コア部品フィールド反映テスト（Phase 2 土台）。ECS（+ entt/DirectXMath）をリンク。
if(TARGET ECS)
    add_executable(ComponentMetaTests component_meta_test.cpp)
    target_link_libraries(ComponentMetaTests PRIVATE ECS)
    if(MSVC)
        target_compile_options(ComponentMetaTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ComponentMetaTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ComponentMetaTests COMMAND ComponentMetaTests)
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

    # Frustum（視錐台抽出＋球カリング）テスト。ヘッダオンリー・純 DirectXMath。
    add_executable(FrustumTests frustum_test.cpp)
    target_include_directories(FrustumTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(FrustumTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(FrustumTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(FrustumTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME FrustumTests COMMAND FrustumTests)

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
