cmake_minimum_required(VERSION 3.28)
project(DX12Engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC compile options
if(MSVC)
    add_compile_options(/W4 /WX /MP /permissive- /utf-8)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Dependencies
find_package(spdlog CONFIG REQUIRED)

# Sub-projects
add_subdirectory(src/core)
add_subdirectory(src/graphics)

# Main executable
add_executable(DX12Engine WIN32 src/main.cpp)

target_link_libraries(DX12Engine
    PRIVATE
        Core
        Graphics
        d3d12.lib
        dxgi.lib
        dxguid.lib
        d3dcompiler.lib
)
