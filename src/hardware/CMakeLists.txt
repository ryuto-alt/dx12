# 物理ハードウェア連携（Arduino / ESP32 を USB シリアルでつなぐ層）。docs/HARDWARE.md
# プロトコル v1 の純ロジック（HwProtocol）/ 設定（HwConfig）/ トランスポート（Serial・Loopback）/ COM 列挙 / HardwareSystem。
add_library(Hardware STATIC
    HwProtocol.cpp
    HwConfig.cpp
    HwSerialTransport.cpp
    HwLoopbackTransport.cpp
    HwPortEnum.cpp
    HwToolchain.cpp
    HardwareSystem.cpp
)

find_package(nlohmann_json CONFIG REQUIRED)

target_include_directories(Hardware PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(Hardware PUBLIC Core PRIVATE setupapi.lib nlohmann_json::nlohmann_json)

target_compile_options(Hardware PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Hardware PRIVATE ${DX12_COMPILE_DEFS})

# レーン別の追加ソース（中身は各レーンが書く。OPTIONAL は付けない＝読み忘れを検出するため）
include(${CMAKE_SOURCE_DIR}/cmake/lanes/hardware_lane_a.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/hardware_lane_b.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/hardware_lane_c.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/hardware_lane_d.cmake)
