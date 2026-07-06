add_library(Network STATIC
    EnetTransport.cpp
)

target_include_directories(Network PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(unofficial-enet CONFIG REQUIRED)

target_link_libraries(Network
    PUBLIC
        Core
    PRIVATE
        unofficial::enet::enet
        ws2_32.lib
        winmm.lib
)

target_compile_options(Network PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Network PRIVATE ${DX12_COMPILE_DEFS})
