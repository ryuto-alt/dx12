add_library(Scene STATIC
    Entity.cpp
    Scene.cpp
    SceneSerializer.cpp
    SceneFlow.cpp
    ComponentRegistry.cpp
)

target_include_directories(Scene PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include   # 封印ランタイムの中立公開ヘッダ（engine/...）
)

find_package(directxmath CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(Scene PUBLIC
    Core
    Graphics
    Renderer
    Animation
    Resource
    ECS
    Microsoft::DirectXMath
    nlohmann_json::nlohmann_json
)

target_compile_options(Scene PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Scene PRIVATE ${DX12_COMPILE_DEFS})
