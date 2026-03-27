add_library(Animation STATIC
    Skeleton.cpp
    AnimationClip.cpp
    Animator.cpp
    SkinningBuffer.cpp
)

target_include_directories(Animation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(directxmath CONFIG REQUIRED)

target_link_libraries(Animation PUBLIC
    Graphics
    Microsoft::DirectXMath
)

target_compile_options(Animation PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Animation PRIVATE ${DX12_COMPILE_DEFS})
