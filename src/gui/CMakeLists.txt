find_package(imgui CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)

add_library(Gui STATIC
    ImGuiManager.cpp
    ImGuizmo.cpp
    RmlSystem.cpp
    RmlRenderer.cpp
    RmlUIManager.cpp
)

# ImGuizmo はサードパーティコードなので警告抑制
set_source_files_properties(ImGuizmo.cpp PROPERTIES COMPILE_FLAGS "/W0")

target_include_directories(Gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
find_package(directxtex CONFIG REQUIRED)
target_link_libraries(Gui PUBLIC Graphics imgui::imgui RmlUi::RmlUi Microsoft::DirectXTex)

if(MSVC)
    target_compile_options(Gui PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(Gui PRIVATE ${DX12_COMPILE_DEFS})
endif()
