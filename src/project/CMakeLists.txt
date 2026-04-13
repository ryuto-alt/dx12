add_library(ProjectLib STATIC
    Project.cpp
    ProjectManager.cpp
)

target_include_directories(ProjectLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ProjectLib PUBLIC Core Gui)

if(MSVC)
    target_compile_options(ProjectLib PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(ProjectLib PRIVATE ${DX12_COMPILE_DEFS})
endif()
