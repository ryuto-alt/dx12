add_library(Renderer STATIC
    Mesh.cpp
    Camera.cpp
    Material.cpp
)
target_include_directories(Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(directxmath CONFIG REQUIRED)
target_link_libraries(Renderer PUBLIC Graphics Microsoft::DirectXMath)

target_compile_options(Renderer PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Renderer PRIVATE ${DX12_COMPILE_DEFS})
