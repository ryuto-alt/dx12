add_library(Scene STATIC
    Transform.cpp
    Entity.cpp
    Scene.cpp
)

target_include_directories(Scene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(directxmath CONFIG REQUIRED)

target_link_libraries(Scene PUBLIC
    Core
    Graphics
    Renderer
    Animation
    Resource
    Microsoft::DirectXMath
)

target_compile_options(Scene PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Scene PRIVATE ${DX12_COMPILE_DEFS})
