add_library(Input STATIC
    InputSystem.cpp
)

target_include_directories(Input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(Input PUBLIC Core)

target_compile_options(Input PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Input PRIVATE ${DX12_COMPILE_DEFS})
