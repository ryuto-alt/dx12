add_library(ECS STATIC
    Components.cpp
)

target_include_directories(ECS PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(EnTT CONFIG REQUIRED)
find_package(directxmath CONFIG REQUIRED)

target_link_libraries(ECS PUBLIC
    EnTT::EnTT
    Animation
    Microsoft::DirectXMath
)

target_compile_options(ECS PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(ECS PRIVATE ${DX12_COMPILE_DEFS})
