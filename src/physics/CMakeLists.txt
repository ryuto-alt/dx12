add_library(Physics STATIC
    PhysicsSystem.cpp
    PhysicsDebugRenderer.cpp
)

target_include_directories(Physics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(Jolt CONFIG REQUIRED)
find_package(directxmath CONFIG REQUIRED)

target_link_libraries(Physics PUBLIC
    Core
    Graphics
    ECS
    Resource
    Jolt::Jolt
    Microsoft::DirectXMath
)

target_compile_options(Physics PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Physics PRIVATE ${DX12_COMPILE_DEFS})
