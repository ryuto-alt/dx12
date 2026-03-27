cmake_minimum_required(VERSION 3.28)
project(DX12Engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC compile options (not global - applied per-target to avoid conflicts with FetchContent deps)
set(DX12_COMPILE_OPTIONS /W4 /WX /MP /permissive- /utf-8)
set(DX12_COMPILE_DEFS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# Dependencies
find_package(spdlog CONFIG REQUIRED)

# Sub-projects
add_subdirectory(src/core)
add_subdirectory(src/graphics)
add_subdirectory(src/renderer)
add_subdirectory(src/resource)

# Main executable
add_executable(DX12Engine WIN32 src/main.cpp)

target_link_libraries(DX12Engine
    PRIVATE
        Core
        Graphics
        Renderer
        Resource
)

if(MSVC)
    target_compile_options(DX12Engine PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(DX12Engine PRIVATE ${DX12_COMPILE_DEFS})
endif()

# --- DXC Shader Compilation ---
find_program(DXC_EXECUTABLE dxc
    PATHS
        "$ENV{WindowsSdkVerBinPath}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
)

if(DXC_EXECUTABLE)
    message(STATUS "Found DXC: ${DXC_EXECUTABLE}")

    set(SHADER_SOURCE "${CMAKE_SOURCE_DIR}/shaders/forward/Forward.hlsl")
    set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

    set(SHADER_OUTPUTS
        "${SHADER_OUTPUT_DIR}/Forward_VS.cso"
        "${SHADER_OUTPUT_DIR}/Forward_PS.cso"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Forward_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/Forward_VS.cso" "${SHADER_SOURCE}"
        DEPENDS "${SHADER_SOURCE}"
        COMMENT "Compiling Forward.hlsl -> Forward_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Forward_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/Forward_PS.cso" "${SHADER_SOURCE}"
        DEPENDS "${SHADER_SOURCE}"
        COMMENT "Compiling Forward.hlsl -> Forward_PS.cso (ps_6_0)"
    )

    add_custom_target(Shaders ALL DEPENDS ${SHADER_OUTPUTS})
    add_dependencies(DX12Engine Shaders)
else()
    message(WARNING "DXC not found -- shader compilation skipped")
endif()

# Shader directory and assets directory (Core needs it because Application.cpp loads shaders/assets)
target_compile_definitions(Core PRIVATE
    SHADER_DIR=L"${CMAKE_BINARY_DIR}/shaders/"
    ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/"
)
