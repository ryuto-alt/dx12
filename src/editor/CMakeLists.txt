add_library(Editor STATIC
    EditorLayer.cpp
    UndoSystem.cpp
    ModelThumbnailRenderer.cpp
    EditorIconRenderer.cpp
    panels/ToolbarPanel.cpp
    panels/HierarchyPanel.cpp
    panels/InspectorPanel.cpp
    panels/SceneViewPanel.cpp
    panels/AssetBrowserPanel.cpp
)

target_include_directories(Editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(Editor PUBLIC Core Scene ECS Renderer Gui Resource Audio Physics)

if(MSVC)
    target_compile_options(Editor PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(Editor PRIVATE ${DX12_COMPILE_DEFS})
endif()
