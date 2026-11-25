add_library(Input STATIC
    InputSystem.cpp
)

target_include_directories(Input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(Input PUBLIC Core PRIVATE xinput.lib)

target_compile_options(Input PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Input PRIVATE ${DX12_COMPILE_DEFS})

# レーン別の追加ソース（中身は各レーンが書く。OPTIONAL は付けない＝読み忘れを検出するため）
include(${CMAKE_SOURCE_DIR}/cmake/lanes/input_lane_a.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/input_lane_b.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/input_lane_c.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/input_lane_d.cmake)
