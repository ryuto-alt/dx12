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
    SplashScreen.cpp
    Logger.cpp
    CrashHandler.cpp
    GameClock.cpp
    PathResolver.cpp
    Updater.cpp
    vfs/Crypto.cpp
    vfs/Compression.cpp
    vfs/PakWriter.cpp
    vfs/PakArchive.cpp
    vfs/Vfs.cpp
    mcp/McpBridge.cpp
)

target_include_directories(Core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_SOURCE_DIR}/include   # 封印ランタイムの中立公開ヘッダ（engine/...）EventBus 等
)

find_package(spdlog CONFIG REQUIRED)

target_link_libraries(Core
    PUBLIC spdlog::spdlog
    PRIVATE imgui::imgui bcrypt.lib Cabinet.lib ws2_32.lib gdiplus.lib  # gdiplus はスプラッシュのPNGロゴ用
)

target_compile_options(Core PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Core PRIVATE ${DX12_COMPILE_DEFS})

# Application.cpp は MCP ハンドラ + nlohmann::json のテンプレート実体化が多く、Debug(未最適化)では
# COFF セクション上限(C1128)を超える。/bigobj で回避（Release は最適化で収まるが両構成へ付けて統一）。
target_compile_options(Core PRIVATE /bigobj)
