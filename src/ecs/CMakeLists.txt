add_library(ECS STATIC
    Components.cpp
    ComponentMeta.cpp
    InstanceGroup.cpp
)

target_include_directories(ECS PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(EnTT CONFIG REQUIRED)
find_package(directxmath CONFIG REQUIRED)

target_link_libraries(ECS PUBLIC
    EnTT::EnTT
    Animation
    Microsoft::DirectXMath
)

target_compile_options(ECS PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(ECS PRIVATE ${DX12_COMPILE_DEFS})

# レーン別の追加ソース（中身は各レーンが書く。OPTIONAL は付けない＝読み忘れを検出するため）
include(${CMAKE_SOURCE_DIR}/cmake/lanes/ecs_lane_a.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/ecs_lane_b.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/ecs_lane_c.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/ecs_lane_d.cmake)
