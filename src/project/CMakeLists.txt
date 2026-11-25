add_library(ProjectLib STATIC
    Project.cpp
    ProjectTemplates.cpp
    ProjectManager.cpp
    GitIntegration.cpp
)

target_include_directories(ProjectLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ProjectLib PUBLIC Core Gui PRIVATE Ole32 Shell32)

if(MSVC)
    target_compile_options(ProjectLib PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(ProjectLib PRIVATE ${DX12_COMPILE_DEFS})
endif()

# レーン別の追加ソース（中身は各レーンが書く。OPTIONAL は付けない＝読み忘れを検出するため）
include(${CMAKE_SOURCE_DIR}/cmake/lanes/project_lane_a.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/project_lane_b.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/project_lane_c.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/project_lane_d.cmake)
