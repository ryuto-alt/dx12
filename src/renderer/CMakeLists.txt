add_library(Renderer STATIC
    Mesh.cpp
    Camera.cpp
    Material.cpp
)
target_include_directories(Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(directxmath CONFIG REQUIRED)
target_link_libraries(Renderer PUBLIC Graphics Microsoft::DirectXMath)
