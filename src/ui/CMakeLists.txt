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
