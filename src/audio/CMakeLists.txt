add_library(Audio STATIC
    AudioClip.cpp
    AudioSystem.cpp
)

target_include_directories(Audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(Audio PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

target_link_libraries(Audio PUBLIC Core PRIVATE xaudio2.lib)

target_compile_options(Audio PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Audio PRIVATE ${DX12_COMPILE_DEFS})
