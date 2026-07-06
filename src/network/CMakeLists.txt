add_library(Network STATIC
    EnetTransport.cpp
    NetworkConfig.cpp
    NetworkSystem.cpp
)

target_include_directories(Network PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(unofficial-enet CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(Network
    PUBLIC
        Core
        EnTT::EnTT
    PRIVATE
        unofficial::enet::enet
        nlohmann_json::nlohmann_json
        ws2_32.lib
        winmm.lib
)

target_compile_options(Network PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Network PRIVATE ${DX12_COMPILE_DEFS})
