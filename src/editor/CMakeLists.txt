add_library(Editor STATIC
    EditorLayer.cpp
    UndoSystem.cpp
    UiEditUtil.cpp
    ScenePick.cpp
    ModelThumbnailRenderer.cpp
    EditorIconRenderer.cpp
    panels/ToolbarPanel.cpp
    panels/HierarchyPanel.cpp
    panels/InspectorPanel.cpp
    panels/SceneViewPanel.cpp
    panels/AssetBrowserPanel.cpp
    panels/ConsolePanel.cpp
    panels/McpBridgePanel.cpp
    panels/VfxEditorPanel.cpp
    panels/UiEditorPanel.cpp
    panels/AnimationEditorPanel.cpp
    panels/SpriteSheetEditorPanel.cpp
    panels/NetworkPanel.cpp
    panels/MaterialEditorPanel.cpp
    panels/MaterialLibraryPanel.cpp
    panels/MaterialPreviewRenderer.cpp
    panels/TerrainPanel.cpp
)

target_include_directories(Editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Ui: UiAnimRuntime（.uianim/.spranim のロードと ECS への適用）をアニメ系パネルが使う
target_link_libraries(Editor PUBLIC Core Scene ECS Renderer Gui Resource Audio Physics Ui)

# MCP / AI Bridge パネルの接続コマンド表示用フォールバックパス。
# 通常はパネルが exe を基準に tools/mcp-server を「実行時」解決する（別 PC・フォルダ移動でも有効）。
# これはビルドしたマシンの絶対パスで、その実行時解決が失敗した場合の最終手段としてのみ使う。
target_compile_definitions(Editor PRIVATE MCP_SERVER_DIR="${CMAKE_SOURCE_DIR}/tools/mcp-server")

if(MSVC)
    target_compile_options(Editor PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(Editor PRIVATE ${DX12_COMPILE_DEFS})
endif()

# シーンビューのライティング編集（太陽ドラッグ / ライトのハンドル / ライティング・パネル）。
# 上の add_library のリストへ挿し込まず target_sources で足しているのは、同じファイルを
# 同時に触る作業とぶつからないようにするため（末尾追記だけで済む）。
target_sources(Editor PRIVATE
    LightHandles.cpp
    panels/LightingPanel.cpp
)

# 任意メッシュの頂点スカルプト窓（洞窟・アーチ・岩などの異形。地形ツールの相棒）。
# 上と同じ理由で target_sources の末尾追記にしている。
target_sources(Editor PRIVATE
    panels/SculptPanel.cpp
)
