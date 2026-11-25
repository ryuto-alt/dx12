add_library(Animation STATIC
    Skeleton.cpp
    AnimationClip.cpp
    AnimPose.cpp
    AnimGraphAsset.cpp
    AnimGraphRuntime.cpp
    FootIK.cpp
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

# レーン別の追加ソース（中身は各レーンが書く。OPTIONAL は付けない＝読み忘れを検出するため）
include(${CMAKE_SOURCE_DIR}/cmake/lanes/animation_lane_a.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/animation_lane_b.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/animation_lane_c.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/animation_lane_d.cmake)
