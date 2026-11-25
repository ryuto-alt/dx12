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
    Terrain
    Jolt::Jolt
    Microsoft::DirectXMath
)

target_compile_options(Physics PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Physics PRIVATE ${DX12_COMPILE_DEFS})

# レーン別の追加ソース（中身は各レーンが書く。OPTIONAL は付けない＝読み忘れを検出するため）
include(${CMAKE_SOURCE_DIR}/cmake/lanes/physics_lane_a.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/physics_lane_b.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/physics_lane_c.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/physics_lane_d.cmake)
