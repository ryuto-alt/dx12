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
