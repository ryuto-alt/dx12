add_library(Animation STATIC
    Skeleton.cpp
    AnimationClip.cpp
    AnimPose.cpp
    AnimGraphAsset.cpp
    AnimGraphRuntime.cpp
    Animator.cpp
    SkinningBuffer.cpp
    NodeGraph.cpp
    NodeAnimationClip.cpp
    NodeAnimator.cpp
    UiAnimAsset.cpp
    SpriteAnimAsset.cpp
)

target_include_directories(Animation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(directxmath CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(Animation PUBLIC
    Graphics
    Microsoft::DirectXMath
    nlohmann_json::nlohmann_json   # .uianim / .spranim の JSON IO
)

target_compile_options(Animation PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Animation PRIVATE ${DX12_COMPILE_DEFS})
