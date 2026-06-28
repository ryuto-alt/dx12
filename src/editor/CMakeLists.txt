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
    panels/McpBridgePanel.cpp
)

target_include_directories(Editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(Editor PUBLIC Core Scene ECS Renderer Gui Resource Audio Physics)

# MCP / AI Bridge パネルが「接続コマンド」を絶対パス込みで表示するためのエンジン側パス。
# game プロジェクトに再ポイントされる AssetsDir と違い、これは常にエンジン repo を指す。
target_compile_definitions(Editor PRIVATE MCP_SERVER_DIR="${CMAKE_SOURCE_DIR}/tools/mcp-server")

if(MSVC)
    target_compile_options(Editor PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(Editor PRIVATE ${DX12_COMPILE_DEFS})
endif()
