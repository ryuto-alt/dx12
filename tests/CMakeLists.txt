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
