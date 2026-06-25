find_package(imgui CONFIG REQUIRED)

add_library(Core STATIC
    Application.cpp
    Window.cpp
    Logger.cpp
    GameClock.cpp
    PathResolver.cpp
)

target_include_directories(Core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_SOURCE_DIR}/include   # 封印ランタイムの中立公開ヘッダ（engine/...）EventBus 等
)

find_package(spdlog CONFIG REQUIRED)

target_link_libraries(Core
    PUBLIC spdlog::spdlog
    PRIVATE imgui::imgui
)

target_compile_options(Core PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Core PRIVATE ${DX12_COMPILE_DEFS})
