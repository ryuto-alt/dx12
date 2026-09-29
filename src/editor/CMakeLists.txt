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
    panels/NavMeshPanel.cpp
)

target_include_directories(Editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Ui: UiAnimRuntime（.uianim/.spranim のロードと ECS への適用）をアニメ系パネルが使う
target_link_libraries(Editor PUBLIC Core Scene ECS Renderer Gui Resource Audio Physics Ui Nav AI)

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

# 選択中の Brain（ゲーム AI）のデバッグ表示。ImGui の背景ドローリストへ描くだけ。
target_sources(Editor PRIVATE
    AiDebugOverlay.cpp
)

# 任意メッシュの頂点スカルプト窓（洞窟・アーチ・岩などの異形。地形ツールの相棒）。
# 上と同じ理由で target_sources の末尾追記にしている。
target_sources(Editor PRIVATE
    panels/SculptPanel.cpp
)

# オーディオミキサー窓（バスのメーター/フェーダー・スナップショット・鳴っている音）。
# 上と同じ理由で target_sources の末尾追記にしている。
target_sources(Editor PRIVATE
    panels/AudioMixerPanel.cpp
)

# トランジション専用プレビュー窓（架空のゲーム画面 2 枚 + プリセット全件のサムネイルアトラス）。
# 上と同じ理由で target_sources の末尾追記にしている。
target_sources(Editor PRIVATE
    panels/TransitionPreviewPanel.cpp
)

# エディタ共通の見た目部品（ui:: 名前空間。PropertyGrid.h の pg:: が内部で呼ぶ）。
# 上と同じ理由で target_sources の末尾追記にしている。
target_sources(Editor PRIVATE
    UiWidgets.cpp
)

# 第2波（操作性の基盤）: コマンド表 + ショートカット処理 / コマンドパレット / トースト通知。
# 表そのもの（EditorCommandTable.h / ToolWindows.h / EditorCreateTable.h）と純ロジック
# （FuzzyMatch.h / ToastQueue.h / TransformMath.h）はヘッダオンリー。tests/editor_ux_test.cpp・
# tests/reparent_test.cpp が単体で検証する。
target_sources(Editor PRIVATE
    EditorCommands.cpp
    CommandPalette.cpp
    Toast.cpp
)

# プロジェクトランチャー（UE5 プロジェクトブラウザ風の起動後画面）。純ロジックは project/LauncherLogic.h・LauncherMotion.h。
# 上のリストへ挿し込まず target_sources の末尾追記にしている（同時編集とぶつからないように）。
target_sources(Editor PRIVATE
    LauncherScreen.cpp
)
