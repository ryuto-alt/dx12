add_library(AI STATIC
    AiSystem.cpp
    Blackboard.cpp
    Utility.cpp
    Perception.cpp
)

target_include_directories(AI PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(directxmath CONFIG REQUIRED)

# ゲーム AI のランタイム（群衆とエンティティの対応・Brain）。
# 純ロジック（nav/ と ai/ の評価器）はテストが .cpp を直接ビルドする。
target_link_libraries(AI PUBLIC
    Core
    ECS
    Scene
    Nav
    Physics
    Microsoft::DirectXMath
)

target_compile_options(AI PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(AI PRIVATE ${DX12_COMPILE_DEFS})
