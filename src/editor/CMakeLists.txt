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
    panels/ConsolePanel.cpp
    panels/McpBridgePanel.cpp
)

target_include_directories(Editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(Editor PUBLIC Core Scene ECS Renderer Gui Resource Audio Physics)

# MCP / AI Bridge パネルの接続コマンド表示用フォールバックパス。
# 通常はパネルが exe を基準に tools/mcp-server を「実行時」解決する（別 PC・フォルダ移動でも有効）。
# これはビルドしたマシンの絶対パスで、その実行時解決が失敗した場合の最終手段としてのみ使う。
target_compile_definitions(Editor PRIVATE MCP_SERVER_DIR="${CMAKE_SOURCE_DIR}/tools/mcp-server")

if(MSVC)
    target_compile_options(Editor PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(Editor PRIVATE ${DX12_COMPILE_DEFS})
endif()
