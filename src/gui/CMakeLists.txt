find_package(imgui CONFIG REQUIRED)

add_library(Gui STATIC
    ImGuiManager.cpp
    ImGuizmo.cpp
    UiTestHarness.cpp
    DeepDiagnostics.cpp
)

# ImGuizmo はサードパーティコードなので警告抑制
set_source_files_properties(ImGuizmo.cpp PROPERTIES COMPILE_FLAGS "/W0")

target_include_directories(Gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# UiTestHarness(エンジン診断)は core/Application.h と editor/EditorContext.h を読む＝
# entt / DirectXMath / ecs のヘッダが要る。ECS はそれらを PUBLIC で持つ（Gui は参照しない＝循環しない）。
# DeepDiagnostics(超詳細診断)は TextureLoader/ModelLoader/ShaderRegistry を直接叩くので Resource も要る
# （Resource → Gui の依存は無いので循環しない）。
target_link_libraries(Gui PUBLIC Graphics imgui::imgui ECS PRIVATE Resource)

if(MSVC)
    target_compile_options(Gui PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(Gui PRIVATE ${DX12_COMPILE_DEFS})
endif()
