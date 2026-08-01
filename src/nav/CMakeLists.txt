add_library(Nav STATIC
    NavRasterize.cpp
    NavRegion.cpp
    NavContour.cpp
    NavPolyMesh.cpp
    NavMesh.cpp
    NavQuery.cpp
    NavBuilder.cpp
)
target_include_directories(Nav PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# 純ロジック。ECS も Renderer も要らない（シーン走査は呼び出し側が三角形スープにして渡す）。
# Core は Types.h と Logger だけのために繋いでいる。
target_link_libraries(Nav PUBLIC Core)

target_compile_options(Nav PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Nav PRIVATE ${DX12_COMPILE_DEFS})
