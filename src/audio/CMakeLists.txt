add_library(Audio STATIC
    AudioClip.cpp
    AudioSystem.cpp
    AudioStream.cpp   # OGG のストリーミング（デコードはワーカースレッド）
)

target_include_directories(Audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(Audio PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

target_link_libraries(Audio PUBLIC Core PRIVATE xaudio2.lib)

target_compile_options(Audio PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Audio PRIVATE ${DX12_COMPILE_DEFS})

# レーン別の追加ソース（中身は各レーンが書く。OPTIONAL は付けない＝読み忘れを検出するため）
include(${CMAKE_SOURCE_DIR}/cmake/lanes/audio_lane_a.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/audio_lane_b.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/audio_lane_c.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/audio_lane_d.cmake)
