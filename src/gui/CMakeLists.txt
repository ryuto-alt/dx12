find_package(imgui CONFIG REQUIRED)

add_library(Gui STATIC
    ImGuiManager.cpp
)

target_include_directories(Gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(Gui PUBLIC Graphics imgui::imgui)

if(MSVC)
    target_compile_options(Gui PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(Gui PRIVATE ${DX12_COMPILE_DEFS})
endif()
