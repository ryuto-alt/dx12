find_package(imgui CONFIG REQUIRED)

# 配布アセット暗号鍵（ビルド毎にランダム生成、gitignore）。無ければ即エラー。
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/AssetKey.h")
    message(FATAL_ERROR
        "src/core/generated/AssetKey.h is missing. "
        "Run: powershell -ExecutionPolicy Bypass -File tools/gen_asset_key.ps1 "
        "(build_debug.bat / build_release.bat do this automatically).")
endif()

add_library(Core STATIC
    Application.cpp
    Window.cpp
    Logger.cpp
    GameClock.cpp
    PathResolver.cpp
    Updater.cpp
    vfs/Crypto.cpp
    vfs/Compression.cpp
    vfs/PakWriter.cpp
    vfs/PakArchive.cpp
    vfs/Vfs.cpp
)

target_include_directories(Core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_SOURCE_DIR}/include   # 封印ランタイムの中立公開ヘッダ（engine/...）EventBus 等
)

find_package(spdlog CONFIG REQUIRED)

target_link_libraries(Core
    PUBLIC spdlog::spdlog
    PRIVATE imgui::imgui bcrypt.lib Cabinet.lib
)

target_compile_options(Core PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Core PRIVATE ${DX12_COMPILE_DEFS})
