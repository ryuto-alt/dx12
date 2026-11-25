add_library(Nav STATIC
    NavRasterize.cpp
    NavRegion.cpp
    NavContour.cpp
    NavPolyMesh.cpp
    NavMesh.cpp
    NavQuery.cpp
    NavCorridor.cpp
    NavCrowd.cpp
    NavBuilder.cpp
)
target_include_directories(Nav PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# 純ロジック。ECS も Renderer も要らない（シーン走査は呼び出し側が三角形スープにして渡す）。
# Core は Types.h と Logger だけのために繋いでいる。
target_link_libraries(Nav PUBLIC Core)

target_compile_options(Nav PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Nav PRIVATE ${DX12_COMPILE_DEFS})

# レーン別の追加ソース（中身は各レーンが書く。OPTIONAL は付けない＝読み忘れを検出するため）
include(${CMAKE_SOURCE_DIR}/cmake/lanes/nav_lane_a.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/nav_lane_b.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/nav_lane_c.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/nav_lane_d.cmake)
