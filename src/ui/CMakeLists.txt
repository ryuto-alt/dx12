find_package(imgui CONFIG REQUIRED)

add_library(Ui STATIC
    UISystem.cpp
    UiAnimRuntime.cpp
)

target_include_directories(Ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Core: PathResolver / EventBus(include/engine)、Graphics: DescriptorHeap/Texture、
# Resource: ResourceManager、ECS: Components/entt/DirectXMath、
# Animation: .uianim/.spranim のデータ構造と JSON IO(UiAnimRuntime)
target_link_libraries(Ui PUBLIC
    Core
    Graphics
    Resource
    ECS
    Animation
    imgui::imgui
)

target_compile_options(Ui PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Ui PRIVATE ${DX12_COMPILE_DEFS})

# レーン別の追加ソース（中身は各レーンが書く。OPTIONAL は付けない＝読み忘れを検出するため）
include(${CMAKE_SOURCE_DIR}/cmake/lanes/ui_lane_a.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/ui_lane_b.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/ui_lane_c.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/ui_lane_d.cmake)
